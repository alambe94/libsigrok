/* Copyright (C) 2026 alambe94. SPDX-License-Identifier: GPL-3.0-or-later */
#include "xlogic_winusb_worker.h"

#include <string.h>
#include <windows.h>

/* A detached worker may outlive libsigrok's driver cleanup while native I/O
 * retires. Keep its code and normal loader-managed dependencies resident for
 * the process lifetime; worker state itself still follows its own lifetime. */
static bool pin_worker_module(void)
{
    static const char anchor = 0;
    static gsize pin_result;
    if (g_once_init_enter(&pin_result))
    {
        HMODULE module;
        const bool pinned =
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&anchor, &module) != 0;
        g_once_init_leave(&pin_result, pinned ? 1 : 2);
    }
    return pin_result == 1;
}

struct xsdk_xlogic_winusb_worker
{
    GMutex lock;
    GCond changed;
    GThread *thread;
    xLOGIC_Capture_Discovery_Info_t selected;
    uint16_t *path;
    uint16_t *serial;
    xLOGIC_Capture_WinUSB_Handle_t *capture; /* worker thread only */
    xLOGIC_WINUSB_Caps_t caps;
    xLOGIC_Capture_Prepare_t options;
    xsdk_xlogic_worker_status status;
    xsdk_xlogic_wire_pool pool;
    xsdk_xlogic_wire_block *blocks;
    size_t *fifo;
    uint8_t *storage;
    uint64_t enqueued_bytes;
    bool open_done;
    bool start_requested;
    bool start_done;
    bool stop_requested;
    bool detach_requested;
    bool discard_requested;
    bool frontend_ready;
    bool pool_ready;
};

static void publish_snapshot(xsdk_xlogic_winusb_worker *worker)
{
    xLOGIC_Capture_Snapshot_t snapshot;
    if (worker->capture && xLOGIC_Capture_WinUSB_Snapshot(worker->capture, &snapshot) == xLOGIC_CAPTURE_OK)
    {
        g_mutex_lock(&worker->lock);
        worker->status.snapshot = snapshot;
        worker->status.has_snapshot = true;
        g_cond_broadcast(&worker->changed);
        g_mutex_unlock(&worker->lock);
    }
}

static xLOGIC_Capture_Sink_Result_t enqueue_wire(void *user, const uint8_t *data, size_t bytes, uint64_t sequence)
{
    xsdk_xlogic_winusb_worker *worker = user;
    xsdk_xlogic_wire_handle handle;
    xLOGIC_Capture_Sink_Result_t result = {0, xLOGIC_CAPTURE_FLOW_ERROR, 1};
    uint8_t *target;
    bool reserved, published, stopping;

    g_mutex_lock(&worker->lock);
    stopping = worker->discard_requested || worker->detach_requested;
    reserved = !stopping && xsdk_xlogic_wire_pool_reserve(&worker->pool, &handle, &target);
    if (reserved)
    {
        size_t occupied = 0;
        for (size_t i = 0; i < worker->pool.capacity; ++i)
        {
            occupied += worker->pool.blocks[i].state != xSDK_xLOGIC_BLOCK_FREE;
        }
        if (occupied > worker->status.pool_high_water)
        {
            worker->status.pool_high_water = occupied;
        }
    }
    if (!reserved && !stopping)
    {
        worker->status.pool_exhausted = true;
    }
    g_mutex_unlock(&worker->lock);
    if (!reserved)
    {
        if (stopping)
        {
            result.flow = xLOGIC_CAPTURE_FLOW_STOP;
            result.error = 0;
        }
        return result;
    }
    if (!data || !bytes || bytes > worker->options.transfer_bytes || bytes > UINT64_MAX - worker->enqueued_bytes)
    {
        g_mutex_lock(&worker->lock);
        (void)xsdk_xlogic_wire_pool_release(&worker->pool, handle);
        g_mutex_unlock(&worker->lock);
        return result;
    }
    memcpy(target, data, bytes);
    g_mutex_lock(&worker->lock);
    published = xsdk_xlogic_wire_pool_publish(&worker->pool, handle, bytes, sequence);
    if (published)
    {
        worker->enqueued_bytes += bytes;
        result.bytes_consumed = bytes;
        result.error = 0;
        result.flow = worker->options.wire_limit_bytes && worker->enqueued_bytes >= worker->options.wire_limit_bytes
                          ? xLOGIC_CAPTURE_FLOW_STOP
                          : xLOGIC_CAPTURE_FLOW_CONTINUE;
        g_cond_broadcast(&worker->changed);
    }
    else if (worker->discard_requested || worker->detach_requested)
    {
        result.flow = xLOGIC_CAPTURE_FLOW_STOP;
        result.error = 0;
    }
    g_mutex_unlock(&worker->lock);
    return result;
}

