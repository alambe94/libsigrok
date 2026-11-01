// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0
// Private Windows MI0 acquisition owner shared by the native and sigrok clients.
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "xlogic_capture_winusb.h"

static const GUID logic_guid = {0x6f72e6b1, 0x2e27, 0x4c7b, {0x8d, 0x31, 0xa0, 0x24, 0x3c, 0x0d, 0x00, 0x03}};
#define xLOGIC_CAPTURE_EP_IN 0x81U

typedef struct
{
    uint8_t *data;
    HANDLE event;
    OVERLAPPED overlapped;
    uint64_t sequence;
    uint64_t reaped_us;
    ULONG requested;
    ULONG actual;
    bool is_pending;
    bool is_completed;
    bool is_visited;
} Capture_Slot_t;

struct xLOGIC_Capture_WinUSB_Context
{
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    LARGE_INTEGER frequency;
    wchar_t *path;
    wchar_t *serial;
    xLOGIC_WINUSB_Caps_t caps;
    xLOGIC_xESK_Host_t host;
    xLOGIC_Capture_Prepare_t options;
    xLOGIC_Capture_Snapshot_t snapshot;
    Capture_Slot_t *slots;
    uint8_t *buffers;
    HANDLE events[MAXIMUM_WAIT_OBJECTS];
    uint64_t next_submission;
    uint64_t next_consumption;
    uint64_t requested_total;
    uint64_t deadline_us;
    uint64_t finish_deadline_us;
    uint64_t drain_deadline_ms;
    uint64_t service_deadline_ms;
    uint64_t window;
    uint64_t window_bytes;
    uint64_t common_id;
    uint64_t operation_id;
    uint8_t common_opcode;
    uint8_t operation_opcode;
    uint32_t last_control_error;
    unsigned retirement_step;
    uint8_t cleanup_stage;
    bool is_in_api;
    bool is_in_callback;
    bool has_stop_request;
    bool sink_requested_stop;
    bool finish_end_seen;
    bool has_abort_attempt;
    bool has_reset_attempt;
    bool has_pause;
    bool has_failure_returned;
    bool is_renewing;
};

static void pump_ready(xLOGIC_Capture_WinUSB_Handle_t *context, bool can_deliver);
static bool active(xLOGIC_Capture_WinUSB_Handle_t *context);

static uint64_t clock_us(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    LARGE_INTEGER value;
    if (!QueryPerformanceCounter(&value) || value.QuadPart < 0 || context->frequency.QuadPart <= 0)
    {
        return UINT64_MAX;
    }
    return ((uint64_t)value.QuadPart / (uint64_t)context->frequency.QuadPart) * UINT64_C(1000000) +
           (((uint64_t)value.QuadPart % (uint64_t)context->frequency.QuadPart) * UINT64_C(1000000)) / (uint64_t)context->frequency.QuadPart;
}
static uint64_t host_clock(void *user)
{
    return clock_us((xLOGIC_Capture_WinUSB_Handle_t *)user) / 1000U;
}
static void host_sleep(void *user, unsigned milliseconds)
{
    (void)user;
    Sleep(milliseconds);
}
static xLOGIC_Capture_Result_t enter(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (context == NULL)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    if (context->is_in_api || context->is_in_callback)
    {
        return xLOGIC_CAPTURE_BUSY;
    }
    context->is_in_api = true;
    return xLOGIC_CAPTURE_OK;
}
static xLOGIC_Capture_Result_t leave(xLOGIC_Capture_WinUSB_Handle_t *context, xLOGIC_Capture_Result_t result)
{
    context->is_in_api = false;
    return result;
}
static void refresh(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    xLOGIC_Capture_Snapshot_t *s = &context->snapshot;
    size_t i;
    s->pending_reads = s->completed_reads = 0U;
    for (i = 0U; context->slots != NULL && i < s->queue_depth; ++i)
    {
        s->pending_reads += context->slots[i].is_pending ? 1U : 0U;
        s->completed_reads += context->slots[i].is_completed ? 1U : 0U;
    }
    s->io_terminal = s->pending_reads == 0U;
    s->can_destroy =
        s->io_terminal && (s->phase == xLOGIC_CAPTURE_OPEN || s->phase == xLOGIC_CAPTURE_PREPARED || s->phase == xLOGIC_CAPTURE_TERMINAL);
    s->boot_id = context->host.boot_id;
    s->session_id = context->host.session_id;
    s->connection_id = context->host.connection_id;
    s->control_id = context->host.control_id;
    s->operation_id = context->host.operation_id;
    s->renewals = context->host.renewals;
    s->max_renew_gap_ms = context->host.max_renew_gap_ms;
    s->is_released = context->host.released;
    s->is_reaped = context->host.reaped;
    s->is_closed = context->host.closed;
    s->is_scoped = context->host.scoped;
    s->is_configured = context->host.configured;
    s->is_start_accepted = context->host.started;
    s->is_stop_accepted = context->host.stopped;
    s->finish_accepted = context->host.finish_requested;
    s->terminal_state = context->host.terminal_state;
    s->terminal_flags = context->host.terminal_flags;
}
static void observe(xLOGIC_Capture_WinUSB_Handle_t *context,
                    xLOGIC_Capture_Event_Kind_t kind,
                    uint64_t sequence,
                    uint64_t bytes,
                    uint8_t request,
                    uint8_t opcode,
                    uint64_t request_id,
                    int32_t result,
                    uint32_t native_error)
{
    xLOGIC_Capture_Event_t event;
    size_t i;
    if (context->options.observer == NULL)
    {
        return;
    }
    refresh(context);
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.timestamp_us = clock_us(context);
    event.read_sequence = sequence;
    event.bytes = bytes;
    event.requested_bytes = bytes;
    if (kind == xLOGIC_CAPTURE_EVENT_READ_REAP || kind == xLOGIC_CAPTURE_EVENT_CLEANUP_READ || kind == xLOGIC_CAPTURE_EVENT_CLEANUP_CHECK)
    {
        for (i = 0U; i < context->snapshot.queue_depth; ++i)
        {
            if (context->slots[i].sequence == sequence)
            {
                event.requested_bytes = context->slots[i].requested;
                break;
            }
        }
    }
    event.request = request;
    event.opcode = opcode;
    event.request_id = request_id;
    event.result = result;
    event.native_error = native_error;
    event.token = context->host.connection_id;
    event.renewals = context->host.renewals;
    event.pending_reads = context->snapshot.pending_reads;
    event.completed_reads = context->snapshot.completed_reads;
    for (i = 0U; context->slots != NULL && i < context->snapshot.queue_depth; ++i)
    {
        if (context->slots[i].is_pending)
        {
            event.pending_bytes_upper_bound += context->slots[i].requested;
        }
        if (context->slots[i].event != NULL && WaitForSingleObject(context->slots[i].event, 0U) == WAIT_OBJECT_0)
        {
            ++event.signaled_reads;
        }
    }
    context->is_in_callback = true;
    context->options.observer(context->options.observer_user, &event);
    context->is_in_callback = false;
}
static xLOGIC_Capture_Result_t failure(xLOGIC_Capture_WinUSB_Handle_t *context,
                                       xLOGIC_Capture_Result_t result,
                                       xLOGIC_Capture_Stage_t stage,
                                       uint32_t native_error,
                                       bool is_cleanup)
{
    xLOGIC_Capture_Error_t *error;
    bool *present;
    present = is_cleanup ? &context->snapshot.has_cleanup_error : &context->snapshot.has_first_error;
    error = is_cleanup ? &context->snapshot.cleanup_error : &context->snapshot.first_error;
    if (!*present)
    {
        memset(error, 0, sizeof(*error));
        error->result = result;
        error->stage = stage;
        error->native_error = native_error;
        error->has_native_error = native_error != 0U;
        error->scoped_outcome = context->host.operation_outcome;
        error->scoped_result = context->host.reply[6];
        *present = true;
        if (!is_cleanup)
        {
            observe(context, xLOGIC_CAPTURE_EVENT_FIRST_FAILURE, 0U, 0U, 0U, 0U, 0U, (int32_t)result, native_error);
        }
    }
    if (!is_cleanup)
    {
        context->has_stop_request = true;
    }
    return result;
}
static xLOGIC_Capture_Result_t discovery_error(xLOGIC_Capture_Error_t *error, uint32_t native_error)
{
    if (error != NULL)
    {
        memset(error, 0, sizeof(*error));
        error->result = xLOGIC_CAPTURE_NATIVE_ERROR;
        error->stage = xLOGIC_CAPTURE_STAGE_DISCOVER;
        error->native_error = native_error;
        error->has_native_error = true;
    }
    return xLOGIC_CAPTURE_NATIVE_ERROR;
}

