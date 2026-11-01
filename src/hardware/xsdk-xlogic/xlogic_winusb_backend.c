/*
 * This file is part of the libsigrok project.
 * Copyright (C) 2026 alambe94
 * SPDX-License-Identifier: GPL-3.0-or-later
 * @file xlogic_winusb_backend.c
 * @brief Windows single-function USB ownership and sigrok session adapter.
 */

#include <config.h>
#include <string.h>
#include "xlogic_winusb_backend.h"
#include "xlogic_winusb_worker.h"

/* Process lifetime, serialized across this driver. The head/count are fixed storage;
 * at most 64 registered owners (active, retained, or closure tombstones) exist.
 * Each node is allocated once at discovery, registered before native Open, and
 * outlives GUI/session teardown when native I/O cannot yet be destroyed.
 * Normal PulseView transfers ownership UI -> sampling thread -> joined UI.
 * Concurrent independent libsigrok sessions/discovery need external process-wide
 * serialization. Native capture is owned by a dedicated worker. */
#define xSDK_xLOGIC_WINUSB_OWNER_LIMIT 64U

struct xsdk_xlogic_winusb_owner
{
    struct xsdk_xlogic_winusb_owner *next;
    xsdk_xlogic_winusb_worker *worker;
    struct sr_dev_inst *sdi;
    GSource *source; /* Borrowed from the main context; no extra reference. */
    gunichar2 *path;
    gunichar2 *serial;
    size_t path_units;
    size_t serial_units;
    uint8_t *decode_buffer;
    size_t decode_buffer_bytes;
    uint64_t frontend_wire_bytes;
    unsigned call_depth;
    gboolean is_bound;
    gboolean is_registered;
    gboolean is_tombstone;
    gboolean is_detached;
    gboolean has_header;
    gboolean has_metadata;
    gboolean has_end;
    gboolean has_adapter_error;
    gboolean has_error_snapshot;
    gboolean was_user_stopped;
};

struct winusb_source
{
    GSource source;
    struct sr_session *session;
    struct xsdk_xlogic_winusb_owner *owner;
    gint64 due;
};

static struct xsdk_xlogic_winusb_owner *owners;
static unsigned owner_count;
static gboolean is_polling_retained;

static gboolean frontend_present(const struct xsdk_xlogic_winusb_owner *owner)
{
    return owner->is_bound && !owner->is_detached && owner->sdi != NULL && (!owner->source || !g_source_is_destroyed(owner->source));
}

static void free_unbound_owner(struct xsdk_xlogic_winusb_owner *owner)
{
    if (owner->is_bound || owner->is_registered || owner->worker || owner->source || owner->call_depth)
    {
        return;
    }
    g_free(owner->path);
    g_free(owner->serial);
    g_free(owner->decode_buffer);
    g_free(owner);
}

static void unregister_owner(struct xsdk_xlogic_winusb_owner *owner)
{
    struct xsdk_xlogic_winusb_owner **link = &owners;

    if (!owner->is_registered)
    {
        return;
    }
    while (*link != owner)
    {
        link = &(*link)->next;
    }
    *link = owner->next;
    owner->next = NULL;
    owner->is_registered = FALSE;
    owner_count--;
}

