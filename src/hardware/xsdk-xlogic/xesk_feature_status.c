// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_feature_status.c
// @brief Read-only feature observation values and bounded paging codec.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <string.h>

// MODULE INCLUDES
#include "xesk_feature_status.h"
#include "xesk_control_internal.h"
#include "xbytes.h"

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static bool query_valid(const xESK_Feature_Query_t *query)
{
    return query->start_index <= xESK_FEATURE_MAX_ROWS && (query->snapshot_id != 0U || query->start_index == 0U);
}

static bool row_identity_valid(const xESK_Feature_Observation_t *row)
{
    return row->feature_id >= xESK_SERVICE_DEVICE && row->feature_id <= xESK_FEATURE_QSPI &&
           (row->feature_id != xESK_SERVICE_DEBUG || row->instance_id <= 1U) &&
           (row->feature_id != xESK_FEATURE_QSPI || row->instance_id == 0U);
}

static bool row_capability_valid(const xESK_Feature_Observation_t *row)
{
    if (row->capability_flags > 7U || row->ownership > xESK_FEATURE_FAULTED || row->restrictions > 63U)
    {
        return false;
    }
    if ((row->capability_flags & (xESK_FEATURE_ENABLED | xESK_FEATURE_OBSERVABLE)) != 0U &&
        (row->capability_flags & xESK_FEATURE_IMPLEMENTED) == 0U)
    {
        return false;
    }
    return (row->capability_flags & xESK_FEATURE_OBSERVABLE) != 0U || row->ownership == xESK_FEATURE_UNKNOWN;
}

static bool row_valid(const xESK_Feature_Observation_t *row)
{
    if (!row_identity_valid(row) || !row_capability_valid(row))
    {
        return false;
    }
    if (row->ownership == xESK_FEATURE_UNKNOWN)
    {
        return row->transport == xESK_FEATURE_TRANSPORT_UNKNOWN;
    }
    if (row->ownership == xESK_FEATURE_UNCLAIMED)
    {
        return row->transport == xESK_FEATURE_TRANSPORT_NONE;
    }
    return row->transport == xESK_FEATURE_TRANSPORT_USB || row->transport == xESK_FEATURE_TRANSPORT_NETWORK ||
           row->transport == xESK_FEATURE_TRANSPORT_INTERNAL || row->transport == xESK_FEATURE_TRANSPORT_UNKNOWN;
}

static bool page_valid(const xESK_Feature_Page_t *page)
{
    if (page->snapshot_id == 0U || page->timebase_id == 0U || page->total_rows == 0U || page->total_rows > xESK_FEATURE_MAX_ROWS ||
        page->start_index >= page->total_rows || page->flags > 1U)
    {
        return false;
    }
    const size_t remaining = (size_t)page->total_rows - page->start_index;
    const size_t expected = remaining < xESK_FEATURE_PAGE_ROWS ? remaining : xESK_FEATURE_PAGE_ROWS;
    if (page->row_count != expected)
    {
        return false;
    }
    for (size_t index = 0U; index < page->row_count; ++index)
    {
        if (!row_valid(&page->rows[index]))
        {
            return false;
        }
        if (index != 0U)
        {
            const xESK_Feature_Observation_t *prior = &page->rows[index - 1U];
            const xESK_Feature_Observation_t *row = &page->rows[index];
            if (row->feature_id < prior->feature_id || (row->feature_id == prior->feature_id && row->instance_id <= prior->instance_id))
            {
                return false;
            }
        }
    }
    return true;
}

