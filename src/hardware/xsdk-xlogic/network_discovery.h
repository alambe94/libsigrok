/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#ifndef LIBSIGROK_xSDK_xLOGIC_NETWORK_DISCOVERY_H
#define LIBSIGROK_xSDK_xLOGIC_NETWORK_DISCOVERY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xnet_dns_wire.h"

#define xSDK_xLOGIC_DISCOVERY_NAME    256U
#define xSDK_xLOGIC_DISCOVERY_RECORDS 128U
#define xSDK_xLOGIC_DISCOVERY_DEVICES 32U
#define xSDK_xLOGIC_DISCOVERY_SERVICE "_xlogic._tcp.local"

// Per-call, module-managed bounded DNS cache; records never cross NICs.
struct xsdk_xlogic_discovery_record
{
    xNET_DNS_Wire_Name_t name;
    xNET_DNS_Wire_Name_t target;
    uint8_t text[256];
    size_t text_length;
    uint32_t address;
    uint64_t expires;
    uint64_t received;
    uint16_t type, port, interface_index;
};
struct xsdk_xlogic_discovery_cache
{
    struct xsdk_xlogic_discovery_record records[xSDK_xLOGIC_DISCOVERY_RECORDS];
};
struct xsdk_xlogic_discovery_device
{
    char instance[xSDK_xLOGIC_DISCOVERY_NAME];
    char host[xSDK_xLOGIC_DISCOVERY_NAME];
    char identity[64];
    char board[64];
    uint32_t address;
    uint16_t port, interface_index;
    bool is_xesk;
};

size_t xsdk_xlogic_discovery_query(uint8_t *packet, size_t capacity, uint16_t id, const char *name, uint16_t type);
bool xsdk_xlogic_discovery_receive(struct xsdk_xlogic_discovery_cache *cache,
                                   const uint8_t *packet,
                                   size_t length,
                                   uint16_t id,
                                   uint16_t interface_index,
                                   uint64_t now);
size_t xsdk_xlogic_discovery_devices(const struct xsdk_xlogic_discovery_cache *cache,
                                     uint64_t now,
                                     struct xsdk_xlogic_discovery_device *devices,
                                     size_t capacity);
bool xsdk_xlogic_discovery_address(const struct xsdk_xlogic_discovery_cache *cache, const char *host, uint64_t now, uint32_t *address);
// IPv4 values use network byte order, including interface masks and record A data.
size_t xsdk_xlogic_mdns_browse(struct xsdk_xlogic_discovery_device *devices, size_t capacity);
bool xsdk_xlogic_mdns_resolve(const char *host, char address[16]);
#endif