static bool finish_native(xsdk_xlogic_winusb_worker *worker)
{
    xLOGIC_Capture_Snapshot_t snapshot;
    xLOGIC_Capture_Error_t error = {0};
    xLOGIC_Capture_Result_t result;
    if (!worker->capture)
    {
        return true;
    }
    if (xLOGIC_Capture_WinUSB_Snapshot(worker->capture, &snapshot) != xLOGIC_CAPTURE_OK || !snapshot.can_destroy)
    {
        return false;
    }
    result = xLOGIC_Capture_WinUSB_Destroy(&worker->capture, &error);
    g_mutex_lock(&worker->lock);
    worker->status.snapshot = snapshot;
    worker->status.has_snapshot = true;
    worker->status.close_result = result;
    worker->status.close_error = error;
    worker->status.close_completed = true;
    worker->status.native_disposed = worker->capture == NULL;
    worker->status.native_retired = worker->capture == NULL && result == xLOGIC_CAPTURE_OK && !snapshot.has_unconfirmed_control &&
                                    (!(snapshot.has_started || snapshot.connection_id) || snapshot.service_retired);
    g_mutex_unlock(&worker->lock);
    return worker->capture == NULL;
}

static void run_native(xsdk_xlogic_winusb_worker *worker)
{
    bool detached = false;
    bool wait_broken = false;
    for (;;)
    {
        bool stop, detach, ready;
        xLOGIC_Capture_Result_t command_result = xLOGIC_CAPTURE_OK;
        xLOGIC_Capture_Snapshot_t snapshot;
        g_mutex_lock(&worker->lock);
        stop = worker->stop_requested;
        detach = worker->detach_requested;
        worker->stop_requested = false;
        g_mutex_unlock(&worker->lock);
        if (detach && !detached)
        {
            command_result = xLOGIC_Capture_WinUSB_Detach(worker->capture);
            detached = command_result == xLOGIC_CAPTURE_OK;
        }
        else if (stop && !detached)
        {
            command_result = xLOGIC_Capture_WinUSB_Request_Stop(worker->capture);
        }
        if (command_result != xLOGIC_CAPTURE_OK)
        {
            g_mutex_lock(&worker->lock);
            worker->status.worker_failed = true;
            g_mutex_unlock(&worker->lock);
            break; /* Retain native ownership; detach/stop is unconfirmed. */
        }
        if (xLOGIC_Capture_WinUSB_Snapshot(worker->capture, &snapshot) != xLOGIC_CAPTURE_OK)
        {
            g_mutex_lock(&worker->lock);
            worker->status.worker_failed = true;
            g_mutex_unlock(&worker->lock);
            break;
        }
        publish_snapshot(worker);
        if (snapshot.can_destroy && finish_native(worker))
        {
            return;
        }
        if (xLOGIC_Capture_WinUSB_Step(worker->capture) != xLOGIC_CAPTURE_OK)
        {
            g_mutex_lock(&worker->lock);
            worker->status.worker_failed = true;
            g_mutex_unlock(&worker->lock);
            break;
        }
        publish_snapshot(worker);
        if (!wait_broken && xLOGIC_Capture_WinUSB_Wait(worker->capture, 10, &ready) != xLOGIC_CAPTURE_OK)
        {
            g_mutex_lock(&worker->lock);
            worker->status.worker_failed = true;
            worker->detach_requested = true;
            worker->discard_requested = true;
            if (worker->pool_ready)
            {
                xsdk_xlogic_wire_pool_close(&worker->pool);
            }
            g_mutex_unlock(&worker->lock);
            wait_broken = true; /* Step still performs nonblocking retirement. */
        }
        if (wait_broken)
        {
            g_mutex_lock(&worker->lock);
            (void)g_cond_wait_until(&worker->changed, &worker->lock, g_get_monotonic_time() + 1000);
            g_mutex_unlock(&worker->lock);
        }
    }
}