typedef struct
{
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
    wchar_t identity[MAX_DEVICE_ID_LEN];
    xLOGIC_Capture_Discovery_Info_t info;
} Discovery_Record_t;

static size_t utf16_length(const wchar_t *text, size_t capacity)
{
    size_t length = 0U;
    while (length < capacity && text[length] != L'\0')
    {
        ++length;
    }
    return length;
}
static xLOGIC_Capture_Result_t
discovery_record(HDEVINFO devices, SP_DEVICE_INTERFACE_DATA *interface_data, Discovery_Record_t *record, xLOGIC_Capture_Error_t *error)
{
    DWORD needed = 0U;
    SP_DEVINFO_DATA device;
    DEVINST node;
    size_t i, serial_index = 0U, identity_units;
    memset(record, 0, sizeof(*record));
    SetupDiGetDeviceInterfaceDetailW(devices, interface_data, NULL, 0U, &needed, NULL);
    if (needed < sizeof(*record->detail))
    {
        return discovery_error(error, GetLastError());
    }
    record->detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)calloc(1U, needed);
    if (record->detail == NULL)
    {
        return xLOGIC_CAPTURE_NO_MEMORY;
    }
    record->detail->cbSize = sizeof(*record->detail);
    memset(&device, 0, sizeof(device));
    device.cbSize = sizeof(device);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, interface_data, record->detail, needed, NULL, &device))
    {
        return discovery_error(error, GetLastError());
    }
    record->info.device_path = (const uint16_t *)record->detail->DevicePath;
    record->info.device_path_units =
        utf16_length(record->detail->DevicePath, (needed - offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)) / sizeof(wchar_t));
    node = device.DevInst;
    for (i = 0U; i < record->info.device_path_units; ++i)
    {
        if (_wcsnicmp(record->detail->DevicePath + i, L"&mi_", 4U) == 0)
        {
            if (CM_Get_Parent(&node, device.DevInst, 0U) != CR_SUCCESS)
            {
                return discovery_error(error, ERROR_NOT_FOUND);
            }
            break;
        }
    }
    if (CM_Get_Device_IDW(node, record->identity, MAX_DEVICE_ID_LEN, 0U) != CR_SUCCESS)
    {
        return discovery_error(error, ERROR_NOT_FOUND);
    }
    identity_units = utf16_length(record->identity, MAX_DEVICE_ID_LEN);
    if (identity_units == MAX_DEVICE_ID_LEN)
    {
        return discovery_error(error, ERROR_INVALID_DATA);
    }
    for (i = 0U; i < identity_units; ++i)
    {
        if (record->identity[i] == L'\\')
        {
            serial_index = i + 1U;
        }
    }
    record->info.serial = (const uint16_t *)(record->identity + serial_index);
    record->info.serial_units = identity_units - serial_index;
    return xLOGIC_CAPTURE_OK;
}
static void dispatch_discovery(const Discovery_Record_t *record,
                               const uint16_t *serial,
                               size_t serial_units,
                               xLOGIC_Capture_Discovery_Callback_t callback,
                               void *user)
{
    if (serial_units == 0U || (record->info.serial_units == serial_units &&
                               _wcsnicmp((const wchar_t *)record->info.serial, (const wchar_t *)serial, serial_units) == 0))
    {
        callback(user, &record->info);
    }
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Discover(const uint16_t *serial,
                                                       size_t serial_units,
                                                       xLOGIC_Capture_Discovery_Callback_t callback,
                                                       void *user,
                                                       xLOGIC_Capture_Error_t *error)
{
    HDEVINFO devices;
    DWORD index;
    xLOGIC_Capture_Result_t result = xLOGIC_CAPTURE_OK;
    if (callback == NULL || (serial == NULL && serial_units != 0U) || serial_units >= MAX_DEVICE_ID_LEN)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    if (error != NULL)
    {
        memset(error, 0, sizeof(*error));
    }
    devices = SetupDiGetClassDevsW(&logic_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
    {
        return discovery_error(error, GetLastError());
    }
    for (index = 0U;; ++index)
    {
        SP_DEVICE_INTERFACE_DATA interface_data;
        Discovery_Record_t record;
        memset(&interface_data, 0, sizeof(interface_data));
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, NULL, &logic_guid, index, &interface_data))
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
            {
                result = discovery_error(error, GetLastError());
            }
            break;
        }
        result = discovery_record(devices, &interface_data, &record, error);
        if (result == xLOGIC_CAPTURE_OK)
        {
            dispatch_discovery(&record, serial, serial_units, callback, user);
        }
        free(record.detail);
        if (result != xLOGIC_CAPTURE_OK)
        {
            break;
        }
    }
    if (!SetupDiDestroyDeviceInfoList(devices) && result == xLOGIC_CAPTURE_OK)
    {
        result = discovery_error(error, GetLastError());
    }
    return result;
}

typedef struct
{
    const xLOGIC_Capture_Discovery_Info_t *selected;
    size_t matches;
    bool has_path;
} Selection_t;
static void select_identity(void *user, const xLOGIC_Capture_Discovery_Info_t *info)
{
    Selection_t *selection = (Selection_t *)user;
    ++selection->matches;
    if (info->device_path_units == selection->selected->device_path_units &&
        _wcsnicmp((const wchar_t *)info->device_path, (const wchar_t *)selection->selected->device_path, info->device_path_units) == 0)
    {
        selection->has_path = true;
    }
}
static bool valid_selection(const xLOGIC_Capture_Discovery_Info_t *selected)
{
    return selected != NULL && selected->serial != NULL && selected->device_path != NULL && selected->serial_units != 0U &&
           selected->serial_units < MAX_DEVICE_ID_LEN && selected->device_path_units != 0U && selected->device_path_units < 32768U;
}
static xLOGIC_Capture_Result_t open_native(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    USB_INTERFACE_DESCRIPTOR descriptor;
    UCHAR suspend = FALSE;
    context->file = CreateFileW(context->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (context->file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        context->file = NULL;
        return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_OPEN, error, false);
    }
    if (!WinUsb_Initialize(context->file, &context->usb) || !WinUsb_QueryInterfaceSettings(context->usb, 0U, &descriptor))
    {
        return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_OPEN, GetLastError(), false);
    }
    if (descriptor.bInterfaceNumber != 0U)
    {
        return failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_OPEN, 0U, false);
    }
    if (!WinUsb_SetPowerPolicy(context->usb, 0x81U, sizeof(suspend), &suspend) || !QueryPerformanceFrequency(&context->frequency))
    {
        return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_OPEN, GetLastError(), false);
    }
    if (context->frequency.QuadPart <= 0 || (uint64_t)context->frequency.QuadPart > UINT64_MAX / 1000000U)
    {
        return failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_OPEN, 0U, false);
    }
    return xLOGIC_CAPTURE_OK;
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Open(const xLOGIC_Capture_Discovery_Info_t *selected,
                                                   xLOGIC_Capture_WinUSB_Handle_t **output,
                                                   xLOGIC_Capture_Error_t *error)
{
    Selection_t selection;
    xLOGIC_Capture_WinUSB_Handle_t *context;
    xLOGIC_Capture_Result_t result;
    if (output == NULL)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!valid_selection(selected))
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    selection.selected = selected;
    selection.matches = 0U;
    selection.has_path = false;
    result = xLOGIC_Capture_WinUSB_Discover(selected->serial, selected->serial_units, select_identity, &selection, error);
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    if (selection.matches != 1U || !selection.has_path)
    {
        return selection.matches > 1U ? xLOGIC_CAPTURE_AMBIGUOUS : xLOGIC_CAPTURE_NOT_FOUND;
    }
    context = (xLOGIC_Capture_WinUSB_Handle_t *)calloc(1U, sizeof(*context));
    if (context == NULL)
    {
        return xLOGIC_CAPTURE_NO_MEMORY;
    }
    context->path = (wchar_t *)calloc(selected->device_path_units + 1U, sizeof(wchar_t));
    context->serial = (wchar_t *)calloc(selected->serial_units + 1U, sizeof(wchar_t));
    if (context->path == NULL || context->serial == NULL)
    {
        free(context->path);
        free(context->serial);
        free(context);
        return xLOGIC_CAPTURE_NO_MEMORY;
    }
    memcpy(context->path, selected->device_path, selected->device_path_units * sizeof(uint16_t));
    memcpy(context->serial, selected->serial, selected->serial_units * sizeof(uint16_t));
    context->snapshot.phase = xLOGIC_CAPTURE_OPEN;
    context->snapshot.io_terminal = context->snapshot.can_destroy = true;
    *output = context;
    result = open_native(context);
    if (error != NULL)
    {
        *error = context->snapshot.first_error;
    }
    return result;
}

