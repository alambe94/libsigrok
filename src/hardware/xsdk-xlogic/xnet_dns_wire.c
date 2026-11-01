// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xnet_dns_wire.c
// @brief Bounded context-free DNS message reader.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <string.h>

// SYSTEM INCLUDES

// MODULE INCLUDES
#include "xnet_dns_wire.h"

// DIAGNOSTIC INCLUDES

// MACROS /////////////////////////////////////////////////////////////////////////

// TYPES //////////////////////////////////////////////////////////////////////////

// VARIABLES //////////////////////////////////////////////////////////////////////

// EXTERN VARIABLES ///////////////////////////////////////////////////////////////

// FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint8_t folded(uint8_t value)
{
    return ((value >= (uint8_t)'A') && (value <= (uint8_t)'Z')) ? (uint8_t)(value + 32U) : value;
}

static xRETURN_t read_name(const uint8_t *data, size_t length, size_t *offset, xNET_DNS_Wire_Name_t *name)
{
    size_t position = *offset;
    size_t limit = position;
    size_t resume = 0U;
    size_t used = 0U;
    size_t steps = 0U;
    while ((position < length) && (steps < 256U))
    {
        steps++;
        uint8_t count = data[position++];
        if ((count & 0xC0U) == 0xC0U)
        {
            if (position >= length)
            {
                return xRETURN_xERR_xNET_INVALID_PACKET;
            }
            size_t target = ((size_t)(count & 0x3FU) << 8U) | data[position++];
            if (target >= limit)
            {
                return xRETURN_xERR_xNET_INVALID_PACKET;
            }
            if (resume == 0U)
            {
                resume = position;
            }
            limit = target;
            position = target;
            continue;
        }
        if ((count > 63U) || ((size_t)count > (length - position)) || ((used + (size_t)count + 1U) > 255U))
        {
            return xRETURN_xERR_xNET_INVALID_PACKET;
        }
        name->wire[used++] = count;
        if (count == 0U)
        {
            name->length = used;
            *offset = (resume == 0U) ? position : resume;
            return xRETURN_xNET_OK;
        }
        (void)memcpy(&name->wire[used], &data[position], count);
        position += count;
        used += count;
    }
    return xRETURN_xERR_xNET_INVALID_PACKET;
}

static uint8_t item_section(const xNET_DNS_Wire_Cursor_t *cursor)
{
    size_t index = cursor->index;
    if (index < cursor->question_count)
    {
        return 0U;
    }
    index -= cursor->question_count;
    if (index < cursor->answer_count)
    {
        return 1U;
    }
    index -= cursor->answer_count;
    return (index < cursor->authority_count) ? 2U : 3U;
}

static xRETURN_t rdata_check(const xNET_DNS_Wire_Cursor_t *cursor, const xNET_DNS_Wire_Item_t *item)
{
    size_t position = item->rdata_offset;
    size_t end = position + item->rdata_length;
    xNET_DNS_Wire_Name_t name;
    if ((item->type == 12U) || (item->type == 5U) || (item->type == 33U))
    {
        if (item->type == 33U)
        {
            if (item->rdata_length < 7U)
            {
                return xRETURN_xERR_xNET_INVALID_PACKET;
            }
            position += 6U;
        }
        xRETURN_t status = read_name(cursor->data, cursor->length, &position, &name);
        if (status != xRETURN_xNET_OK)
        {
            return status;
        }
        return (position == end) ? xRETURN_xNET_OK : xRETURN_xERR_xNET_INVALID_PACKET;
    }
    if (item->type == 16U)
    {
        while (position < end)
        {
            size_t count = cursor->data[position++];
            if (count > (end - position))
            {
                return xRETURN_xERR_xNET_INVALID_PACKET;
            }
            position += count;
        }
    }
    return xRETURN_xNET_OK;
}