static gpointer worker_main(gpointer user)
{
    xsdk_xlogic_winusb_worker *worker = user;
    xLOGIC_Capture_Error_t error = {0};
    xLOGIC_Capture_Result_t result;
    bool has_open_error;
    bool start, detach;
    result = xLOGIC_Capture_WinUSB_Open(&worker->selected, &worker->capture, &error);
    has_open_error = result != xLOGIC_CAPTURE_OK && error.result != xLOGIC_CAPTURE_OK;
    if (result == xLOGIC_CAPTURE_OK)
    {
        result = xLOGIC_Capture_WinUSB_Get_Caps(worker->capture, &worker->caps);
    }
    g_mutex_lock(&worker->lock);
    worker->status.open_result = result;
    worker->status.open_error = error;
    worker->status.has_open_error = has_open_error;
    worker->status.open_completed = true;
    worker->open_done = true;
    g_cond_broadcast(&worker->changed);
    while (result == xLOGIC_CAPTURE_OK && !worker->start_requested && !worker->detach_requested)
    {
        g_cond_wait(&worker->changed, &worker->lock);
    }
    start = worker->start_requested;
    detach = worker->detach_requested || result != xLOGIC_CAPTURE_OK;
    g_mutex_unlock(&worker->lock);
    if (start && !detach)
    {
        result = xLOGIC_Capture_WinUSB_Prepare(worker->capture, &worker->options);
        if (result == xLOGIC_CAPTURE_OK)
        {
            result = xLOGIC_Capture_WinUSB_Start(worker->capture);
        }
        publish_snapshot(worker);
        g_mutex_lock(&worker->lock);
        worker->status.start_result = result;
        worker->status.start_completed = true;
        worker->start_done = true;
        if (result != xLOGIC_CAPTURE_OK)
        {
            worker->detach_requested = true;
        }
        g_cond_broadcast(&worker->changed);
        g_mutex_unlock(&worker->lock);
    }
    if (worker->capture)
    {
        if (detach)
        {
            g_mutex_lock(&worker->lock);
            worker->detach_requested = true;
            g_mutex_unlock(&worker->lock);
        }
        run_native(worker);
    }
    g_mutex_lock(&worker->lock);
    worker->status.thread_exited = true;
    if (!worker->capture && !worker->status.close_completed)
    {
        worker->status.close_completed = true;
        worker->status.native_disposed = true;
        worker->status.native_retired = true;
    }
    worker->start_done = true;
    g_cond_broadcast(&worker->changed);
    g_mutex_unlock(&worker->lock);
    return NULL;
}

static void free_worker_storage(xsdk_xlogic_winusb_worker *worker)
{
    g_free(worker->storage);
    g_free(worker->blocks);
    g_free(worker->fifo);
    g_free(worker->path);
    g_free(worker->serial);
    g_cond_clear(&worker->changed);
    g_mutex_clear(&worker->lock);
    g_free(worker);
}

