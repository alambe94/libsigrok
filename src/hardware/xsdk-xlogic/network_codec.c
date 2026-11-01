/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2026 alambe94
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include <zlib.h>
#include "network_codec.h"
#include "xlogic_network_wire.h"

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static int decode_rle(const uint8_t *data, size_t bytes, uint8_t *output, size_t count)
{
    size_t position = 0U, written = 0U;

    while (position < bytes)
    {
        const uint8_t value = data[position++];
        uint32_t run = 0U;
        unsigned int shift = 0U;
        uint8_t part;

        do
        {
            if (position == bytes || shift > 14U)
            {
                return -1;
            }
            part = data[position++];
            run |= (uint32_t)(part & 127U) << shift;
            if (shift != 0U && (part & 127U) == 0U && (part & 128U) == 0U)
            {
                return -1;
            }
            shift += 7U;
        } while ((part & 128U) != 0U);
        if (run == 0U || run > count - written)
        {
            return -1;
        }
        memset(output + written, value, run);
        written += run;
    }
    return written == count ? 0 : -1;
}

static int packed_length(const uint8_t *data, size_t bytes, size_t count, unsigned int width)
{
    const size_t bits = count * width;
    const unsigned int tail = (unsigned int)(bits % 8U);

    if (bytes != (bits + 7U) / 8U)
    {
        return -1;
    }
    if (tail != 0U && (data[bytes - 1U] >> tail) != 0U)
    {
        return -1;
    }
    return 0;
}

static uint8_t packed_value(const uint8_t *data, size_t sample, unsigned int width)
{
    const size_t bit = sample * width;
    unsigned int value = data[bit / 8U] >> (bit % 8U);

    if (bit % 8U + width > 8U)
    {
        value |= (unsigned int)data[bit / 8U + 1U] << (8U - bit % 8U);
    }
    return (uint8_t)(value & ((1U << width) - 1U));
}

static int palette_dictionary_valid(const uint8_t *palette, size_t count)
{
    size_t index, previous;

    for (index = 0U; index < count; index++)
    {
        for (previous = 0U; previous < index; previous++)
        {
            if (palette[index] == palette[previous])
            {
                return -1;
            }
        }
    }
    return 0;
}

static int decode_palette(const uint8_t *data, size_t bytes, uint8_t *output, size_t count)
{
    unsigned int palette_count, width;
    size_t index;
    unsigned int seen = 0U;

    if (bytes == 0U || data[0] == 0U || data[0] > 4U)
    {
        return -1;
    }
    palette_count = data[0];
    width = palette_count == 1U ? 0U : (palette_count == 2U ? 1U : 2U);
    if (bytes < 1U + palette_count)
    {
        return -1;
    }
    if (palette_dictionary_valid(data + 1U, palette_count) != 0)
    {
        return -1;
    }
    if (packed_length(data + 1U + palette_count, bytes - 1U - palette_count, count, width) != 0)
    {
        return -1;
    }
    for (index = 0U; index < count; index++)
    {
        const unsigned int entry = width == 0U ? 0U : packed_value(data + 1U + palette_count, index, width);
        if (entry >= palette_count || entry > seen)
        {
            return -1;
        }
        seen += entry == seen ? 1U : 0U;
        output[index] = data[1U + entry];
    }
    return seen == palette_count ? 0 : -1;
}

static int decode_active(const uint8_t *data, size_t bytes, uint8_t *output, size_t count)
{
    unsigned int width = 0U, bit;
    size_t index;

    if (bytes < 2U || (data[0] & data[1]) != 0U)
    {
        return -1;
    }
    for (bit = 0U; bit < 8U; bit++)
    {
        width += (data[1] >> bit) & 1U;
    }
    if (packed_length(data + 2U, bytes - 2U, count, width) != 0)
    {
        return -1;
    }
    for (index = 0U; index < count; index++)
    {
        uint8_t dense = width == 0U ? 0U : packed_value(data + 2U, index, width);
        uint8_t value = data[0];
        for (bit = 0U; bit < 8U; bit++)
        {
            if ((data[1] & (1U << bit)) != 0U)
            {
                value |= (dense & 1U) << bit;
                dense >>= 1U;
            }
        }
        output[index] = value;
    }
    {
        uint8_t varying = 0U;
        for (index = 0U; index < count; index++)
        {
            varying |= output[index] ^ output[0];
        }
        return varying == data[1] ? 0 : -1;
    }
}

