// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xlogic_network_protocol.c
// @brief Explicit little-endian finite capture framing.

// INCLUDES ///////////////////////////////////////////////////////////////////////
#include "xlogic_network_protocol.h"
#include "xutil_crc32.h"

// LOCAL FUNCTIONS ////////////////////////////////////////////////////////////////
static uint16_t read_u16(const uint8_t *wire)
{
    return (uint16_t)((uint16_t)wire[0] | ((uint16_t)wire[1] << 8U));
}

static uint32_t read_u32(const uint8_t *wire)
{
    return (uint32_t)wire[0] | ((uint32_t)wire[1] << 8U) | ((uint32_t)wire[2] << 16U) | ((uint32_t)wire[3] << 24U);
}

static uint64_t read_u64(const uint8_t *wire)
{
    return (uint64_t)read_u32(wire) | ((uint64_t)read_u32(wire + 4U) << 32U);
}

static void write_u16(uint8_t *wire, uint16_t value)
{
    wire[0] = (uint8_t)value;
    wire[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *wire, uint32_t value)
{
    for (uint32_t index = 0U; index < 4U; ++index)
    {
        wire[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_u64(uint8_t *wire, uint64_t value)
{
    write_u32(wire, (uint32_t)value);
    write_u32(wire + 4U, (uint32_t)(value >> 32U));
}

static bool opcode_valid(uint32_t opcode)
{
    return (opcode >= xLOGIC_NET_OP_CAPS) && (opcode <= xLOGIC_NET_OP_READ_BLOCK);
}

static bool prefix_valid(const uint8_t *wire, size_t bytes, uint16_t expected)
{
    return (wire != NULL) && (bytes == expected) && (read_u32(wire) == xLOGIC_NET_MAGIC) && (read_u16(wire + 4U) == xLOGIC_NET_VERSION) &&
           (read_u16(wire + 6U) == expected);
}

static void prefix_write(uint8_t *wire, uint16_t bytes)
{
    write_u32(wire, xLOGIC_NET_MAGIC);
    write_u16(wire + 4U, xLOGIC_NET_VERSION);
    write_u16(wire + 6U, bytes);
}

static bool response_valid(const xLOGIC_NET_Response_t *response)
{
    const uint32_t states = response->flags & xLOGIC_NET_STATE_FLAGS;
    const uint32_t payload_max = response->opcode == xLOGIC_NET_OP_READ_BLOCK ? xLOGIC_NET_BLOCK_HEADER_BYTES + xLOGIC_NET_BLOCK_MAX_ENCODED
                                                                              : xLOGIC_NET_MAX_READ_BYTES;
    return opcode_valid(response->opcode) && (response->status <= xLOGIC_NET_STATUS_EXPIRED) &&
           ((response->flags & ~xLOGIC_NET_KNOWN_FLAGS) == 0U) &&
           ((response->flags & xLOGIC_NET_FEATURE_FLAGS) == xLOGIC_NET_FEATURE_FLAGS) && ((states & (states - 1U)) == 0U) &&
           (response->payload_bytes <= payload_max) && ((response->payload_bytes != 0U) || (response->payload_crc32 == 0U));
}

// MODULE FUNCTIONS IMPLEMENTATION /////////////////////////////////////////////////
xRETURN_t xLOGIC_NET_Request_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Request_t *request)
{
    if ((request == NULL) || (wire == NULL))
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (!prefix_valid(wire, bytes, xLOGIC_NET_REQUEST_BYTES))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Request_t decoded = {.opcode = read_u32(wire + 8U),
                                          .request_id = read_u32(wire + 12U),
                                          .capture_id = read_u64(wire + 16U),
                                          .argument0 = read_u64(wire + 24U),
                                          .argument1 = read_u32(wire + 32U),
                                          .flags = read_u32(wire + 36U)};
    if (!opcode_valid(decoded.opcode) || (decoded.flags != 0U))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    *request = decoded;
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Request_Encode(const xLOGIC_NET_Request_t *request, uint8_t *wire, size_t bytes)
{
    if ((request == NULL) || (wire == NULL))
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if ((bytes != xLOGIC_NET_REQUEST_BYTES) || !opcode_valid(request->opcode) || (request->flags != 0U))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Request_t copy = *request;
    prefix_write(wire, xLOGIC_NET_REQUEST_BYTES);
    write_u32(wire + 8U, copy.opcode);
    write_u32(wire + 12U, copy.request_id);
    write_u64(wire + 16U, copy.capture_id);
    write_u64(wire + 24U, copy.argument0);
    write_u32(wire + 32U, copy.argument1);
    write_u32(wire + 36U, copy.flags);
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Response_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Response_t *response)
{
    if ((response == NULL) || (wire == NULL))
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (!prefix_valid(wire, bytes, xLOGIC_NET_RESPONSE_BYTES))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Response_t decoded = {.opcode = read_u32(wire + 8U),
                                           .request_id = read_u32(wire + 12U),
                                           .status = read_u32(wire + 16U),
                                           .flags = read_u32(wire + 20U),
                                           .capture_id = read_u64(wire + 24U),
                                           .sample_rate = read_u64(wire + 32U),
                                           .sample_count = read_u64(wire + 40U),
                                           .offset = read_u64(wire + 48U),
                                           .payload_bytes = read_u32(wire + 56U),
                                           .payload_crc32 = read_u32(wire + 60U),
                                           .timestamp_ticks = read_u64(wire + 64U),
                                           .timestamp_hz = read_u64(wire + 72U)};
    if (!response_valid(&decoded))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    *response = decoded;
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Response_Encode(const xLOGIC_NET_Response_t *response, uint8_t *wire, size_t bytes)
{
    if ((response == NULL) || (wire == NULL))
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if ((bytes != xLOGIC_NET_RESPONSE_BYTES) || !response_valid(response))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Response_t copy = *response;
    prefix_write(wire, xLOGIC_NET_RESPONSE_BYTES);
    write_u32(wire + 8U, copy.opcode);
    write_u32(wire + 12U, copy.request_id);
    write_u32(wire + 16U, copy.status);
    write_u32(wire + 20U, copy.flags);
    write_u64(wire + 24U, copy.capture_id);
    write_u64(wire + 32U, copy.sample_rate);
    write_u64(wire + 40U, copy.sample_count);
    write_u64(wire + 48U, copy.offset);
    write_u32(wire + 56U, copy.payload_bytes);
    write_u32(wire + 60U, copy.payload_crc32);
    write_u64(wire + 64U, copy.timestamp_ticks);
    write_u64(wire + 72U, copy.timestamp_hz);
    return xRETURN_OK;
}

uint32_t xLOGIC_NET_CRC32(const void *data, size_t bytes)
{
    return xUTIL_CRC32_IEEE_Calculate(data, bytes);
}

static bool block_selection_valid(uint32_t mode, uint32_t mask, uint32_t layout)
{
    const bool mode_valid = mode < 32U && (xLOGIC_NET_BLOCK_MODE_MASK & ((uint32_t)1U << mode)) != 0U;
    return mode_valid && (mask == 0xffU || mask == 0x0fU || mask == 0x03U) && layout == xLOGIC_NET_BLOCK_LAYOUT_U8;
}

static bool block_config_valid(const xLOGIC_NET_Block_Config_t *config)
{
    return block_selection_valid(config->mode, config->channel_mask, config->layout) && config->decoded_max != 0U &&
           config->decoded_max <= xLOGIC_NET_BLOCK_MAX_DECODED && config->encoded_budget >= config->decoded_max &&
           config->encoded_budget <= xLOGIC_NET_BLOCK_MAX_ENCODED && config->fallback <= xLOGIC_NET_BLOCK_ALLOW_RAW;
}

static bool block_valid(const xLOGIC_NET_Block_t *block)
{
    return block_selection_valid(block->mode, block->channel_mask, block->layout) && block->decoded_samples != 0U &&
           block->decoded_samples <= xLOGIC_NET_BLOCK_MAX_DECODED && block->encoded_bytes != 0U &&
           block->encoded_bytes <= xLOGIC_NET_BLOCK_MAX_ENCODED && (block->flags & ~xLOGIC_NET_BLOCK_FLAG_RAW_FALLBACK) == 0U &&
           (block->flags == 0U || block->mode == xLOGIC_NET_BLOCK_RAW) &&
           (block->mode != xLOGIC_NET_BLOCK_RAW || block->encoded_bytes == block->decoded_samples);
}

xRETURN_t xLOGIC_NET_Negotiation_Encode(const xLOGIC_NET_Block_Config_t *config, uint8_t *wire, size_t bytes)
{
    if (config == NULL || wire == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    const xLOGIC_NET_Negotiation_t negotiation = {.block = *config, .credits = xLOGIC_NET_BLOCK_MAX_CREDITS};
    return xLOGIC_NET_Negotiation_Window_Encode(&negotiation, wire, bytes);
}

xRETURN_t xLOGIC_NET_Negotiation_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Block_Config_t *config)
{
    if (wire == NULL || config == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    xLOGIC_NET_Negotiation_t negotiation;
    const xRETURN_t status = xLOGIC_NET_Negotiation_Window_Decode(wire, bytes, &negotiation);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (negotiation.credits != xLOGIC_NET_BLOCK_MAX_CREDITS)
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    *config = negotiation.block;
    return xRETURN_OK;
}

static bool negotiation_valid(const xLOGIC_NET_Negotiation_t *negotiation)
{
    return block_config_valid(&negotiation->block) && negotiation->credits != 0U && negotiation->credits <= xLOGIC_NET_BLOCK_WINDOW_CREDITS;
}

xRETURN_t xLOGIC_NET_Negotiation_Window_Encode(const xLOGIC_NET_Negotiation_t *negotiation, uint8_t *wire, size_t bytes)
{
    if (negotiation == NULL || wire == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (bytes != xLOGIC_NET_NEGOTIATION_BYTES || !negotiation_valid(negotiation))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Negotiation_t copy = *negotiation;
    write_u32(wire, xLOGIC_NET_BLOCK_VERSION);
    write_u32(wire + 4U, xLOGIC_NET_BLOCK_MODE_MASK);
    write_u32(wire + 8U, copy.block.mode);
    write_u32(wire + 12U, copy.block.channel_mask);
    write_u32(wire + 16U, copy.block.layout);
    write_u32(wire + 20U, copy.block.decoded_max);
    write_u32(wire + 24U, copy.block.encoded_budget);
    write_u32(wire + 28U, copy.block.fallback);
    write_u32(wire + 32U, copy.credits);
    write_u32(wire + 36U, xLOGIC_NET_LEASE_MS);
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Negotiation_Window_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Negotiation_t *negotiation)
{
    if (wire == NULL || negotiation == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (bytes != xLOGIC_NET_NEGOTIATION_BYTES)
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    if (read_u32(wire) != xLOGIC_NET_BLOCK_VERSION || read_u32(wire + 4U) != xLOGIC_NET_BLOCK_MODE_MASK ||
        read_u32(wire + 36U) != xLOGIC_NET_LEASE_MS)
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Negotiation_t decoded = {.block = {.mode = read_u32(wire + 8U),
                                                        .channel_mask = read_u32(wire + 12U),
                                                        .layout = read_u32(wire + 16U),
                                                        .decoded_max = read_u32(wire + 20U),
                                                        .encoded_budget = read_u32(wire + 24U),
                                                        .fallback = read_u32(wire + 28U)},
                                              .credits = read_u32(wire + 32U)};
    if (!negotiation_valid(&decoded))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    *negotiation = decoded;
    return xRETURN_OK;
}

xRETURN_t
xLOGIC_NET_Negotiation_Request_Encode(const xLOGIC_NET_Negotiation_t *negotiation, uint32_t request_id, xLOGIC_NET_Request_t *request)
{
    if (negotiation == NULL || request == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (!negotiation_valid(negotiation))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const uint64_t credits = negotiation->credits == xLOGIC_NET_BLOCK_MAX_CREDITS ? 0U : negotiation->credits;
    const xLOGIC_NET_Request_t encoded = {
        .opcode = xLOGIC_NET_OP_NEGOTIATE,
        .request_id = request_id,
        .argument0 = (uint64_t)negotiation->block.mode | ((uint64_t)negotiation->block.channel_mask << 8U) |
                     ((uint64_t)negotiation->block.layout << 16U) | ((uint64_t)negotiation->block.fallback << 24U) |
                     ((uint64_t)negotiation->block.decoded_max << xLOGIC_NET_BLOCK_DECODED_SHIFT) |
                     (credits << xLOGIC_NET_BLOCK_CREDITS_SHIFT),
        .argument1 = negotiation->block.encoded_budget};
    *request = encoded;
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Negotiation_Request_Decode(const xLOGIC_NET_Request_t *request, xLOGIC_NET_Negotiation_t *negotiation)
{
    if (request == NULL || negotiation == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (request->opcode != xLOGIC_NET_OP_NEGOTIATE || request->capture_id != 0U || request->flags != 0U)
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const uint32_t credits = (uint16_t)(request->argument0 >> xLOGIC_NET_BLOCK_CREDITS_SHIFT);
    const xLOGIC_NET_Negotiation_t decoded = {.block = {.mode = (uint8_t)request->argument0,
                                                        .channel_mask = (uint8_t)(request->argument0 >> 8U),
                                                        .layout = (uint8_t)(request->argument0 >> 16U),
                                                        .fallback = (uint8_t)(request->argument0 >> 24U),
                                                        .decoded_max = (uint16_t)(request->argument0 >> xLOGIC_NET_BLOCK_DECODED_SHIFT),
                                                        .encoded_budget = request->argument1},
                                              .credits = credits == 0U ? xLOGIC_NET_BLOCK_MAX_CREDITS : credits};
    if (!negotiation_valid(&decoded))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    *negotiation = decoded;
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Block_Encode(const xLOGIC_NET_Block_t *block, uint8_t *wire, size_t bytes)
{
    if (block == NULL || wire == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (bytes != xLOGIC_NET_BLOCK_HEADER_BYTES || !block_valid(block))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Block_t copy = *block;
    write_u16(wire, xLOGIC_NET_BLOCK_VERSION);
    wire[2] = (uint8_t)copy.mode;
    wire[3] = (uint8_t)copy.layout;
    write_u32(wire + 4U, copy.channel_mask);
    write_u32(wire + 8U, copy.decoded_samples);
    write_u32(wire + 12U, copy.encoded_bytes);
    write_u32(wire + 16U, copy.raw_crc32);
    write_u32(wire + 20U, 0U);
    write_u32(wire + 24U, 0U);
    write_u32(wire + 28U, copy.flags);
    return xRETURN_OK;
}

xRETURN_t xLOGIC_NET_Block_Decode(const uint8_t *wire, size_t bytes, xLOGIC_NET_Block_t *block)
{
    if (wire == NULL || block == NULL)
    {
        return xRETURN_xERR_NULL_PTR;
    }
    if (bytes != xLOGIC_NET_BLOCK_HEADER_BYTES)
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    if (read_u16(wire) != xLOGIC_NET_BLOCK_VERSION || read_u32(wire + 20U) != 0U || read_u32(wire + 24U) != 0U)
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    const xLOGIC_NET_Block_t decoded = {.mode = wire[2],
                                        .layout = wire[3],
                                        .channel_mask = read_u32(wire + 4U),
                                        .decoded_samples = read_u32(wire + 8U),
                                        .encoded_bytes = read_u32(wire + 12U),
                                        .raw_crc32 = read_u32(wire + 16U),
                                        .flags = read_u32(wire + 28U)};
    if (!block_valid(&decoded))
    {
        return xRETURN_xERR_INVALID_ARG;
    }
    *block = decoded;
    return xRETURN_OK;
}

// EOF /////////////////////////////////////////////////////////////////////////////
