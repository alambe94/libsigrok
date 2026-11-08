// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_internal.h
// @brief Private ESK wire conversion helpers.

#ifndef xESK_INTERNAL_H
#define xESK_INTERNAL_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stddef.h>
#include <stdbool.h>
// MODULE INCLUDES
#include "xesk.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // MODULE DECLARATIONS /////////////////////////////////////////////////////////
    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    size_t xESK_Payload_Size(uint8_t kind);
    bool xESK_Payload_Reserved_Valid(uint8_t kind, const uint8_t *payload);
    void xESK_Payload_Encode(const xESK_Record_t *record, uint8_t *payload);
    void xESK_Payload_Decode(const uint8_t *payload, xESK_Record_t *record);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_INTERNAL_H

// EOF /////////////////////////////////////////////////////////////////////////////
