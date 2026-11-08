// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_control_internal.h
// @brief Private ESK control payload helpers.

#ifndef xESK_CONTROL_INTERNAL_H
#define xESK_CONTROL_INTERNAL_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stddef.h>
// MODULE INCLUDES
#include "xesk_control.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // MODULE DECLARATIONS ////////////////////////////////////////////////////////////
    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    bool xESK_Control_Range_Valid(const void *pointer, size_t length, size_t alignment);
    bool xESK_Control_Ranges_Disjoint(const void *left, size_t left_length, const void *right, size_t right_length);
    xRETURN_t xESK_Control_Payload_Validate(const xESK_Control_Message_t *message);
    size_t xESK_Control_Payload_Size(const xESK_Control_Message_t *message);
    xRETURN_t xESK_Control_Payload_Encode(const xESK_Control_Message_t *message, uint8_t *payload);
    xRETURN_t xESK_Control_Payload_Decode(const uint8_t *payload, size_t length, xESK_Control_Message_t *message);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_CONTROL_INTERNAL_H

// EOF /////////////////////////////////////////////////////////////////////////////
