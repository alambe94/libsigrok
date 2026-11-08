/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#include "xesk_network_session.h"
#include "xesk_control_internal.h"
#include <string.h>

static bool disjoint(const void *a, size_t a_size, const void *b, size_t b_size)
{
    return xESK_Control_Range_Valid(a, a_size, 1U) && xESK_Control_Range_Valid(b, b_size, 1U) &&
           xESK_Control_Ranges_Disjoint(a, a_size, b, b_size);
}

static bool request_allowed(const xSDK_xESK_Network_Session_t *session, const xESK_Control_Message_t *request)
{
    if (request->opcode == xESK_CONTROL_DESCRIBE)
    {
        return session->boot_id != 0U || (request->descriptor_kind == xESK_RECORD_DEVICE && request->index == 0U);
    }
    if (request->opcode == xESK_CONTROL_CLAIM)
    {
        return session->boot_id != 0U && session->has_finite_logic && session->session_id == 0U &&
               request->expiry_policy == xESK_EXPIRY_DEADLINE;
    }
    if (session->session_id == 0U)
    {
        return false;
    }
    if (session->retiring)
    {
        return request->opcode == xESK_CONTROL_RELEASE || (request->opcode == xESK_CONTROL_REAP && session->safe_to_reap);
    }
    return request->opcode == xESK_CONTROL_RENEW || request->opcode == xESK_CONTROL_RELEASE || request->opcode == xESK_CONTROL_SERVICE;
}

static bool request_storage_valid(const xSDK_xESK_Network_Session_t *session,
                                  const xESK_Control_Message_t *request,
                                  const uint8_t *wire,
                                  size_t capacity,
                                  const size_t *written)
{
    return xESK_Control_Range_Valid(session, sizeof(*session), _Alignof(xSDK_xESK_Network_Session_t)) &&
           xESK_Control_Range_Valid(request, sizeof(*request), _Alignof(xESK_Control_Message_t)) &&
           xESK_Control_Range_Valid(written, sizeof(*written), _Alignof(size_t)) &&
           disjoint(session, sizeof(*session), request, sizeof(*request)) && disjoint(session, sizeof(*session), wire, capacity) &&
           disjoint(session, sizeof(*session), written, sizeof(*written)) && disjoint(request, sizeof(*request), wire, capacity) &&
           disjoint(request, sizeof(*request), written, sizeof(*written)) && disjoint(wire, capacity, written, sizeof(*written));
}

static xRETURN_t logic_request_check(const xSDK_xESK_Network_Session_t *session, const xESK_Control_Message_t *request)
{
    xESK_Logic_Request_t logic = {0};
    const xRETURN_t status = xESK_Logic_Network_Request_Decode(request, &logic);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (logic.opcode == xLOGIC_NET_OP_START)
    {
        return session->has_capture ? xRETURN_xERR_xESK_INVALID_STATE : xRETURN_OK;
    }
    if (!session->has_capture || logic.capture_id != session->capture.capture_id)
    {
        return xRETURN_xERR_xESK_INVALID_STATE;
    }
    return (logic.opcode == xLOGIC_NET_OP_READ || logic.opcode == xLOGIC_NET_OP_READ_BLOCK) && !session->capture_ready
               ? xRETURN_xERR_xESK_INVALID_STATE
               : xRETURN_OK;
}

xRETURN_t xSDK_xESK_Network_Request(xSDK_xESK_Network_Session_t *session,
                                    const xESK_Control_Message_t *request,
                                    uint8_t *wire,
                                    size_t capacity,
                                    size_t *written)
{
    if (!request_storage_valid(session, request, wire, capacity, written))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (session->poisoned || session->has_pending || session->has_block || session->request_id == UINT64_MAX ||
        !request_allowed(session, request))
    {
        return xRETURN_xERR_xESK_INVALID_STATE;
    }
    xESK_Control_Message_t candidate = *request;
    candidate.kind = xESK_CONTROL_REQUEST;
    candidate.request_id = session->request_id + 1U;
    candidate.boot_id = session->boot_id;
    candidate.session_id = candidate.opcode == xESK_CONTROL_DESCRIBE || candidate.opcode == xESK_CONTROL_CLAIM ? 0U : session->session_id;
    if (candidate.opcode != xESK_CONTROL_DESCRIBE)
    {
        candidate.service_id = xESK_SERVICE_LOGIC;
        candidate.instance_id = 0U;
    }
    if (candidate.opcode == xESK_CONTROL_SERVICE)
    {
        const xRETURN_t decoded = logic_request_check(session, &candidate);
        if (decoded != xRETURN_OK)
        {
            return decoded;
        }
    }
    const xRETURN_t status = xESK_Control_Encode(&candidate, wire, capacity, written);
    if (status == xRETURN_OK)
    {
        session->pending = candidate;
        session->request_id = candidate.request_id;
        session->has_pending = true;
    }
    return status;
}

static bool session_reply_valid(const xESK_Control_Message_t *reply)
{
    const xESK_Session_t *record = &reply->record.data.session;
    return record->transport == xESK_TRANSPORT_NETWORK && record->owner_kind == xESK_OWNER_SDK &&
           record->service_id == xESK_SERVICE_LOGIC && record->instance_id == 0U;
}

