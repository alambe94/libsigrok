// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xlogic_network_wire.h
// @brief Transport-independent finite capture wire constants.

#ifndef xLOGIC_NETWORK_WIRE_H
#define xLOGIC_NETWORK_WIRE_H

#include <stdint.h>

#define xLOGIC_NET_MAGIC          UINT32_C(0x314e4c58)
#define xLOGIC_NET_VERSION        1U
#define xLOGIC_NET_REQUEST_BYTES  40U
#define xLOGIC_NET_RESPONSE_BYTES 80U
#define xLOGIC_NET_CAPS_BYTES     16U
#define xLOGIC_NET_MAX_READ_BYTES 16384U
#define xLOGIC_NET_DEFAULT_PORT   5556U
#define xLOGIC_NET_LEASE_MS       30000U

#define xLOGIC_NET_OP_CAPS       1U
#define xLOGIC_NET_OP_START      2U
#define xLOGIC_NET_OP_STATUS     3U
#define xLOGIC_NET_OP_READ       4U
#define xLOGIC_NET_OP_STOP       5U
#define xLOGIC_NET_OP_RELEASE    6U
#define xLOGIC_NET_OP_NEGOTIATE  7U
#define xLOGIC_NET_OP_READ_BLOCK 8U

// Opt-in extension; legacy CAPS, feature flags and raw READ stay byte-compatible.
#define xLOGIC_NET_BLOCK_VERSION           1U
#define xLOGIC_NET_NEGOTIATION_BYTES       40U
#define xLOGIC_NET_BLOCK_HEADER_BYTES      32U
#define xLOGIC_NET_BLOCK_MAX_DECODED       32768U
#define xLOGIC_NET_BLOCK_MAX_ENCODED       32768U
#define xLOGIC_NET_BLOCK_MAX_CREDITS       8U
#define xLOGIC_NET_BLOCK_WINDOW_CREDITS    256U
#define xLOGIC_NET_BLOCK_DECODED_SHIFT     32U
#define xLOGIC_NET_BLOCK_CREDITS_SHIFT     48U
#define xLOGIC_NET_BLOCK_MODE_MASK         UINT32_C(0x73)
#define xLOGIC_NET_BLOCK_RAW               0U
#define xLOGIC_NET_BLOCK_RLE               1U
#define xLOGIC_NET_BLOCK_PALETTE           4U
#define xLOGIC_NET_BLOCK_ACTIVE_BITS       5U
#define xLOGIC_NET_BLOCK_NIBBLE            6U
#define xLOGIC_NET_BLOCK_LAYOUT_U8         0U
#define xLOGIC_NET_BLOCK_STRICT            0U
#define xLOGIC_NET_BLOCK_ALLOW_RAW         1U
#define xLOGIC_NET_BLOCK_FLAG_RAW_FALLBACK UINT32_C(1)

#define xLOGIC_NET_STATUS_OK          0U
#define xLOGIC_NET_STATUS_INVALID     1U
#define xLOGIC_NET_STATUS_BUSY        2U
#define xLOGIC_NET_STATUS_UNSUPPORTED 3U
#define xLOGIC_NET_STATUS_CAPTURE_ID  4U
#define xLOGIC_NET_STATUS_HARDWARE    5U
#define xLOGIC_NET_STATUS_NOT_READY   6U
#define xLOGIC_NET_STATUS_RANGE       7U
#define xLOGIC_NET_STATUS_EXPIRED     8U

#define xLOGIC_NET_FLAG_FINITE       UINT32_C(1)
#define xLOGIC_NET_FLAG_RAW8         UINT32_C(2)
#define xLOGIC_NET_FLAG_EXPERIMENTAL UINT32_C(4)
#define xLOGIC_NET_FLAG_CAPTURING    UINT32_C(256)
#define xLOGIC_NET_FLAG_READY        UINT32_C(512)
#define xLOGIC_NET_FLAG_FAILED       UINT32_C(1024)
#define xLOGIC_NET_FEATURE_FLAGS     UINT32_C(7)
#define xLOGIC_NET_STATE_FLAGS       UINT32_C(1792)
#define xLOGIC_NET_KNOWN_FLAGS       UINT32_C(1799)

#endif // xLOGIC_NETWORK_WIRE_H

// EOF /////////////////////////////////////////////////////////////////////////////