static void control_identity(xLOGIC_Capture_WinUSB_Handle_t *context,
                             uint8_t request,
                             const uint8_t *data,
                             size_t bytes,
                             uint8_t *opcode,
                             uint64_t *request_id)
{
    if (request == 0x71U && bytes >= 48U)
    {
        context->common_opcode = data[10];
        context->common_id = xlogic_xesk_get(data + 16U, 8U);
    }
    if (request >= 0x71U && request <= 0x73U)
    {
        *opcode = context->common_opcode;
        *request_id = context->common_id;
    }
    if (request == 0x76U && bytes >= 32U)
    {
        context->operation_opcode = data[5];
        context->operation_id = xlogic_xesk_get(data + 24U, 8U);
    }
    if (request == 0x76U || request == 0x77U)
    {
        *opcode = context->operation_opcode;
        *request_id = context->operation_id;
    }
}
static void service_renewal_reads(xLOGIC_Capture_WinUSB_Handle_t *context, bool success)
{
    size_t i;
    if (!success || !context->is_renewing || context->snapshot.phase != xLOGIC_CAPTURE_RUNNING || !active(context))
    {
        return;
    }
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        context->slots[i].is_visited = false;
    }
    pump_ready(context, true);
}
static int control_transfer(void *user, bool is_input, uint8_t request, uint16_t value, uint8_t *data, size_t bytes)
{
    xLOGIC_Capture_WinUSB_Handle_t *context = (xLOGIC_Capture_WinUSB_Handle_t *)user;
    WINUSB_SETUP_PACKET setup;
    ULONG actual = 0U;
    BOOL success;
    uint8_t opcode = 0U;
    uint64_t request_id = 0U;
    DWORD error;
    if (bytes > UINT16_MAX || context->usb == NULL)
    {
        return -1;
    }
    control_identity(context, request, data, bytes, &opcode, &request_id);
    if (request == 0x71U && opcode == 3U)
    {
        observe(context, xLOGIC_CAPTURE_EVENT_READ_EVENTS, 0U, 0U, 0U, 0U, 0U, 0, 0U);
        observe(context, xLOGIC_CAPTURE_EVENT_RENEW_ENTER, 0U, 0U, request, opcode, request_id, 0, 0U);
    }
    observe(context, xLOGIC_CAPTURE_EVENT_CONTROL_ENTER, 0U, 0U, request, opcode, request_id, 0, 0U);
    memset(&setup, 0, sizeof(setup));
    setup.RequestType = is_input ? 0xc1U : 0x41U;
    setup.Request = request;
    setup.Value = value;
    setup.Length = (USHORT)bytes;
    success = WinUsb_ControlTransfer(context->usb, setup, data, (ULONG)bytes, &actual, NULL);
    error = success ? 0U : GetLastError();
    context->last_control_error = error;
    observe(context, xLOGIC_CAPTURE_EVENT_CONTROL_RETURN, 0U, 0U, request, opcode, request_id, success ? (int32_t)actual : -1, error);
    service_renewal_reads(context, success != FALSE);
    if (!success)
    {
        return is_input && request == 0x72U && error == ERROR_GEN_FAILURE ? -2 : -1;
    }
    return (int)actual;
}
static bool read_status(xLOGIC_Capture_WinUSB_Handle_t *context, xLOGIC_WINUSB_Status_t *status)
{
    memset(status, 0, sizeof(*status));
    return control_transfer(context, true, xLOGIC_WINUSB_REQUEST_GET_STATUS, 0U, (uint8_t *)status, sizeof(*status)) == sizeof(*status) &&
           xlogic_xesk_status((const uint8_t *)status);
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Get_Caps(xLOGIC_Capture_WinUSB_Handle_t *context, xLOGIC_WINUSB_Caps_t *caps)
{
    xLOGIC_Capture_Result_t result = enter(context);
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    if (caps == NULL || context->snapshot.phase != xLOGIC_CAPTURE_OPEN || context->snapshot.has_first_error)
    {
        return leave(context, xLOGIC_CAPTURE_INVALID_STATE);
    }
    if (control_transfer(context, true, xLOGIC_WINUSB_REQUEST_GET_CAPS, 0U, (uint8_t *)&context->caps, sizeof(context->caps)) !=
            sizeof(context->caps) ||
        context->caps.magic != xLOGIC_WINUSB_MAGIC || context->caps.version != xLOGIC_WINUSB_ABI_VERSION ||
        context->caps.struct_size != sizeof(context->caps) || context->caps.stream_buffer_count < 2U ||
        context->caps.stream_buffer_bytes == 0U)
    {
        result = failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_PREPARE, context->last_control_error, false);
    }
    else
    {
        *caps = context->caps;
    }
    return leave(context, result);
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Control_Idle(xLOGIC_Capture_WinUSB_Handle_t *context,
                                                           bool is_input,
                                                           uint8_t request,
                                                           uint16_t value,
                                                           void *data,
                                                           size_t bytes,
                                                           size_t *actual)
{
    xLOGIC_Capture_Result_t result = enter(context);
    int transferred;
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    if (context->snapshot.phase != xLOGIC_CAPTURE_OPEN || context->snapshot.has_first_error || actual == NULL || bytes > UINT16_MAX ||
        (bytes != 0U && data == NULL) || (request >= 0x60U && request <= 0x64U) || (request >= 0x70U && request <= 0x77U))
    {
        return leave(context, xLOGIC_CAPTURE_INVALID_ARGUMENT);
    }
    *actual = 0U;
    transferred = control_transfer(context, is_input, request, value, (uint8_t *)data, bytes);
    if (transferred < 0 || (size_t)transferred != bytes)
    {
        result = failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_IDLE_CONTROL, context->last_control_error, false);
    }
    else
    {
        *actual = (size_t)transferred;
    }
    return leave(context, result);
}
static bool valid_queue_geometry(const xLOGIC_Capture_Prepare_t *options)
{
    return options->queue_depth != 0U && options->queue_depth <= MAXIMUM_WAIT_OBJECTS && options->transfer_bytes != 0U &&
           options->transfer_bytes <= ULONG_MAX && options->transfer_bytes <= SIZE_MAX / options->queue_depth;
}
static bool valid_capture_plan(const xLOGIC_Capture_WinUSB_Handle_t *context, const xLOGIC_Capture_Prepare_t *options)
{
    if (options->config.magic != xLOGIC_WINUSB_MAGIC || options->config.version != xLOGIC_WINUSB_ABI_VERSION ||
        options->config.struct_size != sizeof(options->config) || options->sink == NULL || options->diagnostic_refill_pause_ms > 250U)
    {
        return false;
    }
    return options->diagnostic_refill_pause_ms == 0U || (options->observer != NULL && options->config.sample_count == 0U &&
                                                         (context->caps.feature_flags & xLOGIC_WINUSB_CAP_SCOPED_CONTROL) != 0U);
}
static xLOGIC_Capture_Result_t prepare_pipe(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    USB_INTERFACE_DESCRIPTOR descriptor;
    WINUSB_PIPE_INFORMATION pipe;
    ULONG maximum = 0U, policy_bytes = sizeof(maximum), raw;
    size_t i;
    const xLOGIC_Capture_Prepare_t *options = &context->options;
    if (!WinUsb_GetPipePolicy(context->usb, xLOGIC_CAPTURE_EP_IN, 0x08U, &policy_bytes, &maximum) ||
        !WinUsb_QueryInterfaceSettings(context->usb, 0U, &descriptor))
    {
        return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_PREPARE, GetLastError(), false);
    }
    if (options->transfer_bytes > maximum)
    {
        return failure(context, xLOGIC_CAPTURE_INVALID_ARGUMENT, xLOGIC_CAPTURE_STAGE_PREPARE, 0U, false);
    }
    context->snapshot.maximum_transfer_bytes = maximum;
    for (i = 0U; i < descriptor.bNumEndpoints; ++i)
    {
        if (!WinUsb_QueryPipe(context->usb, 0U, (UCHAR)i, &pipe))
        {
            return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_PREPARE, GetLastError(), false);
        }
        if (pipe.PipeId != xLOGIC_CAPTURE_EP_IN || pipe.PipeType != UsbdPipeTypeBulk || pipe.MaximumPacketSize == 0U)
        {
            continue;
        }
        raw = options->requests_raw_io && options->transfer_bytes % pipe.MaximumPacketSize == 0U &&
              (options->config.sample_count == 0U || options->config.sample_count % pipe.MaximumPacketSize == 0U);
        if (!WinUsb_SetPipePolicy(context->usb, xLOGIC_CAPTURE_EP_IN, 0x07U, sizeof(raw), &raw))
        {
            return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_PREPARE, GetLastError(), false);
        }
        context->snapshot.is_raw_io_enabled = raw != FALSE;
        return xLOGIC_CAPTURE_OK;
    }
    return failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_PREPARE, 0U, false);
}
static xLOGIC_Capture_Result_t allocate_queue(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    size_t i;
    const size_t count = context->options.queue_depth;
    const size_t bytes = context->options.transfer_bytes;
    context->slots = (Capture_Slot_t *)calloc(count, sizeof(*context->slots));
    context->buffers = (uint8_t *)malloc(count * bytes);
    if (context->slots == NULL || context->buffers == NULL)
    {
        return failure(context, xLOGIC_CAPTURE_NO_MEMORY, xLOGIC_CAPTURE_STAGE_PREPARE, 0U, false);
    }
    for (i = 0U; i < count; ++i)
    {
        context->slots[i].data = context->buffers + i * bytes;
        context->slots[i].event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (context->slots[i].event == NULL)
        {
            return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_PREPARE, GetLastError(), false);
        }
        context->events[i] = context->slots[i].event;
    }
    return xLOGIC_CAPTURE_OK;
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Prepare(xLOGIC_Capture_WinUSB_Handle_t *context, const xLOGIC_Capture_Prepare_t *options)
{
    xLOGIC_Capture_Result_t result = enter(context);
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    if (options == NULL || context->snapshot.phase != xLOGIC_CAPTURE_OPEN || context->snapshot.has_first_error ||
        context->caps.magic != xLOGIC_WINUSB_MAGIC || options->finish_timeout_ms > xLOGIC_CAPTURE_FINISH_TIMEOUT_MAX_MS ||
        !valid_queue_geometry(options) || !valid_capture_plan(context, options))
    {
        return leave(context, xLOGIC_CAPTURE_INVALID_ARGUMENT);
    }
    context->options = *options;
    if (context->options.finish_timeout_ms == 0U)
    {
        context->options.finish_timeout_ms = xLOGIC_CAPTURE_FINISH_TIMEOUT_DEFAULT_MS;
    }
    context->snapshot.queue_depth = options->queue_depth;
    context->snapshot.transfer_bytes = options->transfer_bytes;
    context->snapshot.minimum_1s_bytes = UINT64_MAX;
    result = prepare_pipe(context);
    if (result == xLOGIC_CAPTURE_OK)
    {
        result = allocate_queue(context);
    }
    if (result == xLOGIC_CAPTURE_OK)
    {
        context->snapshot.phase = xLOGIC_CAPTURE_PREPARED;
    }
    refresh(context);
    return leave(context, result);
}

