// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_control_payload.c
// @brief xESK control payload validation and conversion.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <string.h>
// MODULE INCLUDES
#include "xesk_control_internal.h"
#include "xesk_feature_status.h"
#include "xbytes.h"

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static bool identity_valid(const xESK_Control_Message_t *message)
{
    if (message->opcode == xESK_CONTROL_DESCRIBE || message->opcode == xESK_CONTROL_FEATURE_STATUS)
    {
        return (message->session_id == 0U) && (message->service_id == 0U) && (message->instance_id == 0U) &&
               (message->opcode != xESK_CONTROL_FEATURE_STATUS || message->boot_id != 0U);
    }
    if ((message->boot_id == 0U) || (message->service_id < xESK_SERVICE_DEVICE) || (message->service_id > xESK_SERVICE_RS485))
    {
        return false;
    }
    if (message->opcode == xESK_CONTROL_CLAIM)
    {
        const bool assigned = (message->kind == xESK_CONTROL_RESPONSE) && (message->status == xESK_CONTROL_OK);
        return (message->session_id != 0U) == assigned;
    }
    return message->session_id != 0U;
}

static bool session_valid(const xESK_Control_Message_t *message)
{
    const xESK_Session_t *session = &message->record.data.session;
    if ((message->record.kind != xESK_RECORD_SESSION) || (session->boot_id != message->boot_id) ||
        (session->session_id != message->session_id) || (session->service_id != message->service_id) ||
        (session->instance_id != message->instance_id))
    {
        return false;
    }
    if ((message->opcode == xESK_CONTROL_CLAIM) || (message->opcode == xESK_CONTROL_RENEW))
    {
        if ((session->state != xESK_SESSION_ACTIVE) || (session->flags != 0U))
        {
            return false;
        }
    }
    if ((message->opcode == xESK_CONTROL_RENEW) && (session->expiry_policy != xESK_EXPIRY_DEADLINE))
    {
        return false;
    }
    return (message->opcode != xESK_CONTROL_RELEASE) || (session->state != xESK_SESSION_ACTIVE);
}

static xRETURN_t record_validate(const xESK_Control_Message_t *message)
{
    const xRETURN_t status = xESK_Record_Validate(&message->record);
    if (status != xRETURN_OK)
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    bool valid = false;
    if (message->opcode == xESK_CONTROL_DESCRIBE)
    {
        valid = (message->record.kind >= xESK_RECORD_DEVICE) && (message->record.kind <= xESK_RECORD_REQUIREMENT);
        if (message->record.kind == xESK_RECORD_DEVICE)
        {
            valid = message->record.data.device.boot_id == message->boot_id;
        }
    }
    else
    {
        valid = session_valid(message);
    }
    return valid ? xRETURN_OK : xRETURN_xERR_xESK_INVALID_ARG;
}

static bool request_payload_valid(const xESK_Control_Message_t *message)
{
    if (message->opcode == xESK_CONTROL_DESCRIBE)
    {
        return (message->descriptor_kind >= xESK_RECORD_DEVICE) && (message->descriptor_kind <= xESK_RECORD_REQUIREMENT) &&
               ((message->descriptor_kind != xESK_RECORD_DEVICE) || (message->index == 0U)) && (message->expiry_policy == 0U) &&
               (message->lease_ticks == 0U);
    }
    if ((message->descriptor_kind != 0U) || (message->index != 0U))
    {
        return false;
    }
    if (message->opcode == xESK_CONTROL_CLAIM)
    {
        return (message->lease_ticks != 0U) &&
               ((message->expiry_policy == xESK_EXPIRY_DEADLINE) || (message->expiry_policy == xESK_EXPIRY_PROTOCOL_LIFETIME));
    }
    const bool needs_lease = message->opcode == xESK_CONTROL_RENEW;
    return (message->expiry_policy == 0U) && ((message->lease_ticks != 0U) == needs_lease);
}