static xRETURN_t next_item(xNET_DNS_Wire_Cursor_t *cursor, xNET_DNS_Wire_Item_t *item)
{
    size_t position = cursor->offset;
    xRETURN_t status = read_name(cursor->data, cursor->length, &position, &item->name);
    if (status != xRETURN_xNET_OK)
    {
        return status;
    }
    item->section = item_section(cursor);
    size_t fields = (item->section == 0U) ? 4U : 10U;
    if ((cursor->length - position) < fields)
    {
        return xRETURN_xERR_xNET_INVALID_PACKET;
    }
    item->type = read_u16(&cursor->data[position]);
    item->class_code = read_u16(&cursor->data[position + 2U]);
    item->ttl = 0U;
    item->rdata_length = 0U;
    if (item->section != 0U)
    {
        item->ttl = ((uint32_t)read_u16(&cursor->data[position + 4U]) << 16U) | read_u16(&cursor->data[position + 6U]);
        item->rdata_length = read_u16(&cursor->data[position + 8U]);
    }
    item->rdata_offset = position + fields;
    if (item->rdata_length > (cursor->length - item->rdata_offset))
    {
        return xRETURN_xERR_xNET_INVALID_PACKET;
    }
    if (item->section != 0U)
    {
        status = rdata_check(cursor, item);
        if (status != xRETURN_xNET_OK)
        {
            return status;
        }
    }
    cursor->offset = item->rdata_offset + item->rdata_length;
    cursor->index++;
    return xRETURN_xNET_OK;
}

// PUBLIC API IMPLEMENTATION /////////////////////////////////////////////////////
xRETURN_t xNET_DNS_Wire_Name_Read(const uint8_t *data, size_t length, size_t *offset, xNET_DNS_Wire_Name_t *name)
{
    if ((data == NULL) || (offset == NULL) || (name == NULL))
    {
        return xRETURN_xERR_xNET_NULL_POINTER;
    }
    if ((length > UINT16_MAX) || (*offset >= length))
    {
        return xRETURN_xERR_xNET_INVALID_LENGTH;
    }
    return read_name(data, length, offset, name);
}

bool xNET_DNS_Wire_Name_Equal(const xNET_DNS_Wire_Name_t *left, const xNET_DNS_Wire_Name_t *right)
{
    if ((left == NULL) || (right == NULL) || (left->length != right->length) || (left->length > 255U))
    {
        return false;
    }
    for (size_t index = 0U; index < left->length; index++)
    {
        if (folded(left->wire[index]) != folded(right->wire[index]))
        {
            return false;
        }
    }
    return true;
}

xRETURN_t xNET_DNS_Wire_Next(xNET_DNS_Wire_Cursor_t *cursor, xNET_DNS_Wire_Item_t *item)
{
    if ((cursor == NULL) || (item == NULL))
    {
        return xRETURN_xERR_xNET_NULL_POINTER;
    }
    if ((cursor->data == NULL) || (cursor->offset > cursor->length))
    {
        return xRETURN_xERR_xNET_INVALID_STATE;
    }
    if (cursor->index >= cursor->total)
    {
        return xRETURN_xERR_xNET_NOT_FOUND;
    }
    return next_item(cursor, item);
}

xRETURN_t xNET_DNS_Wire_Open(xNET_DNS_Wire_Cursor_t *cursor, const uint8_t *data, size_t length)
{
    if ((cursor == NULL) || (data == NULL))
    {
        return xRETURN_xERR_xNET_NULL_POINTER;
    }
    if ((length < 12U) || (length > UINT16_MAX))
    {
        return xRETURN_xERR_xNET_INVALID_LENGTH;
    }
    xNET_DNS_Wire_Cursor_t parsed = {0};
    parsed.data = data;
    parsed.length = length;
    parsed.offset = 12U;
    parsed.id = read_u16(data);
    parsed.flags = read_u16(&data[2]);
    parsed.question_count = read_u16(&data[4]);
    parsed.answer_count = read_u16(&data[6]);
    parsed.authority_count = read_u16(&data[8]);
    parsed.additional_count = read_u16(&data[10]);
    parsed.total = (size_t)parsed.question_count + parsed.answer_count + parsed.authority_count + parsed.additional_count;
    xNET_DNS_Wire_Item_t item;
    while (parsed.index < parsed.total)
    {
        xRETURN_t status = next_item(&parsed, &item);
        if (status != xRETURN_xNET_OK)
        {
            return status;
        }
    }
    if (parsed.offset != length)
    {
        return xRETURN_xERR_xNET_INVALID_PACKET;
    }
    parsed.offset = 12U;
    parsed.index = 0U;
    *cursor = parsed;
    return xRETURN_xNET_OK;
}

// EOF /////////////////////////////////////////////////////////////////////////////