static bool add_count(xLOGIC_Capture_WinUSB_Handle_t *context, uint64_t *value, uint64_t amount)
{
    if (*value > UINT64_MAX - amount)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_REAP, 0U, false);
        return false;
    }
    *value += amount;
    return true;
}
static bool complete_windows(xLOGIC_Capture_WinUSB_Handle_t *context, uint64_t now)
{
    xLOGIC_Capture_Snapshot_t *s = &context->snapshot;
    uint64_t window;
    if (now < s->acquisition_start_us)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_REAP, 0U, false);
        return false;
    }
    window = (now - s->acquisition_start_us) / UINT64_C(1000000);
    if (window < context->window)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_REAP, 0U, false);
        return false;
    }
    if (window != context->window)
    {
        if (context->window_bytes < s->minimum_1s_bytes)
        {
            s->minimum_1s_bytes = context->window_bytes;
        }
        if (context->window_bytes > s->maximum_1s_bytes)
        {
            s->maximum_1s_bytes = context->window_bytes;
        }
        if (window - context->window > 1U)
        {
            s->minimum_1s_bytes = 0U;
        }
        s->complete_1s_windows += window - context->window;
        context->window = window;
        context->window_bytes = 0U;
    }
    return true;
}
static bool active(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    uint64_t now = clock_us(context);
    if (now < context->snapshot.last_read_us || now == UINT64_MAX)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_REAP, 0U, false);
    }
    if (context->snapshot.phase == xLOGIC_CAPTURE_FINISHING)
    {
        return !context->snapshot.has_first_error && !context->snapshot.is_detached && !context->finish_end_seen &&
               now < context->finish_deadline_us;
    }
    return !context->has_stop_request && !context->snapshot.has_first_error &&
           (context->options.wire_limit_bytes == 0U || context->snapshot.delivered_wire_bytes < context->options.wire_limit_bytes) &&
           now < context->deadline_us;
}
static bool post_read(xLOGIC_Capture_WinUSB_Handle_t *context, Capture_Slot_t *slot)
{
    uint64_t bytes = context->options.transfer_bytes;
    uint64_t now;
    if (slot->is_pending || slot->is_completed || context->next_submission == UINT64_MAX)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_SUBMIT, 0U, false);
        return false;
    }
    if (context->options.config.sample_count != 0U)
    {
        if (context->requested_total >= context->options.config.sample_count)
        {
            return true;
        }
        if (bytes > context->options.config.sample_count - context->requested_total)
        {
            bytes = context->options.config.sample_count - context->requested_total;
        }
    }
    if (!ResetEvent(slot->event))
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_SUBMIT, GetLastError(), false);
        return false;
    }
    memset(&slot->overlapped, 0, sizeof(slot->overlapped));
    slot->overlapped.hEvent = slot->event;
    slot->sequence = context->next_submission++;
    slot->requested = (ULONG)bytes;
    slot->actual = 0U;
    slot->is_pending = true;
    if (!WinUsb_ReadPipe(context->usb, xLOGIC_CAPTURE_EP_IN, slot->data, slot->requested, NULL, &slot->overlapped) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        slot->is_pending = false;
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_SUBMIT, GetLastError(), false);
        return false;
    }
    if (context->options.config.sample_count != 0U)
    {
        context->requested_total += bytes;
    }
    ++context->snapshot.submitted_reads;
    now = clock_us(context);
    if (slot->reaped_us != 0U)
    {
        if (now < slot->reaped_us)
        {
            failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_SUBMIT, 0U, false);
            return false;
        }
        if (now - slot->reaped_us > context->snapshot.max_repost_delay_us)
        {
            context->snapshot.max_repost_delay_us = now - slot->reaped_us;
        }
        slot->reaped_us = 0U;
    }
    observe(context, xLOGIC_CAPTURE_EVENT_READ_POST, slot->sequence, bytes, 0U, 0U, 0U, 0, 0U);
    return true;
}
static bool capture_operation(xLOGIC_Capture_WinUSB_Handle_t *context, uint8_t opcode, bool is_cleanup)
{
    xLOGIC_Capture_Stage_t stage = opcode == 1U   ? xLOGIC_CAPTURE_STAGE_CONFIGURE
                                   : opcode == 2U ? xLOGIC_CAPTURE_STAGE_START
                                   : opcode == 4U ? xLOGIC_CAPTURE_STAGE_FINISH
                                                  : xLOGIC_CAPTURE_STAGE_STOP;
    bool success;
    if (context->host.scoped)
    {
        success = xlogic_xesk_operation(&context->host, opcode, opcode == 1U ? (const uint8_t *)&context->options.config : NULL) == 0;
    }
    else
    {
        if (opcode == 4U)
        {
            return false;
        }
        uint8_t request =
            opcode == 1U ? xLOGIC_WINUSB_REQUEST_CONFIGURE : (opcode == 2U ? xLOGIC_WINUSB_REQUEST_START : xLOGIC_WINUSB_REQUEST_STOP);
        size_t bytes = opcode == 1U ? sizeof(context->options.config) : 0U;
        success = control_transfer(context, false, request, 0U, (uint8_t *)&context->options.config, bytes) == (int)bytes;
    }
    if (!success)
    {
        xLOGIC_Capture_Error_t *error;
        if (context->host.scoped && !context->snapshot.scoped_failure.is_present)
        {
            xLOGIC_Capture_Scoped_Failure_t *scoped = &context->snapshot.scoped_failure;
            scoped->is_present = true;
            scoped->is_cleanup = is_cleanup;
            scoped->opcode = opcode;
            scoped->operation_id = context->host.operation_id;
            scoped->outcome = context->host.operation_outcome;
            scoped->out_bytes = context->host.operation_out_bytes;
            scoped->reply_bytes = context->host.operation_reply_bytes;
            scoped->native_error = context->last_control_error;
            memcpy(scoped->reply, context->host.reply, sizeof(scoped->reply));
        }
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, stage, context->last_control_error, is_cleanup);
        error = is_cleanup ? &context->snapshot.cleanup_error : &context->snapshot.first_error;
        error->opcode = opcode;
        error->request = context->host.scoped ? 0x76U : (uint8_t)(0x60U + opcode);
    }
    return success;
}
static bool usable_start_status(const xLOGIC_Capture_WinUSB_Handle_t *context, const xLOGIC_WINUSB_Status_t *status)
{
    if ((status->status_flags & xLOGIC_WINUSB_STATUS_START_PENDING) != 0U)
    {
        return false;
    }
    if (status->running != 0U)
    {
        return true;
    }
    return context->options.config.sample_count != 0U && status->produced_bytes >= context->options.config.sample_count &&
           status->transmitted_bytes >= context->options.config.sample_count;
}
static bool start_admission(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    context->host.user = context;
    context->host.transfer = control_transfer;
    context->host.clock_ms = host_clock;
    context->host.sleep_ms = host_sleep;
    context->host.scoped = (context->caps.feature_flags & xLOGIC_WINUSB_CAP_SCOPED_CONTROL) != 0U;
    context->snapshot.phase = xLOGIC_CAPTURE_RUNNING;
    if (xlogic_xesk_open(&context->host) != 0 || !capture_operation(context, 1U, false) ||
        !read_status(context, &context->snapshot.baseline_status))
    {
        return false;
    }
    if (context->snapshot.baseline_status.configured == 0U ||
        context->snapshot.baseline_status.requested_sample_rate != context->options.config.sample_rate ||
        context->snapshot.baseline_status.stream_format != context->options.config.stream_format)
    {
        return false;
    }
    context->snapshot.has_baseline_status = true;
    return true;
}
static bool accepted_start(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    const uint64_t now = clock_us(context);
    context->snapshot.has_started = true;
    context->snapshot.acquisition_start_us = context->snapshot.last_read_us = now;
    if (now == UINT64_MAX || (context->options.duration_us != 0U && context->options.duration_us > UINT64_MAX - now))
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_START, 0U, false);
        return false;
    }
    context->deadline_us = context->options.duration_us == 0U ? UINT64_MAX : now + context->options.duration_us;
    if (context->host.scoped)
    {
        memcpy(&context->snapshot.start_status, context->host.reply + 32U, sizeof(context->snapshot.start_status));
    }
    else if (!read_status(context, &context->snapshot.start_status))
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_START, context->last_control_error, false);
        return false;
    }
    context->snapshot.has_start_status = usable_start_status(context, &context->snapshot.start_status);
    context->snapshot.phase = context->snapshot.has_start_status ? xLOGIC_CAPTURE_RUNNING : xLOGIC_CAPTURE_STARTING;
    return true;
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Start(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    xLOGIC_Capture_Result_t result = enter(context);
    size_t i;
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    if (context->snapshot.phase != xLOGIC_CAPTURE_PREPARED || context->snapshot.has_first_error || context->has_stop_request)
    {
        return leave(context, xLOGIC_CAPTURE_INVALID_STATE);
    }
    if (!start_admission(context))
    {
        return leave(context,
                     failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_CONFIGURE, context->last_control_error, false));
    }
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        if (!post_read(context, &context->slots[i]))
        {
            return leave(context, context->snapshot.first_error.result);
        }
    }
    if (!capture_operation(context, 2U, false) || !accepted_start(context))
    {
        return leave(context, context->snapshot.first_error.result);
    }
    refresh(context);
    return leave(context, xLOGIC_CAPTURE_OK);
}

