// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xlogic_network_protocol.h
// @brief Stateless finite capture framing and integrity helpers.

#ifndef xLOGIC_NETWORK_PROTOCOL_H
#define xLOGIC_NETWORK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xlogic_network_wire.h"
#include "xreturn.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint32_t opcode;
        uint32_t request_id;
        uint64_t capture_id;
        uint64_t argument0;
        uint32_t argument1;
        uint32_t flags;
    } xLOGIC_NET_Request_t;

    typedef struct
    {
        uint32_t opcode;
        uint32_t request_id;
        uint32_t status;
        uint32_t flags;
        uint64_t capture_id;
        uint64_t sample_rate;
        uint64_t sample_count;
        uint64_t offset;
        uint32_t payload_bytes;
        uint32_t payload_crc32;
        uint64_t timestamp_ticks;
        uint64_t timestamp_hz;
    } xLOGIC_NET_Response_t;

    typedef struct
    {
        uint32_t mode;
        uint32_t channel_mask;
        uint32_t layout;
        uint32_t decoded_max;
        uint32_t encoded_budget;
        uint32_t fallback;
    } xLOGIC_NET_Block_Config_t;

    // Connection negotiation keeps transport policy separate from the stable
    // block-decoder configuration. Credits are canonical: 1..256, never zero.
    typedef struct
    {
        xLOGIC_NET_Block_Config_t block;
        uint32_t credits;
    } xLOGIC_NET_Negotiation_t;

    typedef struct
    {
        uint32_t mode;
        uint32_t layout;
        uint32_t channel_mask;
        uint32_t decoded_samples;
        uint32_t encoded_bytes;
        uint32_t raw_crc32;
        uint32_t flags;
    } xLOGIC_NET_Block_t;

    // Decode exactly one fixed header. Failure preserves the output object.
    // Semantic command bounds, resource ownership and payload checks belong to
    // the service or host. Input/output overlap is supported.
    xRETURN_t xLOGIC_NET_Request_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Request_t *request);
    xRETURN_t xLOGIC_NET_Request_Encode(const xLOGIC_NET_Request_t *request, uint8_t *wire, size_t bytes);
    xRETURN_t xLOGIC_NET_Response_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Response_t *response);
    xRETURN_t xLOGIC_NET_Response_Encode(const xLOGIC_NET_Response_t *response, uint8_t *wire, size_t bytes);

    // Fixed extension payloads, little endian. These helpers validate syntax
    // and fixed limits; negotiated limits and capture identity are consumer-owned.
    // Failures preserve output. Input/output overlap is supported.
    xRETURN_t xLOGIC_NET_Negotiation_Encode(const xLOGIC_NET_Block_Config_t *config, uint8_t *wire, size_t bytes);
    xRETURN_t xLOGIC_NET_Negotiation_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Block_Config_t *config);
    // Legacy negotiation helpers above retain exactly eight credits. Window
    // helpers accept 1..256. Request packing encodes eight as zero in bits48..63;
    // request decoding maps zero back to eight. Flags remain zero for every op.
    // Validation failures preserve output; input/output overlap is supported.
    xRETURN_t xLOGIC_NET_Negotiation_Window_Encode(const xLOGIC_NET_Negotiation_t *negotiation, uint8_t *wire, size_t bytes);
    xRETURN_t xLOGIC_NET_Negotiation_Window_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Negotiation_t *negotiation);
    xRETURN_t
    xLOGIC_NET_Negotiation_Request_Encode(const xLOGIC_NET_Negotiation_t *negotiation, uint32_t request_id, xLOGIC_NET_Request_t *request);
    xRETURN_t xLOGIC_NET_Negotiation_Request_Decode(const xLOGIC_NET_Request_t *request, xLOGIC_NET_Negotiation_t *negotiation);
    xRETURN_t xLOGIC_NET_Block_Encode(const xLOGIC_NET_Block_t *block, uint8_t *wire, size_t bytes);
    xRETURN_t xLOGIC_NET_Block_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Block_t *block);

    // IEEE reflected CRC32 over payload bytes, equivalent to zlib.crc32().
    // Empty input returns zero; nonempty input requires a valid readable range.
    uint32_t xLOGIC_NET_CRC32(const void *data, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif // xLOGIC_NETWORK_PROTOCOL_H

// EOF /////////////////////////////////////////////////////////////////////////////
