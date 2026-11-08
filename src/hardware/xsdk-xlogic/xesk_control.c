// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_control.c
// @brief xESK common control framing and correlation.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <string.h>
// MODULE INCLUDES
#include "xesk_control.h"
#include "xesk_control_internal.h"
#include "xesk_feature_status.h"
#include "xbytes.h"

// VARIABLES //////////////////////////////////////////////////////////////////////
static const uint8_t wire_magic[8] = {'x', 'E', 'S', 'K', 'C', 'T', 'R', 'L'};

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
bool xESK_Control_Range_Valid(const void *pointer, size_t length, size_t alignment)
{
    return (length <= UINTPTR_MAX - (uintptr_t)pointer) && (((uintptr_t)pointer % alignment) == 0U);
}

bool xESK_Control_Ranges_Disjoint(const void *left, size_t left_length, const void *right, size_t right_length)
{
    if ((left_length == 0U) || (right_length == 0U))
    {
        return true;
    }
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    return (left_start <= right_start) ? (right_start - left_start >= left_length) : (left_start - right_start >= right_length);
}

static bool encode_ranges_valid(const xESK_Control_Message_t *message, const uint8_t *buffer, size_t capacity, const size_t *written)
{
    return xESK_Control_Range_Valid(message, sizeof(*message), _Alignof(xESK_Control_Message_t)) &&
           xESK_Control_Range_Valid(buffer, capacity, 1U) && xESK_Control_Range_Valid(written, sizeof(*written), _Alignof(size_t)) &&
           xESK_Control_Ranges_Disjoint(message, sizeof(*message), buffer, capacity) &&
           xESK_Control_Ranges_Disjoint(message, sizeof(*message), written, sizeof(*written)) &&
           xESK_Control_Ranges_Disjoint(buffer, capacity, written, sizeof(*written));
}

static bool enums_valid(const xESK_Control_Message_t *message)
{
    return (message->kind >= xESK_CONTROL_REQUEST) && (message->kind <= xESK_CONTROL_RESPONSE) &&
           (message->opcode >= xESK_CONTROL_DESCRIBE) && (message->opcode <= xESK_CONTROL_FEATURE_STATUS) &&
           (message->status <= xESK_CONTROL_NOT_FOUND);
}

static xRETURN_t header_validate(const uint8_t *buffer, size_t length)
{
    if ((length < xESK_CONTROL_HEADER_BYTES) || (length > xESK_CONTROL_MAX_BYTES))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    if ((memcmp(buffer, wire_magic, sizeof(wire_magic)) != 0) || (xRead_LE16(buffer + 12U) != length) || (xRead_LE16(buffer + 14U) != 0U) ||
        (xRead_LE32(buffer + 44U) != 0U))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    if ((buffer[8] != xESK_CONTROL_VERSION) || (buffer[9] < xESK_CONTROL_REQUEST) || (buffer[9] > xESK_CONTROL_RESPONSE) ||
        (buffer[10] < xESK_CONTROL_DESCRIBE) || (buffer[10] > xESK_CONTROL_FEATURE_STATUS) || (buffer[11] > xESK_CONTROL_NOT_FOUND))
    {
        return xRETURN_xERR_xESK_UNSUPPORTED;
    }
    return xRETURN_OK;
}

static bool reply_identity_valid(const xESK_Control_Message_t *request, const xESK_Control_Message_t *response)
{
    if ((request->request_id != response->request_id) || (request->opcode != response->opcode) ||
        (request->service_id != response->service_id) || (request->instance_id != response->instance_id))
    {
        return false;
    }
    if (((request->opcode != xESK_CONTROL_CLAIM) || (response->status != xESK_CONTROL_OK)) && (request->session_id != response->session_id))
    {
        return false;
    }
    return (request->boot_id == 0U) || (response->status == xESK_CONTROL_STALE) || (request->boot_id == response->boot_id);
}

static bool reply_payload_valid(const xESK_Control_Message_t *request, const xESK_Control_Message_t *response)
{
    if (response->status != xESK_CONTROL_OK)
    {
        return true;
    }
    if (request->opcode == xESK_CONTROL_DESCRIBE)
    {
        return request->descriptor_kind == response->record.kind;
    }

    if (request->opcode == xESK_CONTROL_FEATURE_STATUS)
    {
        xESK_Feature_Query_t query;
        xESK_Feature_Page_t page;
        const xRETURN_t decoded = xESK_Feature_Query_Decode(request->payload.data, request->payload.length, &query);
        if (decoded != xRETURN_OK)
        {
            return false;
        }
        const xRETURN_t result = xESK_Feature_Page_Decode(response->payload.data, response->payload.length, &page);
        if (result != xRETURN_OK)
        {
            return false;
        }
        return xESK_Feature_Reply_Validate(&query, &page) == xRETURN_OK;
    }
    return (request->opcode != xESK_CONTROL_CLAIM) || (request->expiry_policy == response->record.data.session.expiry_policy);
}