xsdk_xlogic_winusb_worker *
xsdk_xlogic_worker_open(const xLOGIC_Capture_Discovery_Info_t *selected, xLOGIC_WINUSB_Caps_t *caps, xsdk_xlogic_worker_status *status)
{
    xsdk_xlogic_winusb_worker *worker;
    GError *error = NULL;
    if (!selected || !caps || !status || !selected->device_path || !selected->serial || !selected->device_path_units ||
        !selected->serial_units || selected->device_path_units > SIZE_MAX / sizeof(uint16_t) ||
        selected->serial_units > SIZE_MAX / sizeof(uint16_t))
    {
        return NULL;
    }
    if (!pin_worker_module())
    {
        return NULL;
    }
    worker = g_try_new0(xsdk_xlogic_winusb_worker, 1);
    if (!worker)
    {
        return NULL;
    }
    g_mutex_init(&worker->lock);
    g_cond_init(&worker->changed);
    worker->path = g_try_malloc_n(selected->device_path_units, sizeof(uint16_t));
    worker->serial = g_try_malloc_n(selected->serial_units, sizeof(uint16_t));
    if (!worker->path || !worker->serial)
    {
        free_worker_storage(worker);
        return NULL;
    }
    memcpy(worker->path, selected->device_path, selected->device_path_units * sizeof(uint16_t));
    memcpy(worker->serial, selected->serial, selected->serial_units * sizeof(uint16_t));
    worker->selected = (xLOGIC_Capture_Discovery_Info_t){worker->path, selected->device_path_units, worker->serial, selected->serial_units};
    worker->thread = g_thread_try_new("xlogic-usb", worker_main, worker, &error);
    if (!worker->thread)
    {
        g_clear_error(&error);
        free_worker_storage(worker);
        return NULL;
    }
    g_mutex_lock(&worker->lock);
    while (!worker->open_done)
    {
        g_cond_wait(&worker->changed, &worker->lock);
    }
    *status = worker->status;
    if (status->open_result == xLOGIC_CAPTURE_OK)
    {
        *caps = worker->caps;
    }
    g_mutex_unlock(&worker->lock);
    return worker;
}

xLOGIC_Capture_Result_t xsdk_xlogic_worker_start(xsdk_xlogic_winusb_worker *worker, const xsdk_xlogic_worker_config *config)
{
    xLOGIC_Capture_Result_t result;
    if (!worker || !config || !config->queue_depth || config->queue_depth > 64 || !config->pool_depth || config->pool_depth > 64 ||
        !config->transfer_bytes || config->transfer_bytes > UINT32_MAX || config->pool_depth > SIZE_MAX / config->transfer_bytes)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    g_mutex_lock(&worker->lock);
    if (worker->start_requested || worker->detach_requested || worker->status.thread_exited ||
        worker->status.open_result != xLOGIC_CAPTURE_OK)
    {
        g_mutex_unlock(&worker->lock);
        return xLOGIC_CAPTURE_INVALID_STATE;
    }
    worker->blocks = g_try_new0(xsdk_xlogic_wire_block, config->pool_depth);
    worker->fifo = g_try_new0(size_t, config->pool_depth);
    worker->storage = g_try_malloc_n(config->pool_depth, config->transfer_bytes);
    if (!worker->blocks || !worker->fifo || !worker->storage ||
        !xsdk_xlogic_wire_pool_init(&worker->pool, worker->storage, config->pool_depth * config->transfer_bytes, config->transfer_bytes,
                                    worker->blocks, worker->fifo, config->pool_depth))
    {
        g_clear_pointer(&worker->blocks, g_free);
        g_clear_pointer(&worker->fifo, g_free);
        g_clear_pointer(&worker->storage, g_free);
        g_mutex_unlock(&worker->lock);
        return xLOGIC_CAPTURE_NO_MEMORY;
    }
    worker->pool_ready = true;
    worker->status.pool_capacity = config->pool_depth;
    worker->options = (xLOGIC_Capture_Prepare_t){0};
    worker->options.config = config->config;
    worker->options.queue_depth = config->queue_depth;
    worker->options.transfer_bytes = config->transfer_bytes;
    worker->options.requests_raw_io = config->requests_raw_io;
    worker->options.wire_limit_bytes = config->wire_limit_bytes;
    worker->options.sink = enqueue_wire;
    worker->options.sink_user = worker;
    worker->start_requested = true;
    g_cond_broadcast(&worker->changed);
    while (!worker->start_done)
    {
        g_cond_wait(&worker->changed, &worker->lock);
    }
    result = worker->status.start_completed ? worker->status.start_result : xLOGIC_CAPTURE_INVALID_STATE;
    g_mutex_unlock(&worker->lock);
    return result;
}

void xsdk_xlogic_worker_status_get(xsdk_xlogic_winusb_worker *worker, xsdk_xlogic_worker_status *status)
{
    if (!worker || !status)
    {
        return;
    }
    g_mutex_lock(&worker->lock);
    *status = worker->status;
    g_mutex_unlock(&worker->lock);
}