static int decode_samples(uint32_t mode, const uint8_t *data, size_t bytes, uint8_t *output, size_t count)
{
    size_t index;

    switch (mode)
    {
    case 0U:
        if (bytes != count)
        {
            return -1;
        }
        memcpy(output, data, count);
        return 0;
    case 1U:
        return decode_rle(data, bytes, output, count);
    case 4U:
        return decode_palette(data, bytes, output, count);
    case 5U:
        return decode_active(data, bytes, output, count);
    case 6U:
        if (bytes < 1U || (data[0] & 15U) != 0U)
        {
            return -1;
        }
        if (packed_length(data + 1U, bytes - 1U, count, 4U) != 0)
        {
            return -1;
        }
        for (index = 0U; index < count; index++)
        {
            output[index] = data[0] | packed_value(data + 1U, index, 4U);
        }
        return 0;
    default:
        return -1;
    }
}

static int
descriptor_mode_valid(const struct xsdk_xlogic_block_config *config, uint32_t actual, uint32_t flags, size_t encoded, size_t decoded)
{
    if (config->fallback == 1U && actual != 0U && encoded >= decoded)
    {
        return -1;
    }
    if (actual == config->mode)
    {
        return flags == 0U ? 0 : -1;
    }
    return actual == 0U && config->mode != 0U && config->fallback == 1U && flags == 1U ? 0 : -1;
}

static int
descriptor_valid(const struct xsdk_xlogic_block_config *config, const uint8_t *payload, size_t bytes, size_t count, size_t capacity)
{
    const uint32_t actual = payload[2];
    const uint32_t flags = read32(payload + 28U);

    if (payload[0] != 1U || payload[1] != 0U || payload[3] != 0U || read32(payload + 4U) != config->mask || read32(payload + 8U) != count ||
        read32(payload + 12U) != bytes - 32U || read32(payload + 20U) != 0U || read32(payload + 24U) != 0U)
    {
        return -1;
    }
    if (count == 0U || count > config->decoded_maximum || count > capacity || bytes - 32U > config->encoded_budget)
    {
        return -1;
    }
    return descriptor_mode_valid(config, actual, flags, bytes - 32U, count);
}

static int block_config_valid(const struct xsdk_xlogic_block_config *config)
{
    if (config->mode != 0U && config->mode != 1U && config->mode != 4U && config->mode != 5U && config->mode != 6U)
    {
        return 0;
    }
    return config->decoded_maximum != 0U && config->decoded_maximum <= xLOGIC_NET_BLOCK_MAX_DECODED &&
           config->encoded_budget <= xLOGIC_NET_BLOCK_MAX_ENCODED && config->encoded_budget >= config->decoded_maximum &&
           config->fallback <= 1U && (config->mask == 255U || config->mask == 15U || config->mask == 3U);
}

int xsdk_xlogic_block_decode(const struct xsdk_xlogic_block_config *config,
                             const uint8_t *payload,
                             size_t payload_bytes,
                             size_t sample_count,
                             uint8_t *output,
                             size_t output_capacity)
{
    size_t index;

    if (!config || !payload || !output || payload_bytes < 32U || sample_count > xLOGIC_NET_BLOCK_MAX_DECODED)
    {
        return -1;
    }
    if (!block_config_valid(config))
    {
        return -1;
    }
    if (descriptor_valid(config, payload, payload_bytes, sample_count, output_capacity) != 0)
    {
        return -1;
    }
    if (decode_samples(payload[2], payload + 32U, payload_bytes - 32U, output, sample_count) != 0)
    {
        return -1;
    }
    for (index = 0U; index < sample_count; index++)
    {
        if ((output[index] & ~config->mask) != 0U)
        {
            return -1;
        }
    }
    return (uint32_t)crc32(0L, output, (uInt)sample_count) == read32(payload + 16U) ? 0 : -1;
}