static xRETURN_t encode_ranges(const void *model, size_t size, size_t alignment, uint8_t *data, size_t capacity, size_t *written)
{
    if (model == NULL || data == NULL || written == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!xESK_Control_Range_Valid(model, size, alignment) || !xESK_Control_Range_Valid(data, capacity, 1U) ||
        !xESK_Control_Range_Valid(written, sizeof(*written), _Alignof(size_t)) ||
        !xESK_Control_Ranges_Disjoint(model, size, data, capacity) ||
        !xESK_Control_Ranges_Disjoint(model, size, written, sizeof(*written)) ||
        !xESK_Control_Ranges_Disjoint(data, capacity, written, sizeof(*written)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return xRETURN_OK;
}

static xRETURN_t decode_ranges(const uint8_t *data, size_t length, void *model, size_t size, size_t alignment)
{
    if (data == NULL || model == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    return xESK_Control_Range_Valid(data, length, 1U) && xESK_Control_Range_Valid(model, size, alignment) &&
                   xESK_Control_Ranges_Disjoint(data, length, model, size)
               ? xRETURN_OK
               : xRETURN_xERR_xESK_INVALID_ARG;
}

static void row_encode(const xESK_Feature_Observation_t *row, uint8_t *data)
{
    xWrite_LE16(data, row->feature_id);
    data[2] = row->instance_id;
    data[3] = row->capability_flags;
    data[4] = row->ownership;
    data[5] = row->transport;
    xWrite_LE16(data + 6U, row->restrictions);
}

static void row_decode(const uint8_t *data, xESK_Feature_Observation_t *row)
{
    *row = (xESK_Feature_Observation_t){xRead_LE16(data), data[2], data[3], data[4], data[5], xRead_LE16(data + 6U)};
}

// PUBLIC API IMPLEMENTATION /////////////////////////////////////////////////////
xRETURN_t xESK_Feature_Observation_Validate(const xESK_Feature_Observation_t *row)
{
    if (row == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!xESK_Control_Range_Valid(row, sizeof(*row), _Alignof(xESK_Feature_Observation_t)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return row_valid(row) ? xRETURN_OK : xRETURN_xERR_xESK_INVALID_ARG;
}

xRETURN_t xESK_Feature_Query_Encode(const xESK_Feature_Query_t *query, uint8_t *data, size_t capacity, size_t *written)
{
    xRETURN_t status = encode_ranges(query, sizeof(*query), _Alignof(xESK_Feature_Query_t), data, capacity, written);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!query_valid(query))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (capacity < xESK_FEATURE_QUERY_BYTES)
    {
        return xRETURN_xERR_xESK_OVERFLOW;
    }
    (void)memset(data, 0, xESK_FEATURE_QUERY_BYTES);
    xWrite_LE64(data, query->snapshot_id);
    xWrite_LE16(data + 8U, query->start_index);
    *written = xESK_FEATURE_QUERY_BYTES;
    return xRETURN_OK;
}

xRETURN_t xESK_Feature_Query_Decode(const uint8_t *data, size_t length, xESK_Feature_Query_t *query)
{
    xRETURN_t status = decode_ranges(data, length, query, sizeof(*query), _Alignof(xESK_Feature_Query_t));
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (length != xESK_FEATURE_QUERY_BYTES || xRead_LE16(data + 10U) != 0U)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    const xESK_Feature_Query_t candidate = {xRead_LE64(data), xRead_LE16(data + 8U)};
    if (!query_valid(&candidate))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *query = candidate;
    return xRETURN_OK;
}

xRETURN_t xESK_Feature_Page_Encode(const xESK_Feature_Page_t *page, uint8_t *data, size_t capacity, size_t *written)
{
    xRETURN_t status = encode_ranges(page, sizeof(*page), _Alignof(xESK_Feature_Page_t), data, capacity, written);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!page_valid(page))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    const size_t length = xESK_FEATURE_PAGE_HEADER_BYTES + 8U * page->row_count;
    if (capacity < length)
    {
        return xRETURN_xERR_xESK_OVERFLOW;
    }
    (void)memset(data, 0, length);
    data[0] = 1U;
    data[1] = 8U;
    data[2] = page->row_count;
    data[3] = page->flags;
    xWrite_LE16(data + 4U, page->total_rows);
    xWrite_LE16(data + 6U, page->start_index);
    xWrite_LE64(data + 8U, page->snapshot_id);
    xWrite_LE64(data + 16U, page->observed_at_ticks);
    xWrite_LE32(data + 24U, page->timebase_id);
    for (size_t index = 0U; index < page->row_count; ++index)
    {
        row_encode(&page->rows[index], data + xESK_FEATURE_PAGE_HEADER_BYTES + 8U * index);
    }
    *written = length;
    return xRETURN_OK;
}

xRETURN_t xESK_Feature_Page_Decode(const uint8_t *data, size_t length, xESK_Feature_Page_t *page)
{
    xRETURN_t status = decode_ranges(data, length, page, sizeof(*page), _Alignof(xESK_Feature_Page_t));
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (length < xESK_FEATURE_PAGE_HEADER_BYTES || length > xESK_MAX_FRAME_BYTES || data[0] != 1U || data[1] != 8U ||
        data[2] > xESK_FEATURE_PAGE_ROWS || length != xESK_FEATURE_PAGE_HEADER_BYTES + 8U * data[2] || xRead_LE32(data + 28U) != 0U)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    xESK_Feature_Page_t candidate = {.snapshot_id = xRead_LE64(data + 8U),
                                     .observed_at_ticks = xRead_LE64(data + 16U),
                                     .timebase_id = xRead_LE32(data + 24U),
                                     .total_rows = xRead_LE16(data + 4U),
                                     .start_index = xRead_LE16(data + 6U),
                                     .row_count = data[2],
                                     .flags = data[3]};
    for (size_t index = 0U; index < candidate.row_count; ++index)
    {
        row_decode(data + xESK_FEATURE_PAGE_HEADER_BYTES + 8U * index, &candidate.rows[index]);
    }
    if (!page_valid(&candidate))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *page = candidate;
    return xRETURN_OK;
}

xRETURN_t xESK_Feature_Reply_Validate(const xESK_Feature_Query_t *query, const xESK_Feature_Page_t *page)
{
    if (query == NULL || page == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!xESK_Control_Range_Valid(query, sizeof(*query), _Alignof(xESK_Feature_Query_t)) ||
        !xESK_Control_Range_Valid(page, sizeof(*page), _Alignof(xESK_Feature_Page_t)) ||
        !xESK_Control_Ranges_Disjoint(query, sizeof(*query), page, sizeof(*page)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return query_valid(query) && page_valid(page) && query->start_index == page->start_index &&
                   (query->snapshot_id == 0U || query->snapshot_id == page->snapshot_id)
               ? xRETURN_OK
               : xRETURN_xERR_xESK_CORRUPT;
}

// EOF /////////////////////////////////////////////////////////////////////////////