static bool reap_read(xLOGIC_Capture_WinUSB_Handle_t *context, Capture_Slot_t *slot)
{
    uint64_t now;
    if (!WinUsb_GetOverlappedResult(context->usb, &slot->overlapped, &slot->actual, FALSE))
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_REAP, GetLastError(), false);
        context->snapshot.first_error.has_read_sequence = true;
        context->snapshot.first_error.read_sequence = slot->sequence;
        context->snapshot.first_error.requested_bytes = slot->requested;
        return false;
    }
    slot->is_pending = false;
    slot->is_completed = true;
    now = clock_us(context);
    if (slot->actual > slot->requested || now < context->snapshot.last_read_us || now == UINT64_MAX)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_REAP, 0U, false);
        return false;
    }
    if (now - context->snapshot.last_read_us > context->snapshot.max_read_gap_us)
    {
        context->snapshot.max_read_gap_us = now - context->snapshot.last_read_us;
    }
    context->snapshot.last_read_us = slot->reaped_us = now;
    if (!add_count(context, &context->snapshot.stream_reaped_wire_bytes, slot->actual))
    {
        return false;
    }
    observe(context, xLOGIC_CAPTURE_EVENT_READ_REAP, slot->sequence, slot->actual, 0U, 0U, 0U, 0, 0U);
    if (!ResetEvent(slot->event))
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_REAP, GetLastError(), false);
        return false;
    }
    return true;
}
static Capture_Slot_t *next_completed(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    size_t i;
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        if (context->slots[i].is_completed && context->slots[i].sequence == context->next_consumption)
        {
            return &context->slots[i];
        }
    }
    return NULL;
}
static bool deliver_to_sink(xLOGIC_Capture_WinUSB_Handle_t *context,
                            Capture_Slot_t *slot,
                            bool is_end,
                            size_t *wanted,
                            xLOGIC_Capture_Sink_Result_t *result)
{
    *wanted = slot->actual;
    if (context->options.wire_limit_bytes != 0U && *wanted > context->options.wire_limit_bytes - context->snapshot.delivered_wire_bytes)
    {
        *wanted = (size_t)(context->options.wire_limit_bytes - context->snapshot.delivered_wire_bytes);
    }
    if (!is_end || *wanted != 0U)
    {
        context->is_in_callback = true;
        *result = context->options.sink(context->options.sink_user, slot->data, *wanted, slot->sequence);
        context->is_in_callback = false;
    }
    if (result->bytes_consumed > *wanted || result->flow > xLOGIC_CAPTURE_FLOW_ERROR ||
        (result->flow == xLOGIC_CAPTURE_FLOW_CONTINUE && result->bytes_consumed != *wanted))
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_SINK, 0U, false);
        return false;
    }
    return true;
}

static bool deliver_slot(xLOGIC_Capture_WinUSB_Handle_t *context, Capture_Slot_t *slot)
{
    size_t wanted;
    xLOGIC_Capture_Sink_Result_t result = {0U, xLOGIC_CAPTURE_FLOW_CONTINUE, 0U};
    const bool is_end = context->snapshot.phase == xLOGIC_CAPTURE_FINISHING && slot->actual < slot->requested;
    slot->is_visited = true;
    if (!deliver_to_sink(context, slot, is_end, &wanted, &result))
    {
        return false;
    }
    if (!complete_windows(context, clock_us(context)))
    {
        return false;
    }
    if (!add_count(context, &context->snapshot.delivered_wire_bytes, result.bytes_consumed))
    {
        return false;
    }
    if (!add_count(context, &context->window_bytes, result.bytes_consumed))
    {
        return false;
    }
    if (wanted != 0U)
    {
        ++context->snapshot.delivered_transfers;
    }
    ++context->next_consumption;
    slot->is_completed = false;
    observe(context, xLOGIC_CAPTURE_EVENT_READ_CONSUME, slot->sequence, result.bytes_consumed, 0U, 0U, 0U, 0, 0U);
    if (result.flow == xLOGIC_CAPTURE_FLOW_ERROR)
    {
        failure(context, xLOGIC_CAPTURE_SINK_ERROR, xLOGIC_CAPTURE_STAGE_SINK, 0U, false);
        context->snapshot.first_error.sink_error = result.error;
    }
    if (result.flow != xLOGIC_CAPTURE_FLOW_CONTINUE)
    {
        context->sink_requested_stop = true;
        context->has_stop_request = true;
        if (context->snapshot.phase == xLOGIC_CAPTURE_FINISHING && result.flow == xLOGIC_CAPTURE_FLOW_STOP)
        {
            failure(context, xLOGIC_CAPTURE_SINK_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, 0U, false);
        }
    }
    context->finish_end_seen = context->finish_end_seen || is_end;
    if (active(context) && !post_read(context, slot))
    {
        return false;
    }
    return active(context);
}
static bool consume_ordered(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    size_t delivered;
    for (delivered = 0U; delivered < context->snapshot.queue_depth && active(context); ++delivered)
    {
        Capture_Slot_t *slot = next_completed(context);
        if (slot == NULL)
        {
            break;
        }
        if (!deliver_slot(context, slot))
        {
            break;
        }
    }
    return active(context);
}

