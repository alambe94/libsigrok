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

#ifndef LIBSIGROK_xSDK_xLOGIC_NETWORK_CODEC_H
#define LIBSIGROK_xSDK_xLOGIC_NETWORK_CODEC_H
#include <stddef.h>
#include <stdint.h>

struct xsdk_xlogic_block_config
{
    uint32_t mode;
    uint32_t mask;
    uint32_t fallback;
    uint32_t decoded_maximum;
    uint32_t encoded_budget;
};

/* Independent wire decoder. Output is private scratch until success; an error
 * may modify scratch but never authorizes publication or an offset advance. */
int xsdk_xlogic_block_decode(const struct xsdk_xlogic_block_config *config,
                             const uint8_t *payload,
                             size_t payload_bytes,
                             size_t sample_count,
                             uint8_t *output,
                             size_t output_capacity);
#endif
