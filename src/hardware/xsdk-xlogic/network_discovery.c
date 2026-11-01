/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#include <string.h>
#include <stdlib.h>
#include "network_discovery.h"

static uint16_t read16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8U) | bytes[1];
}

static bool name_make(const char *text, xNET_DNS_Wire_Name_t *name)
{
    const char *label = text;
    size_t length, used = 0;
    while (*label)
    {
        const char *end = strchr(label, '.');
        length = end ? (size_t)(end - label) : strlen(label);
        if (!length || length > 63U || used + length + 2U > sizeof(name->wire))
        {
            return false;
        }
        name->wire[used++] = (uint8_t)length;
        memcpy(name->wire + used, label, length);
        used += length;
        label += length;
        if (*label)
        {
            label++;
        }
    }
    name->wire[used++] = 0;
    name->length = used;
    return used > 1U;
}

size_t xsdk_xlogic_discovery_query(uint8_t *packet, size_t capacity, uint16_t id, const char *name, uint16_t type)
{
    xNET_DNS_Wire_Name_t encoded;
    size_t offset;
    bool valid;
    if (!packet || !name || !id)
    {
        return 0;
    }
    valid = name_make(name, &encoded);
    if (!valid || capacity < encoded.length + 16U)
    {
        return 0;
    }
    memset(packet, 0, 12);
    packet[0] = (uint8_t)(id >> 8U);
    packet[1] = (uint8_t)id;
    packet[5] = 1;
    memcpy(packet + 12, encoded.wire, encoded.length);
    offset = 12U + encoded.length;
    packet[offset++] = (uint8_t)(type >> 8U);
    packet[offset++] = (uint8_t)type;
    packet[offset++] = 0;
    packet[offset++] = 1;
    return offset;
}

static bool record_equal(const struct xsdk_xlogic_discovery_record *left, const struct xsdk_xlogic_discovery_record *right)
{
    return left->type == right->type && left->interface_index == right->interface_index &&
           xNET_DNS_Wire_Name_Equal(&left->name, &right->name) && xNET_DNS_Wire_Name_Equal(&left->target, &right->target) &&
           left->port == right->port && left->address == right->address && left->text_length == right->text_length &&
           memcmp(left->text, right->text, left->text_length) == 0;
}

static bool record_read(const xNET_DNS_Wire_Cursor_t *cursor, const xNET_DNS_Wire_Item_t *item, struct xsdk_xlogic_discovery_record *record)
{
    size_t offset = item->rdata_offset;
    const uint8_t *bytes = cursor->data + offset;
    xRETURN_t result;
    memset(record, 0, sizeof(*record));
    record->name = item->name;
    record->type = item->type;
    switch (item->type)
    {
    case 1:
        if (item->rdata_length != 4U)
        {
            return false;
        }
        memcpy(&record->address, bytes, 4);
        return true;
    case 33:
        if (read16(bytes) != 0 || read16(bytes + 2) != 0 || read16(bytes + 4) == 0)
        {
            return false;
        }
        record->port = read16(bytes + 4);
        offset += 6;
        break;
    case 12:
        break;
    case 16:
        if (item->rdata_length > sizeof(record->text))
        {
            return false;
        }
        memcpy(record->text, bytes, item->rdata_length);
        record->text_length = item->rdata_length;
        return true;
    default:
        return false;
    }
    result = xNET_DNS_Wire_Name_Read(cursor->data, cursor->length, &offset, &record->target);
    return result == xRETURN_xNET_OK;
}

static bool cache_record(struct xsdk_xlogic_discovery_cache *cache,
                         struct xsdk_xlogic_discovery_record *incoming,
                         const xNET_DNS_Wire_Item_t *item,
                         uint64_t now)
{
    size_t index;
    struct xsdk_xlogic_discovery_record *slot = NULL;
    for (index = 0; index < xSDK_xLOGIC_DISCOVERY_RECORDS; index++)
    {
        struct xsdk_xlogic_discovery_record *existing = &cache->records[index];
        if (record_equal(existing, incoming))
        {
            slot = existing;
        }
        else if ((item->class_code & 0x8000U) && existing->received != now && existing->type == incoming->type &&
                 existing->interface_index == incoming->interface_index && xNET_DNS_Wire_Name_Equal(&existing->name, &incoming->name))
        {
            existing->expires = 0;
        }
        if (!slot && existing->expires <= now)
        {
            slot = existing;
        }
    }
    if (!slot)
    {
        memset(cache, 0, sizeof(*cache));
        return false;
    }
    incoming->expires = item->ttl ? now + (uint64_t)item->ttl * 1000000U : 0;
    incoming->received = now;
    *slot = *incoming;
    return true;
}

