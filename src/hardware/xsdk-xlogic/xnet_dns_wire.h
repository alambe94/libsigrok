// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xnet_dns_wire.h
// @brief Bounded context-free DNS message reader.

#ifndef xNET_DNS_WIRE_H
#define xNET_DNS_WIRE_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SYSTEM INCLUDES

// MODULE INCLUDES
#include "xnet_return.h"

// DIAGNOSTIC INCLUDES

#ifdef __cplusplus
extern "C"
{
#endif

    // PUBLIC API /////////////////////////////////////////////////////////

    // MACROS /////////////////////////////////////////////////////////////////////////

    // TYPES //////////////////////////////////////////////////////////////////////////
    typedef struct
    {
        uint8_t wire[256];
        size_t length;
    } xNET_DNS_Wire_Name_t;

    typedef struct
    {
        const uint8_t *data;
        size_t length;
        size_t offset;
        size_t index;
        size_t total;
        uint16_t question_count;
        uint16_t answer_count;
        uint16_t authority_count;
        uint16_t additional_count;
        uint16_t id;
        uint16_t flags;
    } xNET_DNS_Wire_Cursor_t;

    typedef struct
    {
        xNET_DNS_Wire_Name_t name;
        uint16_t type;
        uint16_t class_code;
        uint32_t ttl;
        size_t rdata_offset;
        size_t rdata_length;
        // 0 question, 1 answer, 2 authority, 3 additional.
        uint8_t section;
    } xNET_DNS_Wire_Item_t;

    // VARIABLES //////////////////////////////////////////////////////////////////////

    // INLINE FUNCTIONS ///////////////////////////////////////////////////////////////

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    // Complete validation before publishing cursor. Input must remain immutable while used.
    // Maximum message length 65535. Unknown RR payloads remain opaque. Forward compression
    // pointers are rejected; each followed pointer must precede its name segment.
    xRETURN_t xNET_DNS_Wire_Open(xNET_DNS_Wire_Cursor_t *cursor, const uint8_t *data, size_t length);
    // NOT_FOUND at end. Failure leaves cursor unchanged; output is unspecified on failure.
    xRETURN_t xNET_DNS_Wire_Next(xNET_DNS_Wire_Cursor_t *cursor, xNET_DNS_Wire_Item_t *item);
    xRETURN_t xNET_DNS_Wire_Name_Read(const uint8_t *data, size_t length, size_t *offset, xNET_DNS_Wire_Name_t *name);
    bool xNET_DNS_Wire_Name_Equal(const xNET_DNS_Wire_Name_t *left, const xNET_DNS_Wire_Name_t *right);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xNET_DNS_WIRE_H

// EOF /////////////////////////////////////////////////////////////////////////////
