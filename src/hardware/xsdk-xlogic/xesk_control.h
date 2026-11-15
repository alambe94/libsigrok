// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_control.h
// @brief xESK common control records and bounded wire codec.

#ifndef xESK_CONTROL_H
#define xESK_CONTROL_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// MODULE INCLUDES
#include "xesk.h"
#ifdef __cplusplus
extern "C"
{
#endif

// PUBLIC API /////////////////////////////////////////////////////////

// MACROS /////////////////////////////////////////////////////////////////////////
#define xESK_CONTROL_VERSION      2U
#define xESK_CONTROL_HEADER_BYTES 48U
// Every opcode except SERVICE; USB EP0 and the plane mailboxes use this bound.
#define xESK_CONTROL_MAX_BYTES (xESK_CONTROL_HEADER_BYTES + xESK_MAX_FRAME_BYTES)
// SERVICE frames carry one service frame; only the network route uses this bound.
#define xESK_CONTROL_SERVICE_MAX_BYTES (xESK_CONTROL_HEADER_BYTES + xESK_SERVICE_FRAME_BYTES)

    // TYPES //////////////////////////////////////////////////////////////////////////
    typedef enum
    {
        xESK_CONTROL_REQUEST = 1,
        xESK_CONTROL_RESPONSE = 2,
    } xESK_Control_Kind_t;

    typedef enum
    {
        xESK_CONTROL_DESCRIBE = 1,
        xESK_CONTROL_CLAIM = 2,
        xESK_CONTROL_RENEW = 3,
        xESK_CONTROL_RELEASE = 4,
        xESK_CONTROL_SESSION = 5,
        xESK_CONTROL_REAP = 6,
        xESK_CONTROL_SERVICE = 7,
        xESK_CONTROL_FEATURE_STATUS = 8,
    } xESK_Control_Opcode_t;

    typedef enum
    {
        xESK_CONTROL_OK = 0,
        xESK_CONTROL_INVALID = 1,
        xESK_CONTROL_BUSY = 2,
        xESK_CONTROL_UNSUPPORTED = 3,
        xESK_CONTROL_STALE = 4,
        xESK_CONTROL_OWNERSHIP = 5,
        xESK_CONTROL_CAPACITY = 6,
        xESK_CONTROL_SEARCH_LIMIT = 7,
        xESK_CONTROL_STATE = 8,
        xESK_CONTROL_INTERNAL = 9,
        xESK_CONTROL_NOT_FOUND = 10,
    } xESK_Control_Status_t;

    // Value-only wire identity, suitable for retention while payload storage is reused.
    typedef struct
    {
        uint8_t kind;
        uint8_t opcode;
        uint8_t status;
        uint64_t request_id;
        uint64_t boot_id;
        uint64_t session_id;
        uint16_t service_id;
        uint16_t instance_id;
    } xESK_Control_Header_t;

    // Borrowed SERVICE payload; the provider owns storage through the consumer's
    // terminal operation. Copy the header to retain identity independently of data.
    typedef struct
    {
        xESK_Control_Header_t header;
        const uint8_t *data;
        size_t length;
    } xESK_Control_Service_View_t;

    typedef struct
    {
        uint8_t kind;
        uint8_t opcode;
        uint8_t status;
        uint64_t request_id;
        uint64_t boot_id;
        uint64_t session_id;
        uint16_t service_id;
        uint16_t instance_id;
        uint8_t descriptor_kind;
        uint32_t index;
        uint8_t expiry_policy;
        uint64_t lease_ticks;
        bool has_record;
        union
        {
            xESK_Record_t record;
            struct
            {
                size_t length;
                uint8_t data[xESK_SERVICE_FRAME_BYTES]; // SERVICE; others use at most xESK_MAX_FRAME_BYTES
            } payload;
        };
    } xESK_Control_Message_t;

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    // Call-scoped value objects. Zero unused fields; record.data is inactive when
    // has_record is false (record.kind must then be zero). No buffer is retained.
    // All failure paths preserve outputs. Typed objects must be valid/aligned;
    // every supplied range must be nonwrapping and disjoint from the other ranges.
    xRETURN_t xESK_Control_Validate(const xESK_Control_Message_t *message);
    xRETURN_t xESK_Control_Encode(const xESK_Control_Message_t *message, uint8_t *buffer, size_t capacity, size_t *written);
    xRETURN_t xESK_Control_Decode(const uint8_t *buffer, size_t length, xESK_Control_Message_t *message);
    xRETURN_t xESK_Control_Reply_Validate(const xESK_Control_Message_t *request, const xESK_Control_Message_t *response);

    // Decode validates the complete SERVICE envelope without copying payload bytes.
    // The input must remain valid while the returned view is used. Failure preserves
    // output; the typed view and input ranges must be valid and disjoint.
    xRETURN_t xESK_Control_Service_Decode(const uint8_t *buffer, size_t length, xESK_Control_Service_View_t *view);
    // Validate correlation using independent identity snapshots, without reading data.
    xRETURN_t xESK_Control_Service_Reply_Validate(const xESK_Control_Header_t *request, const xESK_Control_Service_View_t *response);
    // Payload is already at buffer + HEADER_BYTES (or empty). Only the header is
    // written; no payload copy or move occurs. Validate all ranges before mutation.
    // View and written must be disjoint from each other and the complete output.
    xRETURN_t
    xESK_Control_Service_Encode_In_Place(const xESK_Control_Service_View_t *view, uint8_t *buffer, size_t capacity, size_t *written);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_CONTROL_H

// EOF /////////////////////////////////////////////////////////////////////////////