bool xsdk_xlogic_discovery_receive(struct xsdk_xlogic_discovery_cache *cache,
                                   const uint8_t *packet,
                                   size_t length,
                                   uint16_t id,
                                   uint16_t interface_index,
                                   uint64_t now)
{
    xNET_DNS_Wire_Cursor_t cursor;
    xNET_DNS_Wire_Item_t item;
    struct xsdk_xlogic_discovery_record record;
    xRETURN_t result;
    bool parsed;
    if (!cache || !packet || !id)
    {
        return false;
    }
    result = xNET_DNS_Wire_Open(&cursor, packet, length);
    if (result != xRETURN_xNET_OK || cursor.id != id || (cursor.flags & 0xfe0fU) != 0x8400U)
    {
        return false;
    }
    for (;;)
    {
        result = xNET_DNS_Wire_Next(&cursor, &item);
        if (result != xRETURN_xNET_OK)
        {
            return result == xRETURN_xERR_xNET_NOT_FOUND;
        }
        if (!item.section || (item.class_code & 0x7fffU) != 1U)
        {
            continue;
        }
        parsed = record_read(&cursor, &item, &record);
        if (!parsed)
        {
            continue;
        }
        record.interface_index = interface_index;
        parsed = cache_record(cache, &record, &item, now);
        if (!parsed)
        {
            return false;
        }
    }
}

static const struct xsdk_xlogic_discovery_record *record_find(const struct xsdk_xlogic_discovery_cache *cache,
                                                              const xNET_DNS_Wire_Name_t *name,
                                                              uint16_t type,
                                                              uint16_t interface_index,
                                                              uint64_t now)
{
    const struct xsdk_xlogic_discovery_record *found = NULL;
    size_t index;
    for (index = 0; index < xSDK_xLOGIC_DISCOVERY_RECORDS; index++)
    {
        const struct xsdk_xlogic_discovery_record *record = &cache->records[index];
        if (record->expires > now && record->type == type && record->interface_index == interface_index &&
            xNET_DNS_Wire_Name_Equal(&record->name, name))
        {
            if (found)
            {
                return NULL;
            }
            found = record;
        }
    }
    return found;
}

static bool text_get(const struct xsdk_xlogic_discovery_record *record, const char *key, char *value, size_t capacity)
{
    size_t offset = 0, key_length = strlen(key), found = 0;
    while (offset < record->text_length)
    {
        size_t length = record->text[offset++];
        const uint8_t *text = record->text + offset;
        size_t index;
        offset += length;
        if (length <= key_length || text[key_length] != '=' || memcmp(text, key, key_length))
        {
            continue;
        }
        if (++found > 1U || length - key_length >= capacity)
        {
            return false;
        }
        for (index = key_length + 1; index < length; index++)
        {
            if (text[index] < 32U || text[index] > 126U)
            {
                return false;
            }
        }
        memcpy(value, text + key_length + 1, length - key_length - 1);
        value[length - key_length - 1] = 0;
    }
    return found == 1U;
}

static bool identity_read(const struct xsdk_xlogic_discovery_record *text, struct xsdk_xlogic_discovery_device *device)
{
    char protocol[32], version[16];
    size_t index;
    bool valid = text_get(text, "id", device->identity, sizeof(device->identity));
    if (!valid || strlen(device->identity) != 12U)
    {
        return false;
    }
    for (index = 0; index < 12U; index++)
    {
        if (!strchr("0123456789abcdef", device->identity[index]))
        {
            return false;
        }
    }
    valid = text_get(text, "board", device->board, sizeof(device->board));
    if (!valid || !device->board[0])
    {
        return false;
    }
    valid = text_get(text, "proto", protocol, sizeof(protocol));
    if (!valid || strcmp(protocol, "xlogic-net"))
    {
        return false;
    }
    valid = text_get(text, "wire", version, sizeof(version));
    return valid && strcmp(version, "1") == 0;
}

static bool name_text(const xNET_DNS_Wire_Name_t *name, char *text, bool instance)
{
    size_t offset = 0, used = 0;
    while (offset < name->length && name->wire[offset])
    {
        size_t length = name->wire[offset++];
        size_t index;
        if (used)
        {
            text[used++] = '.';
        }
        for (index = 0; index < length; index++)
        {
            uint8_t byte = name->wire[offset++];
            if (byte < 32U || byte > 126U || used + 1U >= xSDK_xLOGIC_DISCOVERY_NAME)
            {
                return false;
            }
            text[used++] = (char)byte;
        }
        if (instance)
        {
            break;
        }
    }
    text[used] = 0;
    return used != 0;
}