void xsdk_xlogic_worker_wait_exit(xsdk_xlogic_winusb_worker *worker, unsigned timeout_ms)
{
    gint64 deadline;
    if (!worker)
    {
        return;
    }
    deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    g_mutex_lock(&worker->lock);
    while (!worker->status.thread_exited && g_cond_wait_until(&worker->changed, &worker->lock, deadline))
    {
    }
    g_mutex_unlock(&worker->lock);
}

void xsdk_xlogic_worker_frontend_ready(xsdk_xlogic_winusb_worker *worker)
{
    if (!worker)
    {
        return;
    }
    g_mutex_lock(&worker->lock);
    if (!worker->discard_requested && !worker->detach_requested)
    {
        worker->frontend_ready = true;
    }
    g_mutex_unlock(&worker->lock);
}

bool xsdk_xlogic_worker_acquire(xsdk_xlogic_winusb_worker *worker,
                                xsdk_xlogic_wire_handle *handle,
                                const uint8_t **data,
                                size_t *bytes,
                                uint64_t *sequence)
{
    bool result;
    if (!worker)
    {
        return false;
    }
    g_mutex_lock(&worker->lock);
    result = worker->pool_ready && worker->frontend_ready && xsdk_xlogic_wire_pool_acquire(&worker->pool, handle, data, bytes, sequence);
    g_mutex_unlock(&worker->lock);
    return result;
}

bool xsdk_xlogic_worker_release(xsdk_xlogic_winusb_worker *worker, xsdk_xlogic_wire_handle handle)
{
    bool result;
    if (!worker)
    {
        return false;
    }
    g_mutex_lock(&worker->lock);
    result = worker->pool_ready && xsdk_xlogic_wire_pool_release(&worker->pool, handle);
    g_mutex_unlock(&worker->lock);
    return result;
}

bool xsdk_xlogic_worker_queue_drained(xsdk_xlogic_winusb_worker *worker)
{
    bool result;
    if (!worker)
    {
        return false;
    }
    g_mutex_lock(&worker->lock);
    result = !worker->pool_ready || xsdk_xlogic_wire_pool_is_drained(&worker->pool);
    g_mutex_unlock(&worker->lock);
    return result;
}

void xsdk_xlogic_worker_request_stop(xsdk_xlogic_winusb_worker *worker, bool discard)
{
    if (!worker)
    {
        return;
    }
    g_mutex_lock(&worker->lock);
    if (!worker->start_requested)
    {
        g_mutex_unlock(&worker->lock);
        return;
    }
    worker->stop_requested = true;
    if (discard)
    {
        worker->discard_requested = true;
        if (worker->pool_ready)
        {
            xsdk_xlogic_wire_pool_close(&worker->pool);
        }
    }
    g_cond_broadcast(&worker->changed);
    g_mutex_unlock(&worker->lock);
}

void xsdk_xlogic_worker_detach(xsdk_xlogic_winusb_worker *worker)
{
    if (!worker)
    {
        return;
    }
    g_mutex_lock(&worker->lock);
    worker->detach_requested = true;
    worker->discard_requested = true;
    if (worker->pool_ready)
    {
        xsdk_xlogic_wire_pool_close(&worker->pool);
    }
    g_cond_broadcast(&worker->changed);
    g_mutex_unlock(&worker->lock);
}

bool xsdk_xlogic_worker_destroy(xsdk_xlogic_winusb_worker **handle)
{
    xsdk_xlogic_winusb_worker *worker;
    bool safe;
    if (!handle || !*handle)
    {
        return false;
    }
    worker = *handle;
    g_mutex_lock(&worker->lock);
    safe = worker->status.thread_exited && worker->status.native_disposed &&
           (!worker->pool_ready || xsdk_xlogic_wire_pool_is_drained(&worker->pool));
    g_mutex_unlock(&worker->lock);
    if (!safe)
    {
        return false;
    }
    g_thread_join(worker->thread);
    *handle = NULL;
    free_worker_storage(worker);
    return true;
}