static void stop_statistics(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    uint64_t now;
    if (context->snapshot.has_started && !context->snapshot.has_stopped)
    {
        now = clock_us(context);
        if (now < context->snapshot.last_read_us)
        {
            failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_STOP, 0U, false);
        }
        else
        {
            complete_windows(context, now);
            if (now - context->snapshot.last_read_us > context->snapshot.max_read_gap_us)
            {
                context->snapshot.max_read_gap_us = now - context->snapshot.last_read_us;
            }
        }
        context->snapshot.acquisition_end_us = now;
        context->snapshot.has_stopped = true;
    }
}
static void begin_cleanup(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    size_t i;
    xLOGIC_Capture_Cleanup_t *cleanup = &context->snapshot.cleanup;
    cleanup->has_started = true;
    refresh(context);
    cleanup->pending_reads_at_entry = context->snapshot.pending_reads;
    cleanup->completed_unconsumed_reads_at_entry = context->snapshot.completed_reads;
    cleanup->unresolved_reads = cleanup->pending_reads_at_entry;
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        if (context->slots[i].is_completed)
        {
            cleanup->completed_unconsumed_bytes_at_entry += context->slots[i].actual;
        }
    }
    stop_statistics(context);
    context->snapshot.phase = xLOGIC_CAPTURE_STOPPING;
}
static void cleanup_read(xLOGIC_Capture_WinUSB_Handle_t *context, Capture_Slot_t *slot)
{
    ULONG actual = 0U;
    DWORD error;
    xLOGIC_Capture_Cleanup_t *cleanup = &context->snapshot.cleanup;
    if (WaitForSingleObject(slot->event, 0U) != WAIT_OBJECT_0)
    {
        return;
    }
    if (WinUsb_GetOverlappedResult(context->usb, &slot->overlapped, &actual, FALSE))
    {
        ++cleanup->successful_reads;
        cleanup->successful_bytes += actual;
        if (context->snapshot.finish_verified && actual != 0U)
        {
            failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_DRAIN, 0U, true);
        }
        observe(context, xLOGIC_CAPTURE_EVENT_CLEANUP_READ, slot->sequence, actual, 0U, context->cleanup_stage, 0U, 0, 0U);
    }
    else
    {
        error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE || error == ERROR_IO_PENDING)
        {
            return;
        }
        if (error != ERROR_OPERATION_ABORTED && !cleanup->first_unexpected_failure.is_present)
        {
            cleanup->first_unexpected_failure.is_present = true;
            cleanup->first_unexpected_failure.native_error = error;
            cleanup->first_unexpected_failure.read_sequence = slot->sequence;
            cleanup->first_unexpected_failure.requested_bytes = slot->requested;
            if (context->options.observer != NULL)
            {
                const uint64_t observed = clock_us(context);
                cleanup->first_unexpected_failure.has_timestamp = observed != UINT64_MAX;
                cleanup->first_unexpected_failure.observed_us = observed != UINT64_MAX ? observed : 0U;
            }
        }
        if (error == ERROR_OPERATION_ABORTED)
        {
            ++cleanup->canceled_reads;
        }
        else if (error == ERROR_GEN_FAILURE)
        {
            ++cleanup->failed_reads;
        }
        else
        {
            // Preserve the existing conservative unknown-status ownership rule.
            return;
        }
        observe(context, xLOGIC_CAPTURE_EVENT_CLEANUP_READ, slot->sequence, 0U, 0U, context->cleanup_stage, 0U, -1, error);
    }
    slot->is_pending = false;
    --cleanup->unresolved_reads;
}
static void inspect_cleanup_reads(xLOGIC_Capture_WinUSB_Handle_t *context, uint8_t stage, bool reap)
{
    size_t i;
    if (context->options.observer == NULL)
    {
        return;
    }
    context->cleanup_stage = stage;
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        Capture_Slot_t *slot = &context->slots[i];
        DWORD ready;
        DWORD error;
        if (!slot->is_pending)
        {
            continue;
        }
        ready = WaitForSingleObject(slot->event, 0U);
        error = ready == WAIT_FAILED ? GetLastError() : 0U;
        observe(context, xLOGIC_CAPTURE_EVENT_CLEANUP_CHECK, slot->sequence, 0U, 0U, stage, 0U,
                ready == WAIT_OBJECT_0 ? 1 : (ready == WAIT_TIMEOUT ? 0 : -1), error);
        if (reap && ready == WAIT_OBJECT_0)
        {
            cleanup_read(context, slot);
        }
    }
}
static void close_service(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (xlogic_xesk_close(&context->host) != 0)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_CLOSE, context->last_control_error, true);
    }
    context->snapshot.has_unconfirmed_control = context->snapshot.has_cleanup_error || context->host.operation_poisoned ||
                                                context->host.poisoned || (context->host.scoped && !context->host.closed);
    context->snapshot.service_retired = !context->snapshot.has_unconfirmed_control &&
                                        (!context->host.scoped || (context->host.released && context->host.reaped && context->host.closed));
    context->snapshot.phase = xLOGIC_CAPTURE_TERMINAL;
}
static void stop_native_reads(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    uint64_t now;
    inspect_cleanup_reads(context, 1U, true); // Before STOP.
    if (!context->snapshot.finish_verified)
    {
        capture_operation(context, 3U, true);     // poisoned operation is never made usable by replay.
        inspect_cleanup_reads(context, 2U, true); // After STOP, before abort.
    }
    if (!WinUsb_AbortPipe(context->usb, xLOGIC_CAPTURE_EP_IN))
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_ABORT, GetLastError(), true);
    }
    context->has_abort_attempt = true;
    inspect_cleanup_reads(context, 3U, false); // After abort; normal drain collects the results.
    now = host_clock(context);
    context->drain_deadline_ms = now <= UINT64_MAX - 1000U ? now + 1000U : now;
    context->snapshot.phase = xLOGIC_CAPTURE_DRAINING;
}
static bool drain_native_reads(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    size_t i;
    uint64_t now;
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        if (context->slots[i].is_pending)
        {
            cleanup_read(context, &context->slots[i]);
        }
    }
    refresh(context);
    if (!context->snapshot.io_terminal)
    {
        if (context->snapshot.phase == xLOGIC_CAPTURE_DRAINING && host_clock(context) >= context->drain_deadline_ms)
        {
            failure(context, xLOGIC_CAPTURE_QUARANTINED, xLOGIC_CAPTURE_STAGE_DRAIN, 0U, true);
            xlogic_xesk_close(&context->host); // once; no RELEASE while native I/O is live.
            context->snapshot.has_unconfirmed_control = true;
            context->snapshot.phase = xLOGIC_CAPTURE_RETAINED_IO;
        }
        return false;
    }
    if (context->snapshot.phase == xLOGIC_CAPTURE_RETAINED_IO)
    {
        context->snapshot.phase = xLOGIC_CAPTURE_TERMINAL;
        return false;
    }
    context->snapshot.cleanup.is_drained = true;
    if (context->snapshot.finish_verified)
    {
        capture_operation(context, 3U, true); // No native reads remain at device STOP.
    }
    if (!context->has_reset_attempt)
    {
        context->has_reset_attempt = true;
        if (!WinUsb_ResetPipe(context->usb, xLOGIC_CAPTURE_EP_IN))
        {
            failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_DRAIN, GetLastError(), true);
        }
    }
    context->retirement_step = 1U;
    now = host_clock(context);
    context->service_deadline_ms = now <= UINT64_MAX - 1000U ? now + 1000U : now;
    context->snapshot.phase = xLOGIC_CAPTURE_STOPPING;
    return true;
}
static void begin_release(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    uint64_t now = host_clock(context);
    if (context->host.operation_poisoned || context->host.release_attempted || now < context->host.last_renew_ms ||
        now - context->host.last_renew_ms >= xLOGIC_xESK_LEASE_MS || now > UINT64_MAX - 1000U)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RELEASE, 0U, true);
        close_service(context);
        return;
    }
    if (now - context->host.last_renew_ms > context->host.max_renew_gap_ms)
    {
        context->host.max_renew_gap_ms = now - context->host.last_renew_ms;
    }
    context->service_deadline_ms = now + 1000U;
    context->host.release_attempted = true;
    if (xlogic_xesk_control_until(&context->host, 4U, context->service_deadline_ms) != 0 || !xlogic_xesk_session(&context->host, false))
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RELEASE, context->last_control_error, true);
        close_service(context);
        return;
    }
    context->host.released = true;
    context->retirement_step = 2U;
}
static void status_service_step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (!read_status(context, &context->snapshot.final_status))
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_STOP, context->last_control_error, true);
        close_service(context);
        return;
    }
    context->snapshot.has_final_status = true;
    if (context->snapshot.final_status.running != 0U ||
        (context->host.scoped && !xlogic_xesk_quiescent((const uint8_t *)&context->snapshot.final_status)))
    {
        if (host_clock(context) >= context->service_deadline_ms)
        {
            failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_STOP, 0U, true);
            close_service(context);
        }
        return;
    }
    if (context->host.scoped)
    {
        begin_release(context);
    }
    else
    {
        close_service(context);
    }
}
static void release_safety_step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (host_clock(context) >= context->service_deadline_ms)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RELEASE, 0U, true);
        close_service(context);
        return;
    }
    if (context->host.terminal_flags == 0U)
    {
        if (xlogic_xesk_control_until(&context->host, 5U, context->service_deadline_ms) != 0 || !xlogic_xesk_session(&context->host, false))
        {
            failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RELEASE, context->last_control_error, true);
            close_service(context);
        }
        return;
    }
    if (xlogic_xesk_control_until(&context->host, 6U, context->service_deadline_ms) != 0)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RELEASE, context->last_control_error, true);
    }
    else
    {
        context->host.reaped = true;
        if (context->host.terminal_state != 3U)
        {
            failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RELEASE, 0U, true);
        }
    }
    close_service(context);
}
static void retire_step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (context->snapshot.phase == xLOGIC_CAPTURE_STOPPING && context->retirement_step == 0U)
    {
        stop_native_reads(context);
        return;
    }
    if (context->snapshot.phase == xLOGIC_CAPTURE_DRAINING || context->snapshot.phase == xLOGIC_CAPTURE_RETAINED_IO)
    {
        if (!drain_native_reads(context))
        {
            return;
        }
    }
    if (context->retirement_step == 1U)
    {
        status_service_step(context);
    }
    else if (context->retirement_step == 2U)
    {
        release_safety_step(context);
    }
}