static bool device_read(const struct xsdk_xlogic_discovery_cache *cache,
                        const struct xsdk_xlogic_discovery_record *pointer,
                        uint64_t now,
                        struct xsdk_xlogic_discovery_device *device)
{
    const struct xsdk_xlogic_discovery_record *service, *text, *address;
    xNET_DNS_Wire_Name_t suffix, expected;
    bool valid;
    size_t prefix = (size_t)pointer->target.wire[0] + 1U;
    if (prefix >= pointer->target.length)
    {
        return false;
    }
    suffix.length = pointer->target.length - prefix;
    memcpy(suffix.wire, pointer->target.wire + prefix, suffix.length);
    valid = name_make(xSDK_xLOGIC_DISCOVERY_SERVICE, &expected);
    if (!valid || !xNET_DNS_Wire_Name_Equal(&suffix, &expected))
    {
        return false;
    }
    service = record_find(cache, &pointer->target, 33, pointer->interface_index, now);
    text = record_find(cache, &pointer->target, 16, pointer->interface_index, now);
    if (!service || !text)
    {
        return false;
    }
    address = record_find(cache, &service->target, 1, pointer->interface_index, now);
    if (!address || !address->address)
    {
        return false;
    }
    valid = identity_read(text, device);
    if (!valid)
    {
        return false;
    }
    valid = name_text(&pointer->target, device->instance, true);
    if (!valid)
    {
        return false;
    }
    valid = name_text(&service->target, device->host, false);
    if (!valid)
    {
        return false;
    }
    device->address = address->address;
    device->port = service->port;
    device->interface_index = pointer->interface_index;
    return true;
}

static size_t
device_merge(struct xsdk_xlogic_discovery_device *devices, size_t count, size_t capacity, const struct xsdk_xlogic_discovery_device *device)
{
    size_t index;
    for (index = 0; index < count; index++)
    {
        if (strcmp(devices[index].identity, device->identity) == 0)
        {
            // Overlapping IPv4 links cannot be selected using a numeric TCP
            // endpoint alone. Suppress the identity rather than guess a route.
            if (devices[index].address != device->address || devices[index].port != device->port ||
                devices[index].interface_index != device->interface_index)
            {
                devices[index].port = 0;
            }
            return count;
        }
    }
    if (count < capacity)
    {
        devices[count++] = *device;
    }
    return count;
}

static int device_compare(const void *left, const void *right)
{
    const struct xsdk_xlogic_discovery_device *a = left, *b = right;
    return strcmp(a->identity, b->identity);
}

size_t xsdk_xlogic_discovery_devices(const struct xsdk_xlogic_discovery_cache *cache,
                                     uint64_t now,
                                     struct xsdk_xlogic_discovery_device *devices,
                                     size_t capacity)
{
    xNET_DNS_Wire_Name_t service;
    size_t index, count = 0;
    bool valid;
    if (!cache || !devices || !capacity)
    {
        return 0;
    }
    valid = name_make(xSDK_xLOGIC_DISCOVERY_SERVICE, &service);
    if (!valid)
    {
        return 0;
    }
    for (index = 0; index < xSDK_xLOGIC_DISCOVERY_RECORDS; index++)
    {
        const struct xsdk_xlogic_discovery_record *pointer = &cache->records[index];
        struct xsdk_xlogic_discovery_device device;
        if (pointer->type != 12U || pointer->expires <= now || !xNET_DNS_Wire_Name_Equal(&pointer->name, &service))
        {
            continue;
        }
        memset(&device, 0, sizeof(device));
        valid = device_read(cache, pointer, now, &device);
        if (!valid)
        {
            continue;
        }
        count = device_merge(devices, count, capacity, &device);
    }
    for (index = 0; index < count;)
    {
        if (!devices[index].port)
        {
            memmove(devices + index, devices + index + 1, (--count - index) * sizeof(*devices));
        }
        else
        {
            index++;
        }
    }
    qsort(devices, count, sizeof(*devices), device_compare);
    return count;
}

bool xsdk_xlogic_discovery_address(const struct xsdk_xlogic_discovery_cache *cache, const char *host, uint64_t now, uint32_t *address)
{
    xNET_DNS_Wire_Name_t name;
    size_t index;
    uint32_t found = 0;
    uint16_t interface_index = 0;
    bool valid;
    if (!cache || !host || !address)
    {
        return false;
    }
    valid = name_make(host, &name);
    if (!valid)
    {
        return false;
    }
    for (index = 0; index < xSDK_xLOGIC_DISCOVERY_RECORDS; index++)
    {
        const struct xsdk_xlogic_discovery_record *record = &cache->records[index];
        if (record->type == 1U && record->expires > now && xNET_DNS_Wire_Name_Equal(&record->name, &name))
        {
            if (found && (found != record->address || interface_index != record->interface_index))
            {
                return false;
            }
            found = record->address;
            interface_index = record->interface_index;
        }
    }
    *address = found;
    return found != 0;
}