static gboolean same_identity(const struct xsdk_xlogic_winusb_owner *a, const struct xsdk_xlogic_winusb_owner *b)
{
    if (a->serial_units != b->serial_units)
    {
        return FALSE;
    }
    for (size_t i = 0; i < a->serial_units; ++i)
    {
        if (g_unichar_toupper(a->serial[i]) != g_unichar_toupper(b->serial[i]))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static int register_owner(struct xsdk_xlogic_winusb_owner *owner)
{
    struct xsdk_xlogic_winusb_owner *item;

    if (owner->is_registered || owner_count == xSDK_xLOGIC_WINUSB_OWNER_LIMIT)
    {
        return SR_ERR;
    }
    for (item = owners; item; item = item->next)
    {
        if (same_identity(item, owner))
        {
            sr_err("xLOGIC serial is already owned or retained after uncertain cleanup.");
            return SR_ERR;
        }
    }
    owner->next = owners;
    owners = owner;
    owner->is_registered = TRUE;
    owner_count++;
    return SR_OK;
}

static void report_error(const char *operation, const xLOGIC_Capture_Error_t *error)
{
    sr_err("xLOGIC %s failed: result=%u stage=%u native=%u (present=%u) sink=%u.", operation, (unsigned)error->result,
           (unsigned)error->stage, error->native_error, error->has_native_error, error->sink_error);
}

static void log_capture_snapshot(const struct xsdk_xlogic_winusb_owner *owner, const xLOGIC_Capture_Snapshot_t *snapshot, const char *point)
{
    const xLOGIC_Capture_Cleanup_t *cleanup = &snapshot->cleanup;

    sr_info("xLOGIC snapshot point=%s phase=%u started=%u stopped=%u has_final_status=%u adapter_error=%u has_first_error=%u "
            "has_cleanup_error=%u raw_io=%u queue_depth=%zu transfer_bytes=%zu maximum_transfer_bytes=%zu pending_reads=%zu "
            "completed_reads=%zu submitted_reads=%" G_GUINT64_FORMAT " reaped_wire_bytes=%" G_GUINT64_FORMAT
            " delivered_wire_bytes=%" G_GUINT64_FORMAT " delivered_transfers=%" G_GUINT64_FORMAT
            " io_terminal=%u service_retired=%u can_destroy=%u.",
            point, (unsigned)snapshot->phase, (unsigned)snapshot->has_started, (unsigned)snapshot->has_stopped,
            (unsigned)snapshot->has_final_status, (unsigned)owner->has_adapter_error, (unsigned)snapshot->has_first_error,
            (unsigned)snapshot->has_cleanup_error, (unsigned)snapshot->is_raw_io_enabled, snapshot->queue_depth, snapshot->transfer_bytes,
            snapshot->maximum_transfer_bytes, snapshot->pending_reads, snapshot->completed_reads, snapshot->submitted_reads,
            snapshot->stream_reaped_wire_bytes, snapshot->delivered_wire_bytes, snapshot->delivered_transfers,
            (unsigned)snapshot->io_terminal, (unsigned)snapshot->service_retired, (unsigned)snapshot->can_destroy);
    sr_info("xLOGIC snapshot-timing point=%s acquisition_start_us=%" G_GUINT64_FORMAT " acquisition_end_us=%" G_GUINT64_FORMAT
            " last_read_us=%" G_GUINT64_FORMAT " max_read_gap_us=%" G_GUINT64_FORMAT " max_repost_delay_us=%" G_GUINT64_FORMAT
            " max_renew_duration_us=%" G_GUINT64_FORMAT " renewals=%" G_GUINT64_FORMAT " max_renew_gap_ms=%" G_GUINT64_FORMAT ".",
            point, snapshot->acquisition_start_us, snapshot->acquisition_end_us, snapshot->last_read_us, snapshot->max_read_gap_us,
            snapshot->max_repost_delay_us, snapshot->max_renew_duration_us, snapshot->renewals, snapshot->max_renew_gap_ms);
    sr_info("xLOGIC snapshot-cleanup point=%s started=%u drained=%u pending_reads_at_entry=%" G_GUINT64_FORMAT
            " completed_unconsumed_reads_at_entry=%" G_GUINT64_FORMAT " completed_unconsumed_bytes_at_entry=%" G_GUINT64_FORMAT
            " successful_reads=%" G_GUINT64_FORMAT " successful_bytes=%" G_GUINT64_FORMAT " canceled_reads=%" G_GUINT64_FORMAT
            " failed_reads=%" G_GUINT64_FORMAT " unresolved_reads=%" G_GUINT64_FORMAT ".",
            point, (unsigned)cleanup->has_started, (unsigned)cleanup->is_drained, cleanup->pending_reads_at_entry,
            cleanup->completed_unconsumed_reads_at_entry, cleanup->completed_unconsumed_bytes_at_entry, cleanup->successful_reads,
            cleanup->successful_bytes, cleanup->canceled_reads, cleanup->failed_reads, cleanup->unresolved_reads);
}

static void log_first_error_snapshot(struct xsdk_xlogic_winusb_owner *owner, const xLOGIC_Capture_Snapshot_t *snapshot)
{
    if (owner->has_error_snapshot || !(owner->has_adapter_error || snapshot->has_first_error || snapshot->has_cleanup_error))
    {
        return;
    }
    // Latch before logging; this is the adapter observation, not the engine's error timestamp.
    owner->has_error_snapshot = TRUE;
    log_capture_snapshot(owner, snapshot, "first-error");
}

static int destroy_terminal(struct xsdk_xlogic_winusb_owner *owner, const xsdk_xlogic_worker_status *status)
{
    if (!status->thread_exited || !status->native_disposed || !xsdk_xlogic_worker_destroy(&owner->worker))
    {
        return SR_ERR;
    }
    if (!status->native_retired)
    {
        owner->is_tombstone = TRUE;
        sr_err("xLOGIC cleanup is uncertain; serial remains quarantined for this process.");
        if (status->close_completed && status->close_result != xLOGIC_CAPTURE_OK)
        {
            report_error("native close", &status->close_error);
        }
        return SR_ERR;
    }
    unregister_owner(owner);
    return SR_OK;
}

static void detach_owner(struct xsdk_xlogic_winusb_owner *owner)
{
    owner->is_detached = TRUE;
    owner->sdi = NULL;
    if (owner->worker)
    {
        xsdk_xlogic_worker_detach(owner->worker);
    }
}

static void poll_retained(void)
{
    struct xsdk_xlogic_winusb_owner *owner = owners;

    if (is_polling_retained)
    {
        return;
    }
    is_polling_retained = TRUE;
    while (owner)
    {
        struct xsdk_xlogic_winusb_owner *next = owner->next;
        xsdk_xlogic_worker_status status;

        if (owner->worker && owner->is_detached && !owner->source && owner->call_depth == 0U)
        {
            owner->call_depth++;
            xsdk_xlogic_worker_status_get(owner->worker, &status);
            if (status.has_snapshot)
            {
                log_first_error_snapshot(owner, &status.snapshot);
                if (status.thread_exited && (status.snapshot.has_started || owner->has_error_snapshot))
                {
                    log_capture_snapshot(owner, &status.snapshot, "terminal");
                }
            }
            if (status.thread_exited)
            {
                (void)destroy_terminal(owner, &status); /* Uncertainty stays registered. */
            }
            owner->call_depth--;
            free_unbound_owner(owner);
        }
        owner = next;
    }
    is_polling_retained = FALSE;
}

struct scan_context
{
    GSList *devices;
    unsigned matches;
};

static gunichar2 *copy_utf16(const uint16_t *value, size_t units)
{
    gunichar2 *copy;

    if (units > (SIZE_MAX / sizeof(*copy)) - 1U)
    {
        return NULL;
    }
    copy = g_try_malloc((units + 1U) * sizeof(*copy));
    if (copy)
    {
        memcpy(copy, value, units * sizeof(*copy));
        copy[units] = 0U;
    }
    return copy;
}

static void discovered(void *user, const xLOGIC_Capture_Discovery_Info_t *info)
{
    struct scan_context *scan = user;
    struct xsdk_xlogic_winusb_owner *owner;
    struct dev_context *devc;
    struct sr_dev_inst *sdi;
    struct sr_channel_group *group;
    char *serial;

    scan->matches++;
    if (!info->serial_units || info->serial_units > G_MAXLONG)
    {
        return;
    }
    serial = g_utf16_to_utf8(info->serial, (glong)info->serial_units, NULL, NULL, NULL);
    if (!serial)
    {
        return;
    }
    owner = g_try_malloc0(sizeof(*owner));
    if (!owner)
    {
        g_free(serial);
        return;
    }
    owner->path = copy_utf16(info->device_path, info->device_path_units);
    owner->serial = copy_utf16(info->serial, info->serial_units);
    owner->path_units = info->device_path_units;
    owner->serial_units = info->serial_units;
    devc = xsdk_xlogic_context_new(NULL);
    if (!owner->path || !owner->serial || !devc)
    {
        free_unbound_owner(owner);
        g_free(devc);
        g_free(serial);
        return;
    }
    sdi = g_malloc0(sizeof(*sdi));
    sdi->status = SR_ST_INACTIVE;
    sdi->vendor = g_strdup("xSDK");
    sdi->model = g_strdup("xLOGIC");
    sdi->version = g_strdup("ABI 3 (open to query capabilities)");
    sdi->serial_num = serial;
    sdi->connection_id = g_strdup_printf("winusb/%s", serial);
    sdi->inst_type = SR_INST_USB;
    sdi->priv = devc;
    devc->winusb = owner;
    owner->sdi = sdi;
    owner->is_bound = TRUE;
    group = sr_channel_group_new(sdi, "Logic", NULL);
    for (unsigned i = 0; i < 8U; ++i)
    {
        char name[8];
        struct sr_channel *channel;
        g_snprintf(name, sizeof(name), "D%u", i);
        channel = sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, name);
        group->channels = g_slist_append(group->channels, channel);
    }
    scan->devices = g_slist_append(scan->devices, sdi);
}

static void discard_scan(GSList *devices)
{
    for (GSList *item = devices; item; item = item->next)
    {
        struct sr_dev_inst *sdi = item->data;
        struct dev_context *devc = sdi->priv;
        xsdk_xlogic_winusb_clear(devc);
        g_free(devc);
        sr_dev_inst_free(sdi);
    }
    g_slist_free(devices);
}

SR_PRIV GSList *xsdk_xlogic_winusb_scan(struct sr_dev_driver *di, const char *connection)
{
    struct scan_context scan = {0};
    xLOGIC_Capture_Error_t error = {0};
    xLOGIC_Capture_Result_t result;
    gunichar2 *serial = NULL;
    glong units = 0;

    poll_retained();
    if (connection)
    {
        if (!g_str_has_prefix(connection, "winusb/") || !connection[7])
        {
            sr_err("Windows USB connection must be winusb/<USB serial>.");
            return std_scan_complete(di, NULL);
        }
        serial = g_utf8_to_utf16(connection + 7, -1, NULL, &units, NULL);
        if (!serial)
        {
            return std_scan_complete(di, NULL);
        }
    }
    result = xLOGIC_Capture_WinUSB_Discover(serial, (size_t)units, discovered, &scan, &error);
    g_free(serial);
    if (result != xLOGIC_CAPTURE_OK || (connection && scan.matches != 1U))
    {
        if (result != xLOGIC_CAPTURE_OK)
        {
            report_error("discovery", &error);
        }
        discard_scan(scan.devices);
        scan.devices = NULL;
    }
    if (!connection)
    {
        scan.devices = g_slist_concat(scan.devices, xsdk_xlogic_network_discover());
    }
    return std_scan_complete(di, scan.devices);
}

static int apply_caps(struct dev_context *devc, const xLOGIC_WINUSB_Caps_t *wire)
{
    struct xsdk_xlogic_caps_wire checked;
    memcpy(&checked, wire, sizeof(checked));
    if (!xsdk_xlogic_caps_valid(&checked))
    {
        sr_err("Device returned incompatible xLOGIC capabilities.");
        return SR_ERR;
    }
    const struct xsdk_xlogic_caps caps = {
        .stream_buffer_bytes = wire->stream_buffer_bytes,
        .stream_buffer_count = wire->stream_buffer_count,
        .max_capture_bytes = wire->max_capture_bytes,
        .max_sample_rate = wire->max_sample_rate,
        .max_channels = wire->max_channels,
        .feature_flags = wire->feature_flags,
        .max_uniform_sample_rate = wire->max_uniform_sample_rate,
        .packed_sample_rate = wire->packed_sample_rate,
        .packed_channel_mask = wire->packed_channel_mask,
        .packed_quantum_bytes = wire->packed_quantum_bytes,
        .packed_samples_per_quantum = wire->packed_samples_per_quantum,
    };
    const uint64_t previous = devc->rate_count ? devc->rates[devc->rate_index].actual : 0U;
    int result = xsdk_xlogic_context_set_caps(devc, &caps);

    if (result != SR_OK || !previous)
    {
        return result;
    }
    for (unsigned i = 0; i < devc->rate_count; ++i)
    {
        if (devc->rates[i].actual == previous)
        {
            devc->rate_index = i;
            return SR_OK;
        }
    }
    return SR_ERR_SAMPLERATE;
}

SR_PRIV int xsdk_xlogic_winusb_open(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_winusb_owner *owner = devc->winusb;
    xLOGIC_Capture_Discovery_Info_t selected;
    xsdk_xlogic_worker_status status = {0};
    xLOGIC_WINUSB_Caps_t caps;
    int ret;

    poll_retained();
    if (!owner || owner->worker || owner->source || owner->is_tombstone)
    {
        return SR_ERR;
    }
    ret = register_owner(owner);
    if (ret != SR_OK)
    {
        return ret;
    }
    owner->sdi = sdi;
    owner->is_detached = FALSE;
    selected.device_path = owner->path;
    selected.device_path_units = owner->path_units;
    selected.serial = owner->serial;
    selected.serial_units = owner->serial_units;
    owner->worker = xsdk_xlogic_worker_open(&selected, &caps, &status);
    if (!owner->worker || status.open_result != xLOGIC_CAPTURE_OK)
    {
        if (!owner->worker)
        {
            sr_err("xLOGIC could not create its USB worker.");
        }
        else if (status.has_open_error)
        {
            report_error("open", &status.open_error);
        }
        else
        {
            sr_err("xLOGIC open/capabilities failed (%u).", (unsigned)status.open_result);
        }
        (void)xsdk_xlogic_winusb_close(sdi); /* Failed ownership remains registered. */
        return SR_ERR;
    }
    ret = apply_caps(devc, &caps);
    if (ret == SR_OK)
    {
        /* Packet storage must outlive reentrant device clear throughout
         * sr_session_send, including the remaining datafeed callbacks. */
        g_free(owner->decode_buffer);
        owner->decode_buffer = devc->decode_buffer;
        owner->decode_buffer_bytes = devc->decode_buffer_bytes;
        devc->decode_buffer = NULL;
        devc->decode_buffer_bytes = 0U;
    }
    if (ret != SR_OK)
    {
        (void)xsdk_xlogic_winusb_close(sdi);
    }
    return ret;
}

static void unpack_samples(uint8_t *samples, const uint8_t *data, size_t count, unsigned shift)
{
    size_t i;
    for (i = 0; i < count / 2U; ++i)
    {
        const uint8_t value = data[i];
        samples[2U * i] = (uint8_t)((value & 15U) << shift);
        samples[2U * i + 1U] = (uint8_t)((value >> 4U) << shift);
    }
    if (count & 1U)
    {
        samples[count - 1U] = (uint8_t)((data[i] & 15U) << shift);
    }
}

static xLOGIC_Capture_Sink_Result_t receive_wire(void *user, const uint8_t *data, size_t bytes, uint64_t sequence)
{
    struct xsdk_xlogic_winusb_owner *owner = user;
    struct dev_context *devc;
    const uint8_t *samples = data;
    size_t accepted, consumed;
    gboolean packed, stop;
    int result;
    xLOGIC_Capture_Sink_Result_t reply = {0, xLOGIC_CAPTURE_FLOW_STOP, 0};

    (void)sequence;
    if (!frontend_present(owner) || owner->has_adapter_error)
    {
        return reply;
    }
    devc = owner->sdi->priv;
    packed = devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS;
    accepted = bytes;
    if (packed)
    {
        if (bytes > owner->decode_buffer_bytes / 2U)
        {
            reply.flow = xLOGIC_CAPTURE_FLOW_ERROR;
            reply.error = 1U;
            return reply;
        }
        accepted *= 2U;
    }
    if (!devc->continuous)
    {
        accepted = MIN((uint64_t)accepted, devc->limit_samples - devc->sent_samples);
    }
    consumed = packed ? accepted / 2U + accepted % 2U : accepted;
    if (packed)
    {
        const unsigned shift = devc->channel_mask == xSDK_xLOGIC_PACKED_UPPER_CHANNEL_MASK ? 4U : 0U;
        unpack_samples(owner->decode_buffer, data, accepted, shift);
        samples = owner->decode_buffer;
    }
    devc->sent_samples += accepted;
    stop = !devc->continuous && devc->sent_samples == devc->limit_samples;
    devc->limit_reached = stop;
    reply.bytes_consumed = consumed;
    reply.flow = stop ? xLOGIC_CAPTURE_FLOW_STOP : xLOGIC_CAPTURE_FLOW_CONTINUE;
    if (accepted)
    {
        /* libsigrok's packet field predates const-correct borrowed buffers. */
        const struct sr_datafeed_logic logic = {.length = accepted, .unitsize = 1, .data = (void *)samples};
        const struct sr_datafeed_packet packet = {.type = SR_DF_LOGIC, .payload = &logic};
        result = sr_session_send(owner->sdi, &packet);
        /* The callback may clear the entire device. Only the process-retained
         * owner is safe to inspect after delivery. */
        if (result != SR_OK)
        {
            reply.flow = xLOGIC_CAPTURE_FLOW_ERROR;
            reply.error = 2U;
        }
        else if (!frontend_present(owner))
        {
            reply.flow = xLOGIC_CAPTURE_FLOW_STOP;
        }
    }
    return reply;
}

static int send_metadata(struct xsdk_xlogic_winusb_owner *owner, const xLOGIC_Capture_Snapshot_t *snapshot)
{
    struct sr_config *config;
    struct sr_datafeed_meta meta;
    struct sr_datafeed_packet packet;
    uint64_t rate;
    int result;

    if (owner->has_metadata || !snapshot->has_start_status || !frontend_present(owner))
    {
        return SR_OK;
    }
    rate = snapshot->start_status.actual_sample_rate;
    if (!(snapshot->start_status.status_flags & xSDK_xLOGIC_STATUS_ACTUAL_RATE_VALID) || !rate)
    {
        owner->has_metadata = TRUE;
        sr_warn("xLOGIC actual samplerate is unknown; no measured samplerate metadata is emitted.");
        return SR_OK;
    }
    config = sr_config_new(SR_CONF_SAMPLERATE, g_variant_new_uint64(rate));
    meta.config = g_slist_append(NULL, config);
    packet.type = SR_DF_META;
    packet.payload = &meta;
    owner->has_metadata = TRUE;
    result = sr_session_send(owner->sdi, &packet);
    g_slist_free(meta.config);
    sr_config_free(config);
    return result;
}

static gboolean final_status_valid(struct xsdk_xlogic_winusb_owner *owner, const xLOGIC_Capture_Snapshot_t *snapshot)
{
    const struct dev_context *devc = owner->sdi->priv;
    const struct xsdk_xlogic_rate *rate = &devc->rates[devc->rate_index];
    const xLOGIC_WINUSB_Status_t *status = &snapshot->final_status;

    if (!snapshot->has_final_status || !snapshot->has_started)
    {
        sr_err("xLOGIC has no accepted capture with a fresh terminal status.");
        return FALSE;
    }
    if (!status->configured || status->running || status->requested_sample_rate != rate->request ||
        status->stream_format != rate->stream_format)
    {
        sr_err("xLOGIC terminal status does not match the capture configuration.");
        return FALSE;
    }
    if ((status->status_flags & xSDK_xLOGIC_STATUS_ACTUAL_RATE_VALID) && status->actual_sample_rate != rate->actual)
    {
        sr_err("xLOGIC reported an unexpected actual samplerate.");
        return FALSE;
    }
    /* Accepted START resets acquisition counters, including on standalone
     * ABI-v3 firmware. CONFIGURE/previous-capture counters are not a baseline. */
    if (status->overruns || status->usb_errors || status->ownership_errors || status->timing_errors ||
        (status->status_flags & xSDK_xLOGIC_STATUS_SESSION_FAULT))
    {
        sr_err("xLOGIC reported device-side capture errors.");
        return FALSE;
    }
    return TRUE;
}

static gboolean has_unknown_read_extents(const xLOGIC_Capture_Cleanup_t *cleanup)
{
    return cleanup->canceled_reads || cleanup->failed_reads || cleanup->unresolved_reads;
}

static gboolean final_accounting_valid(struct xsdk_xlogic_winusb_owner *owner, const xLOGIC_Capture_Snapshot_t *snapshot)
{
    const struct dev_context *devc = owner->sdi->priv;
    const xLOGIC_Capture_Cleanup_t *cleanup = &snapshot->cleanup;
    const xLOGIC_WINUSB_Status_t *status = &snapshot->final_status;
    uint64_t successful;
    gboolean partial;

    if (cleanup->successful_bytes > UINT64_MAX - snapshot->stream_reaped_wire_bytes)
    {
        return FALSE;
    }
    successful = snapshot->stream_reaped_wire_bytes + cleanup->successful_bytes;
    partial = has_unknown_read_extents(cleanup);
    if (successful > status->produced_bytes || status->transmitted_bytes > status->produced_bytes)
    {
        sr_err("xLOGIC successful wire reads exceed committed producer data.");
        return FALSE;
    }
    if (!devc->continuous && !devc->limit_reached && !owner->was_user_stopped)
    {
        sr_err("xLOGIC stopped before the requested logical sample limit.");
        return FALSE;
    }
    if (!devc->device_streaming && devc->limit_reached)
    {
        if (successful != devc->sent_samples || status->transmitted_bytes != devc->sent_samples)
        {
            sr_err("xLOGIC finite capture delivery/reap/transmit counts differ.");
            return FALSE;
        }
    }
    else if (!partial && status->transmitted_bytes != successful)
    {
        sr_err("xLOGIC completed wire reads and device transmitted bytes differ.");
        return FALSE;
    }
    if (!owner->was_user_stopped && owner->frontend_wire_bytes != snapshot->delivered_wire_bytes)
    {
        sr_err("xLOGIC frontend did not receive all queued wire data.");
        return FALSE;
    }
    sr_info("xLOGIC wire reaped=%" G_GUINT64_FORMAT " queued=%" G_GUINT64_FORMAT " delivered=%" G_GUINT64_FORMAT
            " logical=%" G_GUINT64_FORMAT " cleanup canceled=%" G_GUINT64_FORMAT " failed=%" G_GUINT64_FORMAT
            " unresolved=%" G_GUINT64_FORMAT " accounting=%s.",
            successful, snapshot->delivered_wire_bytes, owner->frontend_wire_bytes, devc->sent_samples, cleanup->canceled_reads,
            cleanup->failed_reads, cleanup->unresolved_reads, partial ? "partial (failed/canceled extents unknown)" : "exact");
    return TRUE;
}

static void log_capture_statistics(const xLOGIC_Capture_Snapshot_t *snapshot)
{
    uint64_t elapsed;

    if (!snapshot->has_started || !snapshot->has_stopped || snapshot->acquisition_start_us == UINT64_MAX ||
        snapshot->acquisition_end_us == UINT64_MAX || snapshot->acquisition_end_us <= snapshot->acquisition_start_us ||
        (snapshot->has_first_error && snapshot->first_error.result == xLOGIC_CAPTURE_CLOCK_ERROR) ||
        (snapshot->has_cleanup_error && snapshot->cleanup_error.result == xLOGIC_CAPTURE_CLOCK_ERROR))
    {
        sr_info("xLOGIC acquisition throughput unavailable (no valid positive capture interval).");
        return;
    }
    elapsed = snapshot->acquisition_end_us - snapshot->acquisition_start_us;
    sr_info("xLOGIC acquisition elapsed_us=%" G_GUINT64_FORMAT " delivered_wire_bytes=%" G_GUINT64_FORMAT " wire_bytes_per_second=%.3f.",
            elapsed, snapshot->delivered_wire_bytes, (double)snapshot->delivered_wire_bytes * 1000000.0 / (double)elapsed);
}

static void finish_capture(struct xsdk_xlogic_winusb_owner *owner, const xsdk_xlogic_worker_status *status)
{
    const xLOGIC_Capture_Snapshot_t *snapshot = &status->snapshot;
    int result;

    if (status->pool_exhausted)
    {
        sr_err("xLOGIC host capture buffer pool exhausted.");
    }
    sr_info("xLOGIC host buffer pool high_water=%zu capacity=%zu.", status->pool_high_water, status->pool_capacity);
    owner->has_adapter_error |= status->worker_failed || status->pool_exhausted;
    log_first_error_snapshot(owner, snapshot);
    log_capture_snapshot(owner, snapshot, "terminal");
    log_capture_statistics(snapshot);
    if (owner->worker && status->thread_exited)
    {
        result = destroy_terminal(owner, status);
        owner->has_adapter_error |= result != SR_OK;
    }
    if (frontend_present(owner))
    {
        struct dev_context *devc = owner->sdi->priv;
        gboolean is_valid = final_status_valid(owner, snapshot);
        if (is_valid)
        {
            is_valid = final_accounting_valid(owner, snapshot);
        }
        owner->has_adapter_error |= !is_valid;
        devc->failed = owner->has_adapter_error || snapshot->has_first_error || snapshot->has_cleanup_error || !snapshot->service_retired;
        devc->acquisition_state = xSDK_xLOGIC_ACQ_IDLE;
        if (snapshot->has_first_error)
        {
            report_error("capture", &snapshot->first_error);
        }
        if (snapshot->has_cleanup_error)
        {
            report_error("retirement", &snapshot->cleanup_error);
        }
        sr_info("xLOGIC terminal overruns=%u usb_errors=%u ownership_errors=%u timing_errors=%u service_retired=%u io_terminal=%u "
                "native_close_confirmed=%u.",
                snapshot->final_status.overruns, snapshot->final_status.usb_errors, snapshot->final_status.ownership_errors,
                snapshot->final_status.timing_errors, (unsigned)snapshot->service_retired, (unsigned)snapshot->io_terminal,
                (unsigned)(!owner->worker && !owner->is_tombstone));
        if (!owner->has_end && owner->has_header)
        {
            owner->has_end = TRUE;
            result = std_session_send_df_end(owner->sdi);
            if (result != SR_OK)
            {
                sr_err("xLOGIC end delivery failed.");
            }
        }
    }
    if (owner->worker)
    {
        detach_owner(owner);
    }
}

static gboolean tick_owner(struct xsdk_xlogic_winusb_owner *owner)
{
    xsdk_xlogic_worker_status status;
    gboolean keep = TRUE;
    int ret;

    owner->call_depth++;
    xsdk_xlogic_worker_status_get(owner->worker, &status);
    if (status.has_snapshot)
    {
        log_first_error_snapshot(owner, &status.snapshot);
        ret = send_metadata(owner, &status.snapshot);
        if (ret != SR_OK)
        {
            owner->has_adapter_error = TRUE;
            log_first_error_snapshot(owner, &status.snapshot);
            sr_err("xLOGIC samplerate metadata delivery failed.");
            xsdk_xlogic_worker_request_stop(owner->worker, true);
        }
        else if (owner->has_metadata && frontend_present(owner))
        {
            xsdk_xlogic_worker_frontend_ready(owner->worker);
        }
    }
    for (unsigned i = 0; i < 4 && frontend_present(owner) && !owner->has_adapter_error; ++i)
    {
        xsdk_xlogic_wire_handle handle;
        const uint8_t *data;
        size_t bytes;
        uint64_t sequence;
        xLOGIC_Capture_Sink_Result_t delivered;
        if (!xsdk_xlogic_worker_acquire(owner->worker, &handle, &data, &bytes, &sequence))
        {
            break;
        }
        delivered = receive_wire(owner, data, bytes, sequence);
        if (!xsdk_xlogic_worker_release(owner->worker, handle))
        {
            owner->has_adapter_error = TRUE;
            xsdk_xlogic_worker_request_stop(owner->worker, true);
        }
        if (delivered.bytes_consumed > UINT64_MAX - owner->frontend_wire_bytes)
        {
            owner->has_adapter_error = TRUE;
        }
        else
        {
            owner->frontend_wire_bytes += delivered.bytes_consumed;
        }
        if (delivered.flow == xLOGIC_CAPTURE_FLOW_ERROR || owner->has_adapter_error)
        {
            owner->has_adapter_error = TRUE;
            xsdk_xlogic_worker_request_stop(owner->worker, true);
        }
    }
    if (!frontend_present(owner))
    {
        detach_owner(owner);
        keep = FALSE;
    }
    else
    {
        xsdk_xlogic_worker_status_get(owner->worker, &status);
        if ((status.thread_exited || (status.has_snapshot && status.snapshot.phase == xLOGIC_CAPTURE_RETAINED_IO)) &&
            (xsdk_xlogic_worker_queue_drained(owner->worker) || (status.thread_exited && owner->has_adapter_error)))
        {
            if (status.has_snapshot)
            {
                finish_capture(owner, &status);
            }
            else
            {
                owner->has_adapter_error = TRUE;
                detach_owner(owner);
            }
            keep = FALSE;
        }
    }
    owner->call_depth--;
    /* Source finalize owns the last lifetime check after dispatch returns. */
    return keep && frontend_present(owner);
}

static gboolean source_prepare(GSource *base, gint *timeout)
{
    struct winusb_source *source = (struct winusb_source *)base;
    const gint64 remaining = source->due - g_get_monotonic_time();
    *timeout = remaining > 0 ? 1 : 0;
    return remaining <= 0;
}

static gboolean source_check(GSource *base)
{
    return g_get_monotonic_time() >= ((struct winusb_source *)base)->due;
}

static gboolean source_dispatch(GSource *base, GSourceFunc callback, gpointer user)
{
    struct winusb_source *source = (struct winusb_source *)base;
    (void)callback;
    (void)user;
    source->due = g_get_monotonic_time() + 1000;
    return tick_owner(source->owner);
}

static void source_finalize(GSource *base)
{
    struct winusb_source *source = (struct winusb_source *)base;
    struct xsdk_xlogic_winusb_owner *owner = source->owner;
    int result;
    owner->source = NULL;
    /* This is the internal session source contract used by upstream fd_source. */
    result = sr_session_source_destroyed(source->session, owner, base);
    if (result != SR_OK)
    {
        sr_err("xLOGIC session source finalization failed (%d).", result);
    }
    if (owner->worker)
    {
        detach_owner(owner);
    }
    free_unbound_owner(owner);
}

static GSourceFuncs source_functions = {
    .prepare = source_prepare,
    .check = source_check,
    .dispatch = source_dispatch,
    .finalize = source_finalize,
};

static int attach_source(struct xsdk_xlogic_winusb_owner *owner)
{
    struct winusb_source *source = (struct winusb_source *)g_source_new(&source_functions, sizeof(*source));
    int result;

    source->owner = owner;
    source->session = owner->sdi->session;
    source->due = g_get_monotonic_time();
    owner->source = &source->source;
    result = sr_session_source_add_internal(source->session, owner, &source->source);
    g_source_unref(&source->source);
    return result;
}

static int capture_options(struct sr_dev_inst *sdi, xsdk_xlogic_worker_config *options)
{
    struct dev_context *devc = sdi->priv;
    const struct xsdk_xlogic_rate *rate = &devc->rates[devc->rate_index];
    const gboolean packed = rate->stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS;
    gboolean streaming;

    if (!devc->rate_count || !devc->channel_mask ||
        ((devc->caps.feature_flags & xSDK_xLOGIC_CAP_RAW_FULL_CLOCK_ONLY) != 0U && devc->channel_mask != 0xffU))
    {
        return SR_ERR_ARG;
    }
    streaming = packed || devc->continuous || devc->limit_samples > devc->caps.max_capture_bytes;
    if (packed)
    {
        const gboolean selectable = (devc->caps.feature_flags & xSDK_xLOGIC_CAP_PACKED_NIBBLE_SELECT) != 0U;
        const gboolean nibble = devc->channel_mask == 15U || devc->channel_mask == 240U;
        if (!nibble || (!selectable && devc->channel_mask != devc->caps.packed_channel_mask))
        {
            sr_err("xLOGIC packed mode requires exactly one advertised four-channel group enabled.");
            return SR_ERR_ARG;
        }
    }
    memset(options, 0, sizeof(*options));
    options->config.magic = xLOGIC_WINUSB_MAGIC;
    options->config.version = xLOGIC_WINUSB_ABI_VERSION;
    options->config.struct_size = sizeof(options->config);
    options->config.sample_rate = rate->request;
    options->config.sample_count = streaming ? 0U : (uint32_t)devc->limit_samples;
    options->config.channel_mask = devc->channel_mask;
    options->config.stream_format = rate->stream_format;
    options->queue_depth = xSDK_xLOGIC_TRANSFER_COUNT;
    options->transfer_bytes = xSDK_xLOGIC_DIRECT_TRANSFER_BYTES;
    options->pool_depth = xSDK_xLOGIC_TRANSFER_COUNT;
    options->requests_raw_io = TRUE;
    options->wire_limit_bytes = devc->continuous ? 0U : packed ? devc->limit_samples / 2U + devc->limit_samples % 2U : devc->limit_samples;
    return SR_OK;
}

static int begin_frontend(struct xsdk_xlogic_winusb_owner *owner)
{
    xsdk_xlogic_worker_status status;
    int ret;

    owner->has_header = TRUE;
    ret = std_session_send_df_header(owner->sdi);
    if (ret != SR_OK || !frontend_present(owner))
    {
        return SR_ERR;
    }
    xsdk_xlogic_worker_status_get(owner->worker, &status);
    if (!status.has_snapshot)
    {
        return SR_ERR;
    }
    /* An immediate-ready START needs the same metadata-before-payload boundary
     * as START_PENDING. Attach the source last, including nested main loops. */
    ret = send_metadata(owner, &status.snapshot);
    if (ret != SR_OK || !frontend_present(owner))
    {
        owner->has_adapter_error = TRUE;
        log_first_error_snapshot(owner, &status.snapshot);
        return SR_ERR;
    }
    return attach_source(owner);
}

SR_PRIV int xsdk_xlogic_winusb_start(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_winusb_owner *owner = devc->winusb;
    xsdk_xlogic_worker_config options;
    xLOGIC_Capture_Result_t result;
    int ret;

    if (!owner || owner->source || owner->is_tombstone || devc->acquisition_state != xSDK_xLOGIC_ACQ_IDLE)
    {
        return SR_ERR;
    }
    if (owner->worker && owner->is_detached && !owner->call_depth)
    {
        xsdk_xlogic_worker_wait_exit(owner->worker, 1000U);
        poll_retained();
    }
    if (!owner->worker)
    {
        ret = xsdk_xlogic_winusb_open(sdi);
        if (ret != SR_OK)
        {
            return ret;
        }
    }
    ret = capture_options(sdi, &options);
    if (ret != SR_OK)
    {
        return ret;
    }
    owner->call_depth++;
    owner->has_header = FALSE;
    owner->has_metadata = FALSE;
    owner->has_end = FALSE;
    owner->has_adapter_error = FALSE;
    owner->has_error_snapshot = FALSE;
    owner->was_user_stopped = FALSE;
    owner->frontend_wire_bytes = 0U;
    devc->sent_samples = 0U;
    devc->failed = FALSE;
    devc->limit_reached = FALSE;
    devc->device_streaming = options.config.sample_count == 0U;
    result = xsdk_xlogic_worker_start(owner->worker, &options);
    if (result == xLOGIC_CAPTURE_OK)
    {
        devc->acquisition_state = xSDK_xLOGIC_ACQ_RUNNING;
        ret = begin_frontend(owner);
    }
    else
    {
        ret = SR_ERR;
    }
    if (ret != SR_OK || !frontend_present(owner))
    {
        if (owner->is_bound && !owner->is_detached && owner->sdi && owner->sdi->priv)
        {
            struct dev_context *current = owner->sdi->priv;
            current->acquisition_state = xSDK_xLOGIC_ACQ_IDLE;
        }
        detach_owner(owner);
        if (owner->source)
        {
            g_source_destroy(owner->source);
        }
        ret = SR_ERR;
    }
    owner->call_depth--;
    if (ret != SR_OK && owner->worker && !owner->source && !owner->call_depth)
    {
        xsdk_xlogic_worker_wait_exit(owner->worker, 1000U);
    }
    free_unbound_owner(owner);
    poll_retained();
    return ret;
}

SR_PRIV int xsdk_xlogic_winusb_stop(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_winusb_owner *owner = devc->winusb;

    /* libsigrok stops every device, including one whose start was rejected. */
    if (!owner || !owner->worker || devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
    {
        return SR_OK;
    }
    devc->acquisition_state = xSDK_xLOGIC_ACQ_STOPPING;
    owner->was_user_stopped = TRUE;
    xsdk_xlogic_worker_request_stop(owner->worker, true);
    return SR_OK;
}

SR_PRIV int xsdk_xlogic_winusb_close(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_winusb_owner *owner = devc->winusb;
    xsdk_xlogic_worker_status status;
    int ret = SR_OK;

    if (!owner)
    {
        return SR_OK;
    }
    owner->call_depth++;
    detach_owner(owner);
    if (owner->source)
    {
        g_source_destroy(owner->source);
    }
    if (owner->worker)
    {
        if (devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE && !owner->source && owner->call_depth == 1U)
        {
            xsdk_xlogic_worker_wait_exit(owner->worker, 1000U);
        }
        xsdk_xlogic_worker_status_get(owner->worker, &status);
        if (status.thread_exited)
        {
            ret = destroy_terminal(owner, &status);
        }
        else
        {
            ret = SR_ERR;
        }
    }
    else if (!owner->worker && !owner->is_tombstone)
    {
        unregister_owner(owner);
    }
    else
    {
        ret = SR_ERR;
    }
    devc->acquisition_state = xSDK_xLOGIC_ACQ_IDLE;
    owner->call_depth--;
    poll_retained();
    return ret;
}

SR_PRIV void xsdk_xlogic_winusb_clear(struct dev_context *devc)
{
    struct xsdk_xlogic_winusb_owner *owner = devc->winusb;

    if (!owner)
    {
        return;
    }
    owner->call_depth++;
    devc->winusb = NULL;
    owner->is_bound = FALSE;
    detach_owner(owner);
    if (owner->source)
    {
        g_source_destroy(owner->source);
    }
    owner->call_depth--;
    free_unbound_owner(owner);
    poll_retained();
}

SR_PRIV int xsdk_xlogic_winusb_prepare_clear(const struct sr_dev_driver *di)
{
    const struct drv_context *drvc = di->context;

    /* Clear bindings before generic libsigrok frees dev_context or drv_context.
     * Retained registry nodes never point back to those shorter-lived objects. */
    for (GSList *item = drvc->instances; item; item = item->next)
    {
        struct sr_dev_inst *sdi = item->data;
        struct dev_context *devc = sdi->priv;
        if (devc->winusb)
        {
            xsdk_xlogic_winusb_clear(devc);
        }
    }
    return SR_OK;
}