static bool payload_reserved_valid(const uint8_t *payload, size_t length, uint8_t opcode)
{
    if ((length == 0U) || (opcode == xESK_CONTROL_RENEW))
    {
        return true;
    }
    const size_t reserved_end = (opcode == xESK_CONTROL_DESCRIBE) ? 4U : 8U;
    for (size_t index = 1U; index < reserved_end; ++index)
    {
        if (payload[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

static bool service_payload_valid(const xESK_Control_Message_t *message)
{
    return !message->has_record && message->descriptor_kind == 0U && message->index == 0U && message->expiry_policy == 0U &&
           message->lease_ticks == 0U && message->payload.length <= xESK_MAX_FRAME_BYTES &&
           (message->kind == xESK_CONTROL_REQUEST || message->status == xESK_CONTROL_OK || message->payload.length == 0U);
}

static xRETURN_t response_payload_validate(const xESK_Control_Message_t *message)
{
    if (message->descriptor_kind != 0U || message->index != 0U || message->expiry_policy != 0U || message->lease_ticks != 0U)
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return message->has_record ? record_validate(message) : xRETURN_OK;
}

static xRETURN_t feature_payload_validate(const xESK_Control_Message_t *message)
{
    if (!service_payload_valid(message))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (message->kind == xESK_CONTROL_RESPONSE && message->status != xESK_CONTROL_OK)
    {
        return xRETURN_OK;
    }
    if (message->kind == xESK_CONTROL_REQUEST)
    {
        xESK_Feature_Query_t query;
        return xESK_Feature_Query_Decode(message->payload.data, message->payload.length, &query);
    }
    xESK_Feature_Page_t page;
    return xESK_Feature_Page_Decode(message->payload.data, message->payload.length, &page);
}

// EXPORTED FUNCTIONS IMPLEMENTATION //////////////////////////////////////////////
xRETURN_t xESK_Control_Payload_Validate(const xESK_Control_Message_t *message)
{
    if (!identity_valid(message))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (message->opcode == xESK_CONTROL_SERVICE)
    {
        return service_payload_valid(message) ? xRETURN_OK : xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (message->opcode == xESK_CONTROL_FEATURE_STATUS)
    {
        return feature_payload_validate(message);
    }

    const bool needs_record =
        (message->kind == xESK_CONTROL_RESPONSE) && (message->status == xESK_CONTROL_OK) && (message->opcode != xESK_CONTROL_REAP);
    if ((message->has_record != needs_record) || (!needs_record && (message->record.kind != 0U)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (message->kind == xESK_CONTROL_REQUEST)
    {
        return request_payload_valid(message) ? xRETURN_OK : xRETURN_xERR_xESK_INVALID_ARG;
    }
    return response_payload_validate(message);
}

size_t xESK_Control_Payload_Size(const xESK_Control_Message_t *message)
{
    if (message->opcode == xESK_CONTROL_SERVICE || message->opcode == xESK_CONTROL_FEATURE_STATUS)
    {
        return message->payload.length;
    }
    if (message->has_record)
    {
        switch (message->record.kind)
        {
        case xESK_RECORD_SERVICE:
            return 64U;
        case xESK_RECORD_RESOURCE:
            return 48U;
        default:
            return 52U;
        }
    }
    if (message->kind == xESK_CONTROL_REQUEST)
    {
        switch (message->opcode)
        {
        case xESK_CONTROL_DESCRIBE:
        case xESK_CONTROL_RENEW:
            return 8U;
        case xESK_CONTROL_CLAIM:
            return 16U;
        default:
            return 0U;
        }
    }
    return 0U;
}

xRETURN_t xESK_Control_Payload_Encode(const xESK_Control_Message_t *message, uint8_t *payload)
{
    if (message->opcode == xESK_CONTROL_SERVICE || message->opcode == xESK_CONTROL_FEATURE_STATUS)
    {
        (void)memcpy(payload, message->payload.data, message->payload.length);
        return xRETURN_OK;
    }
    if (message->has_record)
    {
        size_t written = 0U;
        return xESK_Record_Encode(&message->record, payload, xESK_MAX_FRAME_BYTES, &written);
    }
    if (message->kind != xESK_CONTROL_REQUEST)
    {
        return xRETURN_OK;
    }
    switch (message->opcode)
    {
    case xESK_CONTROL_DESCRIBE:
        payload[0] = message->descriptor_kind;
        xWrite_LE32(payload + 4U, message->index);
        break;
    case xESK_CONTROL_CLAIM:
        payload[0] = message->expiry_policy;
        xWrite_LE64(payload + 8U, message->lease_ticks);
        break;
    case xESK_CONTROL_RENEW:
        xWrite_LE64(payload, message->lease_ticks);
        break;
    default:
        break;
    }
    return xRETURN_OK;
}

xRETURN_t xESK_Control_Payload_Decode(const uint8_t *payload, size_t length, xESK_Control_Message_t *message)
{
    if (message->opcode == xESK_CONTROL_SERVICE || message->opcode == xESK_CONTROL_FEATURE_STATUS)
    {
        if (length > xESK_MAX_FRAME_BYTES)
        {
            return xRETURN_xERR_xESK_CORRUPT;
        }
        message->payload.length = length;
        (void)memcpy(message->payload.data, payload, length);
        return xRETURN_OK;
    }
    if ((message->kind == xESK_CONTROL_RESPONSE) && (message->status == xESK_CONTROL_OK) && (message->opcode != xESK_CONTROL_REAP))
    {
        const xRETURN_t status = xESK_Record_Decode(payload, length, &message->record);
        message->has_record = status == xRETURN_OK;
        return status;
    }
    const size_t expected = xESK_Control_Payload_Size(message);
    if (length != expected)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    if (!payload_reserved_valid(payload, length, message->opcode))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    switch (message->opcode)
    {
    case xESK_CONTROL_DESCRIBE:
        if (length != 0U)
        {
            message->descriptor_kind = payload[0];
            message->index = xRead_LE32(payload + 4U);
        }
        break;
    case xESK_CONTROL_CLAIM:
        if (length != 0U)
        {
            message->expiry_policy = payload[0];
            message->lease_ticks = xRead_LE64(payload + 8U);
        }
        break;
    case xESK_CONTROL_RENEW:
        if (length != 0U)
        {
            message->lease_ticks = xRead_LE64(payload);
        }
        break;
    default:
        break;
    }
    return xRETURN_OK;
}

// EOF /////////////////////////////////////////////////////////////////////////////