static bool device_matches(const xESK_Device_t *expected, const xESK_Device_t *actual)
{
    return memcmp(expected->device_id, actual->device_id, sizeof(expected->device_id)) == 0 && expected->boot_id == actual->boot_id &&
           expected->firmware_version == actual->firmware_version && expected->hardware_id == actual->hardware_id &&
           expected->transports == actual->transports && expected->timebase_id == actual->timebase_id;
}

static bool capture_matches(const xESK_Logic_Metadata_t *expected, const xESK_Logic_Metadata_t *actual, bool timestamp_known)
{
    return expected->capture_id == actual->capture_id && expected->sample_rate == actual->sample_rate &&
           expected->sample_count == actual->sample_count && expected->stored_bytes == actual->stored_bytes &&
           expected->format == actual->format && expected->channel_mask == actual->channel_mask &&
           (!timestamp_known || (expected->timestamp_ticks == actual->timestamp_ticks && expected->timestamp_hz == actual->timestamp_hz));
}

typedef struct
{
    xESK_Logic_Request_t request;
    xESK_Logic_Metadata_t metadata;
    bool has_block;
} Service_Reply_t;

static bool control_reply_valid(const xSDK_xESK_Network_Session_t *session, const xESK_Control_Message_t *reply)
{
    if (session->boot_id != 0U && reply->boot_id != session->boot_id)
    {
        return false;
    }
    if (reply->status != xESK_CONTROL_OK)
    {
        return true;
    }
    if (reply->opcode == xESK_CONTROL_CLAIM || reply->opcode == xESK_CONTROL_RENEW || reply->opcode == xESK_CONTROL_RELEASE)
    {
        if (!session_reply_valid(reply))
        {
            return false;
        }
        const xESK_Session_t *record = &reply->record.data.session;
        if (reply->opcode == xESK_CONTROL_CLAIM)
        {
            return reply->session_id > session->highest_session_id && record->timebase_id == session->logic_timebase_id;
        }
        return record->timebase_id == session->session_timebase_id && record->expiry_policy == xESK_EXPIRY_DEADLINE;
    }
    if (reply->opcode == xESK_CONTROL_DESCRIBE && reply->record.kind == xESK_RECORD_DEVICE)
    {
        const xESK_Device_t *device = &reply->record.data.device;
        return (device->transports & xESK_TRANSPORT_NETWORK) != 0U && (session->boot_id == 0U || device_matches(&session->device, device));
    }
    return true;
}

static bool service_metadata_valid(const xSDK_xESK_Network_Session_t *session, const Service_Reply_t *service)
{
    const uint32_t operation = service->request.opcode;
    const xESK_Logic_Metadata_t *actual = &service->metadata;
    if (actual->status != xLOGIC_NET_STATUS_OK || operation == xLOGIC_NET_OP_START || operation == xLOGIC_NET_OP_STOP ||
        operation == xLOGIC_NET_OP_RELEASE)
    {
        return true;
    }
    return capture_matches(&session->capture, actual, session->capture_ready) &&
           (!session->capture_ready || (actual->flags & xLOGIC_NET_FLAG_READY) != 0U);
}

static xRETURN_t
decode_service_reply(const xSDK_xESK_Network_Session_t *session, const xESK_Control_Message_t *reply, Service_Reply_t *service)
{
    xRETURN_t status = xESK_Logic_Network_Request_Decode(&session->pending, &service->request);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (service->request.opcode == xLOGIC_NET_OP_READ_BLOCK)
    {
        status = xESK_Logic_Network_Block_Header_Decode(&session->pending, reply, &service->metadata);
        service->has_block = status == xRETURN_OK;
    }
    else
    {
        xESK_Logic_Network_Reply_t result = {0};
        status = xESK_Logic_Network_Reply_Decode(&session->pending, reply, &result);
        service->metadata = result.header;
    }
    return status == xRETURN_OK && !service_metadata_valid(session, service) ? xRETURN_xERR_xESK_CORRUPT : status;
}

static void update_capture(xSDK_xESK_Network_Session_t *session, const Service_Reply_t *service)
{
    const xESK_Logic_Metadata_t *actual = &service->metadata;
    const uint32_t operation = service->request.opcode;
    if (actual->status != xLOGIC_NET_STATUS_OK)
    {
        return;
    }
    if (operation == xLOGIC_NET_OP_START)
    {
        session->capture = *actual;
        session->has_capture = true;
    }
    if ((actual->flags & xLOGIC_NET_FLAG_READY) != 0U && !session->capture_ready)
    {
        session->capture = *actual;
        session->capture_ready = true;
    }
    if (operation == xLOGIC_NET_OP_STOP || operation == xLOGIC_NET_OP_RELEASE)
    {
        session->has_capture = false;
        session->capture_ready = false;
    }
}