static void pump_ready(xLOGIC_Capture_WinUSB_Handle_t *context, bool can_deliver)
{
    size_t i;
    for (i = 0U; i < context->snapshot.queue_depth && active(context); ++i)
    {
        Capture_Slot_t *slot = &context->slots[i];
        DWORD ready;
        if (slot->is_visited)
        {
            continue;
        }
        slot->is_visited = true;
        if (!slot->is_pending)
        {
            continue;
        }
        ready = WaitForSingleObject(slot->event, 0U);
        if (ready == WAIT_TIMEOUT)
        {
            continue;
        }
        if (ready != WAIT_OBJECT_0)
        {
            failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_REAP, GetLastError(), false);
            break;
        }
        if (!reap_read(context, slot) || (can_deliver && !consume_ordered(context)))
        {
            break;
        }
    }
}
static void renew_capture(xLOGIC_Capture_WinUSB_Handle_t *context, uint64_t due_time)
{
    uint64_t current = host_clock(context);
    const uint64_t started = clock_us(context);
    int status = -1;
    if (current >= due_time && current >= context->host.last_renew_ms && current - context->host.last_renew_ms < xLOGIC_xESK_LEASE_MS)
    {
        context->is_renewing = true;
        status = xlogic_xesk_tick(&context->host);
        context->is_renewing = false;
    }
    if (status != 0)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_RENEW, context->last_control_error, false);
    }
    else
    {
        observe(context, xLOGIC_CAPTURE_EVENT_READ_EVENTS, 0U, 0U, 0U, 0U, 0U, 0, 0U);
        observe(context, xLOGIC_CAPTURE_EVENT_RENEW_RETURN, 0U, 0U, 0U, 0U, 0U, 0, 0U);
    }
    current = clock_us(context);
    if (current < started)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_RENEW, 0U, false);
    }
    else if (current - started > context->snapshot.max_renew_duration_us)
    {
        context->snapshot.max_renew_duration_us = current - started;
    }
    if (!context->snapshot.has_first_error && !context->has_pause && context->options.diagnostic_refill_pause_ms != 0U)
    {
        context->has_pause = true;
        observe(context, xLOGIC_CAPTURE_EVENT_PAUSE_ENTER, 0U, 0U, 0U, 0U, 0U, 0, 0U);
        Sleep(context->options.diagnostic_refill_pause_ms);
        observe(context, xLOGIC_CAPTURE_EVENT_PAUSE_RETURN, 0U, 0U, 0U, 0U, 0U, 0, 0U);
    }
}
static void start_metadata_step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    xLOGIC_WINUSB_Status_t status;
    if (!read_status(context, &status))
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_START, context->last_control_error, false);
    }
    else if (usable_start_status(context, &status))
    {
        context->snapshot.start_status = status;
        context->snapshot.has_start_status = true;
        context->snapshot.phase = xLOGIC_CAPTURE_RUNNING; // return for metadata before next Step delivers.
    }
}

static bool graceful_finish_available(const xLOGIC_Capture_WinUSB_Handle_t *context)
{
    return context->snapshot.phase == xLOGIC_CAPTURE_RUNNING && context->host.scoped &&
           (context->caps.feature_flags & xLOGIC_WINUSB_CAP_GRACEFUL_FINISH) != 0U && context->options.config.sample_count == 0U &&
           context->options.wire_limit_bytes == 0U && !context->snapshot.has_first_error && !context->snapshot.is_detached &&
           !context->sink_requested_stop;
}

static void start_graceful_finish(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    const uint64_t now = clock_us(context);
    const uint64_t timeout = (uint64_t)context->options.finish_timeout_ms * UINT64_C(1000);
    if (now == UINT64_MAX || now > UINT64_MAX - timeout)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, 0U, false);
        return;
    }
    context->finish_deadline_us = now + timeout;
    if (capture_operation(context, 4U, false))
    {
        context->snapshot.phase = xLOGIC_CAPTURE_FINISHING;
    }
}

static void verify_graceful_finish(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    xLOGIC_WINUSB_Status_t status;
    const uint64_t now = clock_us(context);
    if (now == UINT64_MAX)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, 0U, false);
        return;
    }
    if (now >= context->finish_deadline_us)
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, WAIT_TIMEOUT, false);
        return;
    }
    if (!read_status(context, &status))
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, context->last_control_error, false);
        return;
    }
    const uint64_t after_status = clock_us(context);
    if (after_status == UINT64_MAX)
    {
        failure(context, xLOGIC_CAPTURE_CLOCK_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, 0U, false);
        return;
    }
    if (after_status >= context->finish_deadline_us)
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, WAIT_TIMEOUT, false);
        return;
    }
    if ((status.status_flags & xLOGIC_WINUSB_STATUS_STREAM_FINISHED) == 0U)
    {
        if (xlogic_xesk_renewal_due(&context->host, host_clock(context)))
        {
            renew_capture(context, host_clock(context));
        }
        return;
    }
    if ((status.status_flags & xLOGIC_WINUSB_STATUS_SESSION_FAULT) != 0U || status.overruns != 0U || status.usb_errors != 0U ||
        status.ownership_errors != 0U || status.timing_errors != 0U || status.produced_bytes != status.transmitted_bytes ||
        status.transmitted_bytes != context->snapshot.delivered_wire_bytes ||
        context->snapshot.stream_reaped_wire_bytes != context->snapshot.delivered_wire_bytes)
    {
        failure(context, xLOGIC_CAPTURE_PROTOCOL_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, 0U, false);
        return;
    }
    context->snapshot.finish_verified = true;
}

