// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_feature_status.h
// @brief Read-only feature observation values and bounded paging codec.

#ifndef xESK_FEATURE_STATUS_H
#define xESK_FEATURE_STATUS_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stddef.h>
#include <stdint.h>
// MODULE INCLUDES
#include "xesk_resource_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

// PUBLIC API /////////////////////////////////////////////////////////

// MACROS /////////////////////////////////////////////////////////////////////////
#define xESK_FEATURE_QUERY_BYTES       12U
#define xESK_FEATURE_PAGE_HEADER_BYTES 32U
#define xESK_FEATURE_PAGE_ROWS         12U
#define xESK_FEATURE_MAX_ROWS          32U

    // TYPES //////////////////////////////////////////////////////////////////////////
    typedef struct
    {
        uint64_t snapshot_id;
        uint16_t start_index;
    } xESK_Feature_Query_t;
    typedef struct
    {
        uint64_t snapshot_id;
        uint64_t observed_at_ticks;
        uint32_t timebase_id;
        uint16_t total_rows;
        uint16_t start_index;
        uint8_t row_count;
        uint8_t flags;
        xESK_Feature_Observation_t rows[xESK_FEATURE_PAGE_ROWS];
    } xESK_Feature_Page_t;
    // Immutable endpoint binding. read runs only on the serialized owner after
    // reply credit is reserved. It may fill its explicit observation cache, but
    // may not mutate owners, expire leases, search allocations or call hardware.
    typedef struct
    {
        xRETURN_t (*read)(void *user,
                          const xESK_Binding_t *binding,
                          const xESK_Feature_Query_t *query,
                          uint64_t now_ticks,
                          xESK_Feature_Page_t *page);
        void *user;
    } xESK_Feature_Reader_t;

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    // Value codecs retain no pointers. Failed calls preserve all outputs. Typed
    // objects and byte ranges must be aligned/nonwrapping and mutually disjoint.
    // Wire integers are explicit little-endian; no C layout is transmitted.
    xRETURN_t xESK_Feature_Observation_Validate(const xESK_Feature_Observation_t *row);
    xRETURN_t xESK_Feature_Query_Encode(const xESK_Feature_Query_t *query, uint8_t *data, size_t capacity, size_t *written);
    xRETURN_t xESK_Feature_Query_Decode(const uint8_t *data, size_t length, xESK_Feature_Query_t *query);
    xRETURN_t xESK_Feature_Page_Encode(const xESK_Feature_Page_t *page, uint8_t *data, size_t capacity, size_t *written);
    xRETURN_t xESK_Feature_Page_Decode(const uint8_t *data, size_t length, xESK_Feature_Page_t *page);
    xRETURN_t xESK_Feature_Reply_Validate(const xESK_Feature_Query_t *query, const xESK_Feature_Page_t *page);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_FEATURE_STATUS_H

// EOF /////////////////////////////////////////////////////////////////////////////
