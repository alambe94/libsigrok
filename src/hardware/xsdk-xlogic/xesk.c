// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk.c
// @brief Transactional ESK descriptor framing.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stdbool.h>
#include <string.h>
// MODULE INCLUDES
#include "xesk.h"
#include "xesk_internal.h"
#include "xbytes.h"

// VARIABLES //////////////////////////////////////////////////////////////////////
static const uint8_t wire_magic[4] = {'x', 'E', 'S', 'K'};

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static bool range_valid(const void *pointer, size_t length)
{
    return length <= UINTPTR_MAX - (uintptr_t)pointer;
}
static bool ranges_disjoint(const void *left, size_t left_length, const void *right, size_t right_length)
{
    if ((left_length == 0U) || (right_length == 0U))
    {
        return true;
    }
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    return (left_start <= right_start) ? (right_start - left_start >= left_length) : (left_start - right_start >= right_length);
}
static bool encode_ranges_valid(const xESK_Record_t *record, uint8_t *buffer, size_t capacity, size_t *written)
{
    return range_valid(record, sizeof(*record)) && range_valid(buffer, capacity) && range_valid(written, sizeof(*written)) &&
           ranges_disjoint(record, sizeof(*record), buffer, capacity) &&
           ranges_disjoint(record, sizeof(*record), written, sizeof(*written)) &&
           ranges_disjoint(buffer, capacity, written, sizeof(*written));
}
static xRETURN_t header_validate(const uint8_t *buffer, size_t length)
{
    if ((length < xESK_HEADER_BYTES) || (length > xESK_MAX_FRAME_BYTES))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    const size_t declared_length = xRead_LE16(buffer + 8U);
    if ((memcmp(buffer, wire_magic, sizeof(wire_magic)) != 0) || ((buffer[6] | buffer[7] | buffer[10] | buffer[11]) != 0U) ||
        (length != declared_length))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    if ((buffer[4] != xESK_WIRE_VERSION) || (xESK_Payload_Size(buffer[5]) == 0U))
    {
        return xRETURN_xERR_xESK_UNSUPPORTED;
    }
    if (length != xESK_HEADER_BYTES + xESK_Payload_Size(buffer[5]))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    return xRETURN_OK;
}

// PUBLIC API IMPLEMENTATION /////////////////////////////////////////////////////
xRETURN_t xESK_Record_Encode(const xESK_Record_t *record, uint8_t *buffer, size_t capacity, size_t *written)
{
    if ((record == NULL) || (buffer == NULL) || (written == NULL))
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!encode_ranges_valid(record, buffer, capacity, written))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    const xRETURN_t status = xESK_Record_Validate(record);
    if (status != xRETURN_OK)
    {
        return status;
    }
    const size_t length = xESK_HEADER_BYTES + xESK_Payload_Size(record->kind);
    if (capacity < length)
    {
        return xRETURN_xERR_xESK_OVERFLOW;
    }
    uint8_t frame[xESK_MAX_FRAME_BYTES] = {0};
    (void)memcpy(frame, wire_magic, sizeof(wire_magic));
    frame[4] = xESK_WIRE_VERSION;
    frame[5] = record->kind;
    xWrite_LE16(frame + 8U, (uint16_t)length);
    xESK_Payload_Encode(record, frame + xESK_HEADER_BYTES);
    (void)memcpy(buffer, frame, length);
    *written = length;
    return xRETURN_OK;
}

xRETURN_t xESK_Record_Decode(const uint8_t *buffer, size_t length, xESK_Record_t *record)
{
    if ((buffer == NULL) || (record == NULL))
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!range_valid(buffer, length) || !range_valid(record, sizeof(*record)) || !ranges_disjoint(buffer, length, record, sizeof(*record)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    const xRETURN_t status = header_validate(buffer, length);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!xESK_Payload_Reserved_Valid(buffer[5], buffer + xESK_HEADER_BYTES))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    xESK_Record_t candidate = {0};
    candidate.kind = buffer[5];
    xESK_Payload_Decode(buffer + xESK_HEADER_BYTES, &candidate);
    const xRETURN_t model_status = xESK_Record_Validate(&candidate);
    if (model_status != xRETURN_OK)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *record = candidate;
    return xRETURN_OK;
}

// EOF /////////////////////////////////////////////////////////////////////////////