// PUBLIC API IMPLEMENTATION /////////////////////////////////////////////////////
xRETURN_t xESK_Control_Validate(const xESK_Control_Message_t *message)
{
    if (message == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!xESK_Control_Range_Valid(message, sizeof(*message), _Alignof(xESK_Control_Message_t)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (!enums_valid(message) || (message->request_id == 0U))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (((message->kind == xESK_CONTROL_REQUEST) && (message->status != xESK_CONTROL_OK)) ||
        ((message->kind == xESK_CONTROL_RESPONSE) && (message->boot_id == 0U)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return xESK_Control_Payload_Validate(message);
}

xRETURN_t xESK_Control_Encode(const xESK_Control_Message_t *message, uint8_t *buffer, size_t capacity, size_t *written)
{
    if ((message == NULL) || (buffer == NULL) || (written == NULL))
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!encode_ranges_valid(message, buffer, capacity, written))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xRETURN_t status = xESK_Control_Validate(message);
    if (status != xRETURN_OK)
    {
        return status;
    }
    const size_t length = xESK_CONTROL_HEADER_BYTES + xESK_Control_Payload_Size(message);
    if (capacity < length)
    {
        return xRETURN_xERR_xESK_OVERFLOW;
    }
    uint8_t frame[xESK_CONTROL_MAX_BYTES] = {0};
    (void)memcpy(frame, wire_magic, sizeof(wire_magic));
    frame[8] = xESK_CONTROL_VERSION;
    frame[9] = message->kind;
    frame[10] = message->opcode;
    frame[11] = message->status;
    xWrite_LE16(frame + 12U, (uint16_t)length);
    xWrite_LE64(frame + 16U, message->request_id);
    xWrite_LE64(frame + 24U, message->boot_id);
    xWrite_LE64(frame + 32U, message->session_id);
    xWrite_LE16(frame + 40U, message->service_id);
    xWrite_LE16(frame + 42U, message->instance_id);
    status = xESK_Control_Payload_Encode(message, frame + xESK_CONTROL_HEADER_BYTES);
    if (status != xRETURN_OK)
    {
        return status;
    }
    (void)memcpy(buffer, frame, length);
    *written = length;
    return xRETURN_OK;
}

xRETURN_t xESK_Control_Decode(const uint8_t *buffer, size_t length, xESK_Control_Message_t *message)
{
    if ((buffer == NULL) || (message == NULL))
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!xESK_Control_Range_Valid(buffer, length, 1U) ||
        !xESK_Control_Range_Valid(message, sizeof(*message), _Alignof(xESK_Control_Message_t)) ||
        !xESK_Control_Ranges_Disjoint(buffer, length, message, sizeof(*message)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xRETURN_t status = header_validate(buffer, length);
    if (status != xRETURN_OK)
    {
        return status;
    }
    xESK_Control_Message_t candidate = {0};
    candidate.kind = buffer[9];
    candidate.opcode = buffer[10];
    candidate.status = buffer[11];
    candidate.request_id = xRead_LE64(buffer + 16U);
    candidate.boot_id = xRead_LE64(buffer + 24U);
    candidate.session_id = xRead_LE64(buffer + 32U);
    candidate.service_id = xRead_LE16(buffer + 40U);
    candidate.instance_id = xRead_LE16(buffer + 42U);
    status = xESK_Control_Payload_Decode(buffer + xESK_CONTROL_HEADER_BYTES, length - xESK_CONTROL_HEADER_BYTES, &candidate);
    if (status != xRETURN_OK)
    {
        return status;
    }
    status = xESK_Control_Validate(&candidate);
    if (status != xRETURN_OK)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *message = candidate;
    return xRETURN_OK;
}

xRETURN_t xESK_Control_Reply_Validate(const xESK_Control_Message_t *request, const xESK_Control_Message_t *response)
{
    if ((request == NULL) || (response == NULL))
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!xESK_Control_Range_Valid(request, sizeof(*request), _Alignof(xESK_Control_Message_t)) ||
        !xESK_Control_Range_Valid(response, sizeof(*response), _Alignof(xESK_Control_Message_t)) ||
        !xESK_Control_Ranges_Disjoint(request, sizeof(*request), response, sizeof(*response)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xRETURN_t status = xESK_Control_Validate(request);
    if (status != xRETURN_OK)
    {
        return status;
    }
    status = xESK_Control_Validate(response);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if ((request->kind != xESK_CONTROL_REQUEST) || (response->kind != xESK_CONTROL_RESPONSE) || !reply_identity_valid(request, response))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (!reply_payload_valid(request, response))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return xRETURN_OK;
}

// EOF /////////////////////////////////////////////////////////////////////////////
