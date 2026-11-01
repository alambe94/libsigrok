// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0
// Private shared Windows acquisition API; this owner API is not a frozen/public ABI.
#ifndef xLOGIC_CAPTURE_WINUSB_H
#define xLOGIC_CAPTURE_WINUSB_H

#define xLOGIC_CAPTURE_FINISH_TIMEOUT_DEFAULT_MS 5000U
#define xLOGIC_CAPTURE_FINISH_TIMEOUT_MAX_MS     60000U

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xlogic_winusb_protocol.h"
#include "xlogic_xesk_host.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // All calls are serialized on one owner thread. The sole reentrant call is
    // Request_Stop from a sink/observer. No other API, including Snapshot, may be
    // called from a callback. No Win32 handle or OVERLAPPED crosses this boundary.
    typedef struct xLOGIC_Capture_WinUSB_Context xLOGIC_Capture_WinUSB_Handle_t;

    typedef enum
    {
        xLOGIC_CAPTURE_OK = 0,
        xLOGIC_CAPTURE_INVALID_ARGUMENT,
        xLOGIC_CAPTURE_INVALID_STATE,
        xLOGIC_CAPTURE_BUSY,
        xLOGIC_CAPTURE_NO_MEMORY,
        xLOGIC_CAPTURE_NOT_FOUND,
        xLOGIC_CAPTURE_AMBIGUOUS,
        xLOGIC_CAPTURE_NATIVE_ERROR,
        xLOGIC_CAPTURE_PROTOCOL_ERROR,
        xLOGIC_CAPTURE_SINK_ERROR,
        xLOGIC_CAPTURE_CLOCK_ERROR,
        xLOGIC_CAPTURE_QUARANTINED
    } xLOGIC_Capture_Result_t;

    typedef enum
    {
        xLOGIC_CAPTURE_OPEN,
        xLOGIC_CAPTURE_PREPARED,
        xLOGIC_CAPTURE_STARTING,
        xLOGIC_CAPTURE_RUNNING,
        xLOGIC_CAPTURE_STOPPING,
        xLOGIC_CAPTURE_DRAINING,
        xLOGIC_CAPTURE_TERMINAL,
        xLOGIC_CAPTURE_RETAINED_IO,
        xLOGIC_CAPTURE_FINISHING
    } xLOGIC_Capture_Phase_t;

    typedef enum
    {
        xLOGIC_CAPTURE_STAGE_NONE,
        xLOGIC_CAPTURE_STAGE_DISCOVER,
        xLOGIC_CAPTURE_STAGE_OPEN,
        xLOGIC_CAPTURE_STAGE_PREPARE,
        xLOGIC_CAPTURE_STAGE_CONFIGURE,
        xLOGIC_CAPTURE_STAGE_START,
        xLOGIC_CAPTURE_STAGE_SUBMIT,
        xLOGIC_CAPTURE_STAGE_REAP,
        xLOGIC_CAPTURE_STAGE_SINK,
        xLOGIC_CAPTURE_STAGE_RENEW,
        xLOGIC_CAPTURE_STAGE_STOP,
        xLOGIC_CAPTURE_STAGE_ABORT,
        xLOGIC_CAPTURE_STAGE_DRAIN,
        xLOGIC_CAPTURE_STAGE_RELEASE,
        xLOGIC_CAPTURE_STAGE_CLOSE,
        xLOGIC_CAPTURE_STAGE_IDLE_CONTROL,
        xLOGIC_CAPTURE_STAGE_FINISH
    } xLOGIC_Capture_Stage_t;

    typedef struct
    {
        xLOGIC_Capture_Result_t result;
        xLOGIC_Capture_Stage_t stage;
        uint32_t native_error;
        bool has_native_error;
        uint32_t sink_error;
        uint64_t read_sequence;
        uint64_t requested_bytes;
        uint64_t actual_bytes;
        bool has_read_sequence;
        bool has_actual_bytes; // false for failed read extents; never infer zero.
        uint8_t request;
        uint8_t opcode;
        xLOGIC_xESK_Operation_Outcome_t scoped_outcome;
        uint8_t scoped_result;
    } xLOGIC_Capture_Error_t;

    // UTF-16 code units; lengths exclude the terminating zero. Discovery borrows
    // these strings only until the callback returns. No CreateFile/WinUSB/control
    // access occurs during discovery. Enumerates present LOGIC interface GUID only,
    // resolving serial from the CM composite parent, not the MI child suffix.
    typedef struct
    {
        const uint16_t *device_path;
        size_t device_path_units;
        const uint16_t *serial;
        size_t serial_units;
    } xLOGIC_Capture_Discovery_Info_t;

    typedef void (*xLOGIC_Capture_Discovery_Callback_t)(void *user, const xLOGIC_Capture_Discovery_Info_t *info);

    // Enumeration always completes; a callback cannot stop before ambiguity is known.
    // Caller copies selected info. Open independently checks full serial uniqueness.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Discover(const uint16_t *serial,
                                                           size_t serial_units,
                                                           xLOGIC_Capture_Discovery_Callback_t callback,
                                                           void *user,
                                                           xLOGIC_Capture_Error_t *error);

    // Deep-copies selected path/serial; revalidates present GUID/parent binding and
    // verifies opened interface number 0. Opens only this LOGIC MI0 handle. Selected
    // serial must be nonempty. Does not open sibling MI1/MI2 or perform admission.
    // Argument/allocation failure leaves *context NULL. After allocation/native
    // ownership, failure may return non-NULL context: caller must Snapshot/Destroy.
    // Native cleanup can fail; never hide ownership or promise successful close.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Open(const xLOGIC_Capture_Discovery_Info_t *selected,
                                                       xLOGIC_Capture_WinUSB_Handle_t **context,
                                                       xLOGIC_Capture_Error_t *error);

    // OPEN only; ordinary ABI-v3 GET_CAPS, cached for Prepare validation.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Get_Caps(xLOGIC_Capture_WinUSB_Handle_t *context, xLOGIC_WINUSB_Caps_t *caps);

    // Narrow native HIL escape, OPEN only and before admission/Prepare. Implements
    // vendor/interface control on selected MI0. Rejects capture/session/admission
    // request opcodes (0x60..0x64 and 0x70..0x77); currently needed for HIL 0x65.
    // No retry/replay. bytes uses object-size domain and is checked before uint16
    // setup length/ULONG narrowing. Successful exact length returned via actual.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Control_Idle(xLOGIC_Capture_WinUSB_Handle_t *context,
                                                               bool is_input,
                                                               uint8_t request,
                                                               uint16_t value,
                                                               void *data,
                                                               size_t bytes,
                                                               size_t *actual);

    typedef enum
    {
        xLOGIC_CAPTURE_FLOW_CONTINUE,
        xLOGIC_CAPTURE_FLOW_STOP,
        xLOGIC_CAPTURE_FLOW_ERROR
    } xLOGIC_Capture_Flow_t;

    typedef struct
    {
        size_t bytes_consumed;
        xLOGIC_Capture_Flow_t flow;
        uint32_t error; // frontend-owned code, meaningful only with FLOW_ERROR.
    } xLOGIC_Capture_Sink_Result_t;

    // Called in submission order. Buffer is borrowed until return; it cannot be
    // retained, freed, modified, or used asynchronously. CONTINUE consumes all bytes.
    // STOP/ERROR may consume a prefix <=bytes; unconsumed suffix is discarded during
    // retirement, never delivered again. Packed logical clipping belongs to sink;
    // bytes_consumed counts wire bytes accepted, not logical sample count.
    typedef xLOGIC_Capture_Sink_Result_t (*xLOGIC_Capture_Sink_t)(void *user, const uint8_t *data, size_t bytes, uint64_t read_sequence);

    typedef enum
    {
        xLOGIC_CAPTURE_EVENT_CONTROL_ENTER,
        xLOGIC_CAPTURE_EVENT_CONTROL_RETURN,
        xLOGIC_CAPTURE_EVENT_RENEW_ENTER,
        xLOGIC_CAPTURE_EVENT_RENEW_RETURN,
        xLOGIC_CAPTURE_EVENT_READ_POST,
        xLOGIC_CAPTURE_EVENT_READ_REAP,
        xLOGIC_CAPTURE_EVENT_READ_CONSUME,
        xLOGIC_CAPTURE_EVENT_READ_EVENTS,
        xLOGIC_CAPTURE_EVENT_PAUSE_ENTER,
        xLOGIC_CAPTURE_EVENT_PAUSE_RETURN,
        xLOGIC_CAPTURE_EVENT_FIRST_FAILURE,
        xLOGIC_CAPTURE_EVENT_CLEANUP_READ,
        xLOGIC_CAPTURE_EVENT_CLEANUP_CHECK
    } xLOGIC_Capture_Event_Kind_t;

    // Fixed POD notification, not a second event ring. Native adapter retains its
    // existing bounded/frozen HostDiagnostics ring and presentation. timestamp_us
    // is sampled only when an observer is installed; normal operational statistics
    // and deadline clock reads are independent of this optional diagnostic clock.
    typedef struct
    {
        xLOGIC_Capture_Event_Kind_t kind;
        uint64_t timestamp_us;
        uint64_t read_sequence;
        uint64_t bytes;
        uint64_t requested_bytes;
        uint64_t request_id;
        uint64_t renewals;
        size_t pending_reads;
        size_t completed_reads;
        size_t signaled_reads;
        uint64_t pending_bytes_upper_bound;
        uint32_t native_error;
        int32_t result;
        uint16_t token;
        uint8_t request;
        uint8_t opcode;
    } xLOGIC_Capture_Event_t;

    typedef void (*xLOGIC_Capture_Observer_t)(void *user, const xLOGIC_Capture_Event_t *event);

    typedef struct
    {
        xLOGIC_WINUSB_Config_t config; // exact existing firmware ABI, no new codec.
        size_t queue_depth;
        size_t transfer_bytes;
        bool requests_raw_io;
        // Delivery cap, 0 unlimited. Submitted/reaped wire tails remain accounted.
        // Finite firmware U8 clamps each request to its remaining record bytes;
        // RAW_IO requires packet-aligned request geometry, otherwise disabled by
        // existing policy. Host delivery clipping never invents aligned wire bytes.
        uint64_t wire_limit_bytes;
        uint64_t duration_us;                // 0 no frontend duration bound; starts at native START boundary.
        uint32_t finish_timeout_ms;          // 0 selects 5000 ms; maximum 60000 ms.
        uint32_t diagnostic_refill_pause_ms; // existing 0..250 injection; native-only.
        xLOGIC_Capture_Sink_t sink;
        void *sink_user;
        xLOGIC_Capture_Observer_t observer; // NULL disables optional observations.
        void *observer_user;
    } xLOGIC_Capture_Prepare_t;

    // OPEN only. Copies all options; allocates the entire slot/buffer/event queue
    // once, validates geometry/power/pipe policies. No caller storage/OVERLAPPED is
    // retained. No slot allocation/reallocation after prepare. Failure is terminal
    // or leaves OPEN with all partial allocations retired; Snapshot distinguishes.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Prepare(xLOGIC_Capture_WinUSB_Handle_t *context, const xLOGIC_Capture_Prepare_t *options);

    // PREPARED only, once per context. Shared codec owns admission/configure/START.
    // Same pre-START queue population and acquisition statistics boundary as native.
    // Any uncertainty is latched, never replayed; Step must still retire owned I/O.
    // Accepted START latches acquisition time, then may return STARTING without
    // usable status. Step reaps/buffers and renews without sink until status is usable;
    // that Step returns at metadata handoff. Caller snapshots/emits start metadata
    // before next Step can deliver. Provisional/nonaccepted operation is a failure.
    // No new startup timeout: existing duration/lease/failure conditions apply.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Start(xLOGIC_Capture_WinUSB_Handle_t *context);

    // Visits at most the prepared slot population in one ready pass, including
    // immediately completed reposts. Ordered sink/repost before one due renewal;
    // unchanged 200ms predicate, fresh post-drain 1000ms/regression checks and
    // post-control check. Does no blocking read wait. Synchronous controls/sinks
    // may block: slot-bounded is not wall-time-bounded.
    // STOP, AbortPipe, nonblocking completion drain, terminal status and scoped
    // release/reap/close are shared phases across calls (no one-second wait loop).
    // OK means step executed, NOT acquisition success: inspect phase/first_error.
    // RETAINED_IO polls only terminal read completion, with no payload callbacks,
    // repost, control replay, or new acquisition. No live I/O is freed on expiry.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Step(xLOGIC_Capture_WinUSB_Handle_t *context);

    // Idempotent intent only; allowed from callbacks on owner thread. Never issues
    // control inline, destroys a slot, or re-enters the pump.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Request_Stop(xLOGIC_Capture_WinUSB_Handle_t *context);

    // Optional native blocking adapter. Wait only; no sink/control/retirement.
    // Hides event handles, caps sleep at next existing service/drain deadline and
    // maximum_ms; 0 polls. Caller Step before/after. Timer frontend does not use it.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Wait(xLOGIC_Capture_WinUSB_Handle_t *context, uint32_t maximum_ms, bool *has_ready_work);

    typedef struct
    {
        bool is_present;
        bool has_timestamp;
        uint32_t native_error;
        uint64_t read_sequence;
        uint64_t requested_bytes;
        uint64_t observed_us;
        // Actual failed byte extent deliberately absent/unknown (JSON null).
    } xLOGIC_Capture_Cleanup_Failure_t;

    typedef struct
    {
        bool has_started;
        bool is_drained;
        uint64_t pending_reads_at_entry;
        uint64_t completed_unconsumed_reads_at_entry;
        uint64_t completed_unconsumed_bytes_at_entry;
        uint64_t successful_reads;
        uint64_t successful_bytes;
        uint64_t canceled_reads;
        uint64_t failed_reads;
        uint64_t unresolved_reads;
        xLOGIC_Capture_Cleanup_Failure_t first_unexpected_failure;
    } xLOGIC_Capture_Cleanup_t;

    typedef struct
    {
        bool is_present;
        bool is_cleanup;
        uint8_t opcode;
        uint64_t operation_id;
        xLOGIC_xESK_Operation_Outcome_t outcome;
        int out_bytes;
        int reply_bytes;
        uint32_t native_error;
        uint8_t reply[96];
    } xLOGIC_Capture_Scoped_Failure_t;

    typedef struct
    {
        xLOGIC_Capture_Phase_t phase;
        bool io_terminal;     // no native read still owns buffer/event/OVERLAPPED.
        bool service_retired; // terminal service ownership CONFIRMED, not just attempted.
        bool can_destroy;     // OPEN/unstarted PREPARED/TERMINAL + io_terminal; no active/nested call.
        bool has_cleanup_error;
        xLOGIC_Capture_Error_t cleanup_error; // separate; never masks first acquisition error.
        bool has_first_error;
        xLOGIC_Capture_Error_t first_error; // immutable after first failure.
        bool has_started;
        bool has_stopped;
        bool is_scoped;
        bool is_configured;
        bool is_start_accepted;
        bool is_stop_accepted;
        uint8_t terminal_state;
        uint32_t terminal_flags;
        bool has_final_status;
        bool is_detached;
        bool is_raw_io_enabled;
        bool is_released;
        bool is_reaped;
        bool is_closed; // scoped xESK CLOSE confirmed, not native file-handle closure.
        bool has_unconfirmed_control;
        bool finish_accepted;
        bool finish_verified;
        size_t queue_depth;
        size_t transfer_bytes;
        size_t maximum_transfer_bytes;
        size_t pending_reads;
        size_t completed_reads;
        uint64_t submitted_reads;
        uint64_t stream_reaped_wire_bytes;
        uint64_t delivered_wire_bytes;
        uint64_t delivered_transfers;
        uint64_t acquisition_start_us;
        uint64_t acquisition_end_us;
        uint64_t last_read_us;
        uint64_t max_read_gap_us;
        uint64_t max_repost_delay_us;
        uint64_t max_renew_duration_us;
        uint64_t complete_1s_windows;
        uint64_t minimum_1s_bytes;
        uint64_t maximum_1s_bytes;
        uint64_t boot_id;
        uint64_t session_id;
        uint64_t control_id;
        uint64_t operation_id;
        uint64_t renewals;
        uint64_t max_renew_gap_ms;
        uint16_t connection_id;
        bool has_baseline_status;
        xLOGIC_WINUSB_Status_t baseline_status;
        bool has_start_status;
        xLOGIC_WINUSB_Status_t start_status;
        xLOGIC_WINUSB_Status_t final_status;
        xLOGIC_Capture_Cleanup_t cleanup;
        xLOGIC_Capture_Scoped_Failure_t scoped_failure;
    } xLOGIC_Capture_Snapshot_t;

    // Local state only, no device control or clock read. Not allowed in callback.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Snapshot(const xLOGIC_Capture_WinUSB_Handle_t *context,
                                                           xLOGIC_Capture_Snapshot_t *snapshot);

    // Owner-thread call outside callbacks. Clears sink/observer AND user pointers
    // before return and sets stop intent. Retains ownership/handles/queue; never
    // implies destroy. Later Step can retire but cannot call detached frontend.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Detach(xLOGIC_Capture_WinUSB_Handle_t *context);

    // Destroy BUSY leaves *context unchanged for caller retention. Once io_terminal,
    // attempts native WinUsb_Free/CloseHandle at most once, consumes/frees context
    // and NULLs *context EVEN when native close fails. close_error reports that
    // uncertainty separately; never retry native close against consumed handles.
    // Caller must retain identity tombstone after native closure uncertainty, and
    // must retain context after BUSY. External retention owner must outlive frontend
    // session/device teardown; this API does not provide a registry/manager.
    // Context is one-shot; can_destroy means memory safety, not confirmed remote STOP.
    xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Destroy(xLOGIC_Capture_WinUSB_Handle_t **context, xLOGIC_Capture_Error_t *close_error);

#ifdef __cplusplus
}
#endif
#endif