static void update_description(xSDK_xESK_Network_Session_t *session, const xESK_Record_t *record)
{
    if (record->kind == xESK_RECORD_DEVICE)
    {
        session->boot_id = record->data.device.boot_id;
        session->device = record->data.device;
    }
    else if (record->kind == xESK_RECORD_SERVICE && record->data.service.service_id == xESK_SERVICE_LOGIC &&
             record->data.service.instance_id == 0U)
    {
        const xESK_Service_t *service = &record->data.service;
        session->has_finite_logic = (service->transports & xESK_TRANSPORT_NETWORK) != 0U && (service->flags & xESK_SERVICE_FINITE) != 0U &&
                                    service->max_request_bytes >= xESK_LOGIC_CAPTURE_REQUEST_BYTES &&
                                    service->max_reply_bytes >= xESK_LOGIC_CAPTURE_RESPONSE_BYTES;
        session->logic_timebase_id = session->has_finite_logic ? service->timebase_id : 0U;
        session->logic_service = session->has_finite_logic ? *service : (xESK_Service_t){0};
    }
}

static void update_session(xSDK_xESK_Network_Session_t *session, const xESK_Control_Message_t *reply, const Service_Reply_t *service)
{
    if (reply->status != xESK_CONTROL_OK)
    {
        return;
    }
    switch (reply->opcode)
    {
    case xESK_CONTROL_DESCRIBE:
        update_description(session, &reply->record);
        break;
    case xESK_CONTROL_CLAIM:
        session->session_id = reply->session_id;
        session->highest_session_id = reply->session_id;
        session->session_timebase_id = reply->record.data.session.timebase_id;
        break;
    case xESK_CONTROL_RELEASE:
        session->retiring = true;
        session->safe_to_reap = (reply->record.data.session.flags & xESK_SESSION_SAFE_STATE_CONFIRMED) != 0U;
        break;
    case xESK_CONTROL_REAP:
        session->session_id = 0U;
        session->session_timebase_id = 0U;
        session->safe_to_reap = false;
        session->retiring = false;
        session->has_capture = false;
        session->capture_ready = false;
        break;
    case xESK_CONTROL_SERVICE:
        update_capture(session, service);
        break;
    default:
        break;
    }
}

static bool
reply_storage_valid(const xSDK_xESK_Network_Session_t *session, const uint8_t *wire, size_t length, const xESK_Control_Message_t *reply)
{
    return xESK_Control_Range_Valid(session, sizeof(*session), _Alignof(xSDK_xESK_Network_Session_t)) &&
           xESK_Control_Range_Valid(reply, sizeof(*reply), _Alignof(xESK_Control_Message_t)) &&
           disjoint(session, sizeof(*session), wire, length) && disjoint(session, sizeof(*session), reply, sizeof(*reply)) &&
           disjoint(wire, length, reply, sizeof(*reply));
}

xRETURN_t xSDK_xESK_Network_Reply(xSDK_xESK_Network_Session_t *session, const uint8_t *wire, size_t length, xESK_Control_Message_t *reply)
{
    if (!reply_storage_valid(session, wire, length, reply))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (session->poisoned || !session->has_pending)
    {
        return xRETURN_xERR_xESK_INVALID_STATE;
    }
    xESK_Control_Message_t candidate = {0};
    xRETURN_t status = xESK_Control_Decode(wire, length, &candidate);
    if (status == xRETURN_OK)
    {
        status = xESK_Control_Reply_Validate(&session->pending, &candidate);
    }
    if (status == xRETURN_OK && !control_reply_valid(session, &candidate))
    {
        status = xRETURN_xERR_xESK_CORRUPT;
    }
    Service_Reply_t service = {0};
    if (status == xRETURN_OK && candidate.status == xESK_CONTROL_OK && candidate.opcode == xESK_CONTROL_SERVICE)
    {
        status = decode_service_reply(session, &candidate, &service);
    }
    if (status != xRETURN_OK)
    {
        session->poisoned = true;
        return xRETURN_xERR_xESK_CORRUPT;
    }
    update_session(session, &candidate, &service);
    session->has_pending = false;
    session->has_block = service.has_block;
    session->block_bytes = service.has_block ? service.metadata.payload_bytes : 0U;
    session->block_crc32 = service.has_block ? service.metadata.payload_crc32 : 0U;
    *reply = candidate;
    return xRETURN_OK;
}

xRETURN_t xSDK_xESK_Network_Block(xSDK_xESK_Network_Session_t *session, const uint8_t *data, size_t length)
{
    if (!xESK_Control_Range_Valid(session, sizeof(*session), _Alignof(xSDK_xESK_Network_Session_t)) ||
        !disjoint(session, sizeof(*session), data, length))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (session->poisoned || !session->has_block)
    {
        return xRETURN_xERR_xESK_INVALID_STATE;
    }
    if (length != session->block_bytes || xLOGIC_NET_CRC32(data, length) != session->block_crc32)
    {
        session->poisoned = true;
        return xRETURN_xERR_xESK_CORRUPT;
    }
    session->has_block = false;
    session->block_bytes = 0U;
    session->block_crc32 = 0U;
    return xRETURN_OK;
}

void xSDK_xESK_Network_Poison(xSDK_xESK_Network_Session_t *session)
{
    if (session != NULL)
    {
        session->poisoned = true;
    }
}
