// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_logic_network.h
// @brief Finite xLOGIC records inside common service framing.

#ifndef xESK_LOGIC_NETWORK_H
#define xESK_LOGIC_NETWORK_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stddef.h>
#include <stdint.h>
// MODULE INCLUDES
#include "xesk_control.h"
#include "xlogic_network_protocol.h"

#ifdef __cplusplus
extern "C"
{
#endif

// PUBLIC API /////////////////////////////////////////////////////////

// MACROS /////////////////////////////////////////////////////////////////////////
#define xESK_LOGIC_CAPTURE_VERSION        2U
#define xESK_LOGIC_CAPTURE_REQUEST_BYTES  40U
#define xESK_LOGIC_CAPTURE_RESPONSE_BYTES 80U
#define xESK_LOGIC_CAPTURE_FEATURE_FLAGS  5U
#define xESK_LOGIC_NETWORK_DATA_BYTES     (xESK_MAX_FRAME_BYTES - xESK_LOGIC_CAPTURE_RESPONSE_BYTES)
#define xESK_LOGIC_FORMAT_RAW8            1U
#define xESK_LOGIC_FORMAT_PACKED4         2U
#define xESK_LOGIC_RATE_PROFILE_1_COUNT   5U

    // TYPES //////////////////////////////////////////////////////////////////////////
    // Common finite v2 is distinct from the standalone XLN1 protocol. READ
    // offsets/counts are stored bytes; logical samples never change units.
    typedef struct
    {
        uint32_t opcode;
        uint16_t format;
        uint16_t channel_mask;
        uint64_t capture_id;
        uint64_t argument0;
        uint32_t argument1;
    } xESK_Logic_Request_t;
    typedef struct
    {
        uint32_t opcode;
        uint32_t status;
        uint16_t flags;
        uint8_t format;
        uint8_t channel_mask;
        uint32_t payload_bytes;
        uint64_t capture_id;
        uint32_t sample_rate;
        uint32_t payload_crc32;
        uint64_t sample_count;
        uint64_t stored_bytes;
        uint64_t offset;
        uint64_t timestamp_ticks;
        uint64_t timestamp_hz;
    } xESK_Logic_Metadata_t;
    typedef struct
    {
        xESK_Logic_Metadata_t header;
        uint8_t data[xESK_LOGIC_NETWORK_DATA_BYTES];
    } xESK_Logic_Network_Reply_t;

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    // Common control supplies boot/session/request identity and resource manager
    // ownership. Finite v2 creates no second request sequence, lease, listener or
    // capture engine. The qualified standalone XLN1 protocol is unchanged. Supported
    // Control-reply operations are START, STATUS, READ, STOP (cancel), RELEASE
    // (finish). READ is bounded to DATA_BYTES. READ_BLOCK is a separately
    // staged data request, never a common-control data reply. Rates, geometry
    // and arena are provider policy.
    // Call-scoped, disjoint typed objects. Failures preserve every output.
    // Profile 1 defines RAW8 network rates and is advertised explicitly in
    // the LOGIC service descriptor. An out-of-range index returns zero.
    uint32_t xESK_Logic_Rate_Profile_1(size_t index);
    xRETURN_t xESK_Logic_Stored_Bytes(uint16_t format, uint16_t channel_mask, uint64_t samples, uint64_t *bytes);
    xRETURN_t xESK_Logic_Network_Request_Encode(const xESK_Logic_Request_t *request, xESK_Control_Message_t *control);
    xRETURN_t xESK_Logic_Network_Request_Decode(const xESK_Control_Message_t *control, xESK_Logic_Request_t *request);
    // Replies echo capture identity (including STOP/RELEASE), exact READ offset
    // and length. CRC is IEEE reflected CRC32 over the bounded payload. A
    // successful STOP/RELEASE means physical quiescence and arena return, never
    // merely queued cleanup. The caller still releases its common LOGIC lease.
    xRETURN_t xESK_Logic_Network_Reply_Encode(const xESK_Control_Message_t *request,
                                              const xESK_Logic_Network_Reply_t *reply,
                                              xESK_Control_Message_t *control);
    xRETURN_t xESK_Logic_Network_Reply_Decode(const xESK_Control_Message_t *request,
                                              const xESK_Control_Message_t *control,
                                              xESK_Logic_Network_Reply_t *reply);
    // READ_BLOCK returns an 80-byte metadata header in common control, followed
    // by separately transported bytes. Decode validates the correlated header;
    // the receiver must verify payload_crc32 after receiving all payload bytes.
    xRETURN_t xESK_Logic_Network_Block_Header_Encode(const xESK_Control_Message_t *request,
                                                     const xESK_Logic_Metadata_t *metadata,
                                                     xESK_Control_Message_t *control);
    xRETURN_t xESK_Logic_Network_Block_Header_Decode(const xESK_Control_Message_t *request,
                                                     const xESK_Control_Message_t *control,
                                                     xESK_Logic_Metadata_t *metadata);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_LOGIC_NETWORK_H

// EOF /////////////////////////////////////////////////////////////////////////////