static void finish_stream_step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (active(context) && clock_us(context) - context->snapshot.last_read_us >= UINT64_C(5000000))
    {
        failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_REAP, WAIT_TIMEOUT, false);
    }
    if (!active(context))
    {
        if (context->snapshot.phase == xLOGIC_CAPTURE_FINISHING && !context->snapshot.has_first_error && !context->snapshot.is_detached)
        {
            if (context->finish_end_seen)
            {
                verify_graceful_finish(context);
            }
            else
            {
                failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_FINISH, WAIT_TIMEOUT, false);
            }
            if (!context->snapshot.has_first_error && !context->snapshot.finish_verified)
            {
                return;
            }
        }
        else if (graceful_finish_available(context))
        {
            start_graceful_finish(context);
            return;
        }
        stop_statistics(context);
        if (context->snapshot.has_first_error && !context->has_failure_returned)
        {
            context->has_failure_returned = true; // durable caller first-failure receipt before STOP.
        }
        else
        {
            begin_cleanup(context);
        }
    }
}
static void stream_step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    const bool is_starting = context->snapshot.phase == xLOGIC_CAPTURE_STARTING;
    size_t i;
    uint64_t due_time;
    bool is_due;
    for (i = 0U; i < context->snapshot.queue_depth; ++i)
    {
        context->slots[i].is_visited = false;
    }
    due_time = host_clock(context);
    is_due = xlogic_xesk_renewal_due(&context->host, due_time);
    if (!is_starting)
    {
        consume_ordered(context);
    }
    pump_ready(context, !is_starting);
    if (active(context) && (is_due || (context->host.scoped && due_time < context->host.last_renew_ms)))
    {
        renew_capture(context, due_time);
    }
    if (is_starting && active(context))
    {
        start_metadata_step(context);
    }
    finish_stream_step(context);
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Step(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    xLOGIC_Capture_Result_t result = enter(context);
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    switch (context->snapshot.phase)
    {
    case xLOGIC_CAPTURE_RUNNING:
    case xLOGIC_CAPTURE_STARTING:
    case xLOGIC_CAPTURE_FINISHING:
        stream_step(context);
        break;
    case xLOGIC_CAPTURE_STOPPING:
    case xLOGIC_CAPTURE_DRAINING:
    case xLOGIC_CAPTURE_RETAINED_IO:
        retire_step(context);
        break;
    case xLOGIC_CAPTURE_PREPARED:
        if (context->has_stop_request)
        {
            context->snapshot.phase = xLOGIC_CAPTURE_TERMINAL;
            context->snapshot.service_retired = true;
        }
        break;
    default:
        break;
    }
    refresh(context);
    return leave(context, xLOGIC_CAPTURE_OK);
}

xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Request_Stop(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    if (context == NULL)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    context->has_stop_request = true;
    return xLOGIC_CAPTURE_OK;
}
static uint32_t wait_budget_ms(xLOGIC_Capture_WinUSB_Handle_t *context, uint32_t maximum_ms)
{
    const bool is_stream = context->snapshot.phase == xLOGIC_CAPTURE_RUNNING || context->snapshot.phase == xLOGIC_CAPTURE_STARTING ||
                           context->snapshot.phase == xLOGIC_CAPTURE_FINISHING;
    uint64_t now, remaining;
    if (maximum_ms > 50U)
    {
        maximum_ms = 50U;
    }
    if (is_stream && context->host.scoped && context->host.claimed)
    {
        now = host_clock(context);
        remaining = now >= context->host.last_renew_ms && now - context->host.last_renew_ms < 200U
                        ? 200U - (now - context->host.last_renew_ms)
                        : 0U;
        if (remaining < maximum_ms)
        {
            maximum_ms = (uint32_t)remaining;
        }
    }
    if (!is_stream && maximum_ms > 1U)
    {
        maximum_ms = 1U;
    }
    return maximum_ms;
}
static xLOGIC_Capture_Result_t wait_native_events(xLOGIC_Capture_WinUSB_Handle_t *context, uint32_t milliseconds, bool *has_ready_work)
{
    HANDLE events[MAXIMUM_WAIT_OBJECTS];
    DWORD count = 0U, wait;
    size_t i;
    for (i = 0U; context->slots != NULL && i < context->snapshot.queue_depth; ++i)
    {
        if (context->slots[i].is_pending)
        {
            events[count++] = context->slots[i].event;
        }
    }
    if (count == 0U)
    {
        if (milliseconds > 1U)
        {
            milliseconds = 1U; // STARTING/service status progress, not an empty 50ms sleep.
        }
        if (milliseconds != 0U)
        {
            Sleep(milliseconds);
        }
        *has_ready_work = true;
        return xLOGIC_CAPTURE_OK;
    }
    wait = WaitForMultipleObjects(count, events, FALSE, milliseconds);
    if (wait == WAIT_FAILED)
    {
        return failure(context, xLOGIC_CAPTURE_NATIVE_ERROR, xLOGIC_CAPTURE_STAGE_REAP, GetLastError(), false);
    }
    *has_ready_work = wait < WAIT_OBJECT_0 + count;
    return xLOGIC_CAPTURE_OK;
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Wait(xLOGIC_Capture_WinUSB_Handle_t *context, uint32_t maximum_ms, bool *has_ready_work)
{
    xLOGIC_Capture_Result_t result = enter(context);
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    if (has_ready_work == NULL)
    {
        return leave(context, xLOGIC_CAPTURE_INVALID_ARGUMENT);
    }
    *has_ready_work = false;
    if ((context->snapshot.phase == xLOGIC_CAPTURE_RUNNING || context->snapshot.phase == xLOGIC_CAPTURE_FINISHING) &&
        next_completed(context) != NULL)
    {
        *has_ready_work = true;
        return leave(context, xLOGIC_CAPTURE_OK);
    }
    result = wait_native_events(context, wait_budget_ms(context, maximum_ms), has_ready_work);
    return leave(context, result);
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Snapshot(const xLOGIC_Capture_WinUSB_Handle_t *context, xLOGIC_Capture_Snapshot_t *snapshot)
{
    if (context == NULL || snapshot == NULL)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    if (context->is_in_api || context->is_in_callback)
    {
        return xLOGIC_CAPTURE_BUSY;
    }
    *snapshot = context->snapshot;
    return xLOGIC_CAPTURE_OK;
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Detach(xLOGIC_Capture_WinUSB_Handle_t *context)
{
    xLOGIC_Capture_Result_t result = enter(context);
    if (result != xLOGIC_CAPTURE_OK)
    {
        return result;
    }
    context->options.sink = NULL;
    context->options.sink_user = NULL;
    context->options.observer = NULL;
    context->options.observer_user = NULL;
    context->snapshot.is_detached = true;
    context->has_stop_request = true;
    return leave(context, xLOGIC_CAPTURE_OK);
}
static void close_error_set(xLOGIC_Capture_Error_t *error, DWORD native_error)
{
    if (error != NULL && error->result == xLOGIC_CAPTURE_OK)
    {
        error->result = xLOGIC_CAPTURE_NATIVE_ERROR;
        error->stage = xLOGIC_CAPTURE_STAGE_CLOSE;
        error->native_error = native_error;
        error->has_native_error = true;
    }
}
xLOGIC_Capture_Result_t xLOGIC_Capture_WinUSB_Destroy(xLOGIC_Capture_WinUSB_Handle_t **output, xLOGIC_Capture_Error_t *close_error)
{
    xLOGIC_Capture_WinUSB_Handle_t *context;
    xLOGIC_Capture_Error_t local_error;
    size_t i;
    if (output == NULL || *output == NULL)
    {
        return xLOGIC_CAPTURE_INVALID_ARGUMENT;
    }
    context = *output;
    if (context->is_in_api || context->is_in_callback)
    {
        return xLOGIC_CAPTURE_BUSY;
    }
    refresh(context);
    if (!context->snapshot.can_destroy)
    {
        return xLOGIC_CAPTURE_BUSY;
    }
    memset(&local_error, 0, sizeof(local_error));
    for (i = 0U; context->slots != NULL && i < context->snapshot.queue_depth; ++i)
    {
        if (context->slots[i].event != NULL && !CloseHandle(context->slots[i].event))
        {
            close_error_set(&local_error, GetLastError());
        }
    }
    if (context->usb != NULL && !WinUsb_Free(context->usb))
    {
        close_error_set(&local_error, GetLastError());
    }
    if (context->file != NULL && !CloseHandle(context->file))
    {
        close_error_set(&local_error, GetLastError());
    }
    free(context->slots);
    free(context->buffers);
    free(context->path);
    free(context->serial);
    free(context);
    *output = NULL;
    if (close_error != NULL)
    {
        *close_error = local_error;
    }
    return local_error.result;
}
