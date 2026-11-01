/* Copyright (C) 2026 alambe94. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef xSDK_xLOGIC_WINUSB_WORKER_H
#define xSDK_xLOGIC_WINUSB_WORKER_H

#include <glib.h>
#include "xlogic_capture_winusb.h"
#include "xlogic_wire_pool.h"

/* The worker owns all native capture calls. Exactly one session thread owns
 * sigrok objects and borrows queued blocks until release; the worker has no
 * sigrok pointers. Status and commands are synchronized by the worker mutex.
 * A session source must poll status and the queue; a worker wait is bounded. */
typedef struct xsdk_xlogic_winusb_worker xsdk_xlogic_winusb_worker;

typedef struct
{
    xLOGIC_WINUSB_Config_t config;
    size_t queue_depth;
    size_t transfer_bytes;
    size_t pool_depth;
    bool requests_raw_io;
    uint64_t wire_limit_bytes;
} xsdk_xlogic_worker_config;

typedef struct
{
    xLOGIC_Capture_Result_t open_result;
    xLOGIC_Capture_Result_t start_result;
    xLOGIC_Capture_Result_t close_result;
    xLOGIC_Capture_Error_t open_error;
    xLOGIC_Capture_Error_t close_error;
    xLOGIC_Capture_Snapshot_t snapshot;
    size_t pool_capacity;
    size_t pool_high_water;
    bool has_open_error; /* Open error only; GetCaps has no error-detail output. */
    bool open_completed;
    bool start_completed;
    bool close_completed;
    bool has_snapshot;
    bool thread_exited;
    bool native_disposed;
    bool native_retired;
    bool worker_failed;
    bool pool_exhausted;
} xsdk_xlogic_worker_status;

/* Waits for Open/GetCaps. Copies discovery strings. NULL means no worker or
 * native operation was started. On failure a non-NULL worker may own native
 * resources; retain it until locally disposable, and quarantine its identity
 * if native retirement cannot be confirmed. */
xsdk_xlogic_winusb_worker *
xsdk_xlogic_worker_open(const xLOGIC_Capture_Discovery_Info_t *selected, xLOGIC_WINUSB_Caps_t *caps, xsdk_xlogic_worker_status *status);
/* One native attempt per worker. Preflight allocation failure is retryable
 * because no native operation was issued. Once submitted, waits for
 * Prepare/Start; a failure still requires detach and safe retirement. */
xLOGIC_Capture_Result_t xsdk_xlogic_worker_start(xsdk_xlogic_winusb_worker *worker, const xsdk_xlogic_worker_config *config);
void xsdk_xlogic_worker_status_get(xsdk_xlogic_winusb_worker *worker, xsdk_xlogic_worker_status *status);
/* Bounded wait for non-reentrant open/start/close paths; never use from a
 * datafeed callback or while holding a borrowed block. */
void xsdk_xlogic_worker_wait_exit(xsdk_xlogic_winusb_worker *worker, unsigned timeout_ms);
/* Until HEADER and rate metadata are delivered, acquire returns false. A
 * metadata failure must request stop/discard rather than acknowledging ready. */
void xsdk_xlogic_worker_frontend_ready(xsdk_xlogic_winusb_worker *worker);
bool xsdk_xlogic_worker_acquire(xsdk_xlogic_winusb_worker *worker,
                                xsdk_xlogic_wire_handle *handle,
                                const uint8_t **data,
                                size_t *bytes,
                                uint64_t *sequence);
bool xsdk_xlogic_worker_release(xsdk_xlogic_winusb_worker *worker, xsdk_xlogic_wire_handle handle);
bool xsdk_xlogic_worker_queue_drained(xsdk_xlogic_winusb_worker *worker);
/* Stop is an intent and is a no-op before start. Discard closes admission and
 * drops queued data, but never invalidates an acquired block. A later discard
 * can upgrade an earlier stop. */
void xsdk_xlogic_worker_request_stop(xsdk_xlogic_winusb_worker *worker, bool discard);
/* Permanently closes payload admission. Retained I/O stays worker-owned and is
 * polled until safe retirement or explicitly quarantined on worker failure. */
void xsdk_xlogic_worker_detach(xsdk_xlogic_winusb_worker *worker);
/* Leaves pointer intact unless the worker exited, the core was disposed and all
 * pool borrows returned. The caller must quarantine an uncertain serial even
 * after local storage is freed. Never wait for a running worker in a callback. */
bool xsdk_xlogic_worker_destroy(xsdk_xlogic_winusb_worker **worker);

#endif
