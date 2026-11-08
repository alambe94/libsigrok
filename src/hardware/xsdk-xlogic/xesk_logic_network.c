// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_logic_network.c
// @brief Finite xLOGIC common-service codec.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <string.h>

// MODULE INCLUDES
#include "xesk_logic_network.h"
#include "xesk_control_internal.h"
#include "xbytes.h"

static const uint8_t wire_magic[4] = {'x', 'L', 'C', '2'};

uint32_t xESK_Logic_Rate_Profile_1(size_t index)
{
    static const uint32_t rates[xESK_LOGIC_RATE_PROFILE_1_COUNT] = {xESK_LOGIC_RATE_PROFILE_1_MAX_HZ, 333333333U, 166666666U, 83333333U,
                                                                    41666666U};
    return index < xESK_LOGIC_RATE_PROFILE_1_COUNT ? rates[index] : 0U;
}

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static bool
objects_disjoint(const void *left, size_t left_bytes, size_t left_alignment, const void *right, size_t right_bytes, size_t right_alignment)
{
    return xESK_Control_Range_Valid(left, left_bytes, left_alignment) && xESK_Control_Range_Valid(right, right_bytes, right_alignment) &&
           xESK_Control_Ranges_Disjoint(left, left_bytes, right, right_bytes);
}

static bool
block_objects_disjoint(const xESK_Control_Message_t *request, const xESK_Control_Message_t *control, const xESK_Logic_Metadata_t *metadata)
{
    return objects_disjoint(request, sizeof(*request), _Alignof(xESK_Control_Message_t), metadata, sizeof(*metadata),
                            _Alignof(xESK_Logic_Metadata_t)) &&
           objects_disjoint(request, sizeof(*request), _Alignof(xESK_Control_Message_t), control, sizeof(*control),
                            _Alignof(xESK_Control_Message_t)) &&
           objects_disjoint(metadata, sizeof(*metadata), _Alignof(xESK_Logic_Metadata_t), control, sizeof(*control),
                            _Alignof(xESK_Control_Message_t));
}

static bool envelope_valid(const xESK_Control_Message_t *control, uint8_t kind)
{
    return control->kind == kind && control->opcode == xESK_CONTROL_SERVICE && control->status == xESK_CONTROL_OK &&
           control->service_id == xESK_SERVICE_LOGIC && control->instance_id == 0U && control->boot_id != 0U && control->session_id != 0U &&
           !control->has_record;
}

static bool geometry_valid(uint16_t format, uint16_t mask, uint64_t samples)
{
    return samples != 0U && ((format == xESK_LOGIC_FORMAT_RAW8 && mask == 0xffU) ||
                             (format == xESK_LOGIC_FORMAT_PACKED4 && (mask == 0x0fU || mask == 0xf0U) && samples % 2U == 0U));
}
static uint64_t stored_bytes(uint16_t format, uint64_t samples)
{
    return format == xESK_LOGIC_FORMAT_PACKED4 ? samples / 2U : samples;
}
static bool read_request_valid(const xESK_Logic_Request_t *request, uint32_t limit)
{
    return request->argument1 != 0U && request->argument1 <= limit && request->argument0 <= UINT64_MAX - request->argument1;
}
static bool request_valid(const xESK_Logic_Request_t *request)
{
    if (request->opcode == xLOGIC_NET_OP_START)
    {
        return request->capture_id == 0U && request->argument1 != 0U &&
               geometry_valid(request->format, request->channel_mask, request->argument0);
    }
    if (request->capture_id == 0U || request->format != 0U || request->channel_mask != 0U)
    {
        return false;
    }
    if (request->opcode == xLOGIC_NET_OP_READ || request->opcode == xLOGIC_NET_OP_READ_BLOCK)
    {
        const uint32_t limit = request->opcode == xLOGIC_NET_OP_READ ? xESK_LOGIC_NETWORK_DATA_BYTES : xLOGIC_NET_BLOCK_MAX_DECODED;
        return read_request_valid(request, limit);
    }
    return (request->opcode == xLOGIC_NET_OP_STATUS || request->opcode == xLOGIC_NET_OP_STOP || request->opcode == xLOGIC_NET_OP_RELEASE) &&
           request->argument0 == 0U && request->argument1 == 0U;
}
static bool metadata_valid(const xESK_Logic_Metadata_t *header)
{
    return header->capture_id != 0U && header->sample_rate != 0U && header->timestamp_hz != 0U &&
           (header->flags & xLOGIC_NET_STATE_FLAGS) != 0U && geometry_valid(header->format, header->channel_mask, header->sample_count) &&
           header->stored_bytes == stored_bytes(header->format, header->sample_count);
}
static bool wire_header_valid(const uint8_t *wire, size_t length, uint16_t expected)
{
    return length >= expected && memcmp(wire, wire_magic, sizeof(wire_magic)) == 0 && xRead_LE16(wire + 4U) == xESK_LOGIC_CAPTURE_VERSION &&
           xRead_LE16(wire + 6U) == expected;
}
static void write_header(uint8_t *wire, uint16_t length)
{
    (void)memset(wire, 0, length);
    xWrite_LE32(wire, UINT32_C(0x32434c78)); // xLC2, a four-byte wire tag.
    xWrite_LE16(wire + 4U, xESK_LOGIC_CAPTURE_VERSION);
    xWrite_LE16(wire + 6U, length);
}
static void request_write(const xESK_Logic_Request_t *request, uint8_t *wire)
{
    write_header(wire, xESK_LOGIC_CAPTURE_REQUEST_BYTES);
    xWrite_LE32(wire + 8U, request->opcode);
    xWrite_LE16(wire + 12U, request->format);
    xWrite_LE16(wire + 14U, request->channel_mask);
    xWrite_LE64(wire + 16U, request->capture_id);
    xWrite_LE64(wire + 24U, request->argument0);
    xWrite_LE32(wire + 32U, request->argument1);
}
static xRETURN_t request_read(const uint8_t *wire, size_t length, xESK_Logic_Request_t *request)
{
    if (length != xESK_LOGIC_CAPTURE_REQUEST_BYTES || !wire_header_valid(wire, length, xESK_LOGIC_CAPTURE_REQUEST_BYTES) ||
        xRead_LE32(wire + 36U) != 0U)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *request = (xESK_Logic_Request_t){.opcode = xRead_LE32(wire + 8U),
                                      .format = xRead_LE16(wire + 12U),
                                      .channel_mask = xRead_LE16(wire + 14U),
                                      .capture_id = xRead_LE64(wire + 16U),
                                      .argument0 = xRead_LE64(wire + 24U),
                                      .argument1 = xRead_LE32(wire + 32U)};
    return xRETURN_OK;
}
static void metadata_write(const xESK_Logic_Metadata_t *header, uint8_t *wire)
{
    write_header(wire, xESK_LOGIC_CAPTURE_RESPONSE_BYTES);
    xWrite_LE32(wire + 8U, header->opcode);
    xWrite_LE32(wire + 12U, header->status);
    xWrite_LE16(wire + 16U, header->flags);
    wire[18] = header->format;
    wire[19] = header->channel_mask;
    xWrite_LE32(wire + 20U, header->payload_bytes);
    xWrite_LE64(wire + 24U, header->capture_id);
    xWrite_LE32(wire + 32U, header->sample_rate);
    xWrite_LE32(wire + 36U, header->payload_crc32);
    xWrite_LE64(wire + 40U, header->sample_count);
    xWrite_LE64(wire + 48U, header->stored_bytes);
    xWrite_LE64(wire + 56U, header->offset);
    xWrite_LE64(wire + 64U, header->timestamp_ticks);
    xWrite_LE64(wire + 72U, header->timestamp_hz);
}
static void metadata_read(const uint8_t *wire, xESK_Logic_Metadata_t *header)
{
    *header = (xESK_Logic_Metadata_t){.opcode = xRead_LE32(wire + 8U),
                                      .status = xRead_LE32(wire + 12U),
                                      .flags = xRead_LE16(wire + 16U),
                                      .format = wire[18],
                                      .channel_mask = wire[19],
                                      .payload_bytes = xRead_LE32(wire + 20U),
                                      .capture_id = xRead_LE64(wire + 24U),
                                      .sample_rate = xRead_LE32(wire + 32U),
                                      .payload_crc32 = xRead_LE32(wire + 36U),
                                      .sample_count = xRead_LE64(wire + 40U),
                                      .stored_bytes = xRead_LE64(wire + 48U),
                                      .offset = xRead_LE64(wire + 56U),
                                      .timestamp_ticks = xRead_LE64(wire + 64U),
                                      .timestamp_hz = xRead_LE64(wire + 72U)};
}

static bool start_reply_matches(const xESK_Logic_Request_t *request, const xESK_Logic_Metadata_t *header)
{
    return header->sample_rate == request->argument1 && header->sample_count == request->argument0 && header->format == request->format &&
           header->channel_mask == request->channel_mask;
}
static bool successful_reply_matches(const xESK_Logic_Request_t *request, const xESK_Logic_Metadata_t *header)
{
    if (request->opcode == xLOGIC_NET_OP_STOP || request->opcode == xLOGIC_NET_OP_RELEASE)
    {
        return header->payload_bytes == 0U && (header->flags & xLOGIC_NET_STATE_FLAGS) == 0U;
    }
    if (!metadata_valid(header))
    {
        return false;
    }
    if (request->opcode == xLOGIC_NET_OP_READ)
    {
        return (header->flags & xLOGIC_NET_FLAG_READY) != 0U && header->offset == request->argument0 &&
               header->payload_bytes == request->argument1 && header->offset <= header->stored_bytes &&
               header->payload_bytes <= header->stored_bytes - header->offset;
    }
    if (request->opcode == xLOGIC_NET_OP_START && !start_reply_matches(request, header))
    {
        return false;
    }
    return header->payload_bytes == 0U && header->offset == 0U;
}

static bool reply_matches(const xESK_Logic_Request_t *request, const xESK_Logic_Network_Reply_t *reply)
{
    const xESK_Logic_Metadata_t *header = &reply->header;
    if (request->opcode == xLOGIC_NET_OP_READ_BLOCK || header->opcode != request->opcode ||
        header->payload_bytes > xESK_LOGIC_NETWORK_DATA_BYTES ||
        (request->opcode != xLOGIC_NET_OP_START && header->capture_id != request->capture_id))
    {
        return false;
    }
    const uint16_t state = header->flags & xLOGIC_NET_STATE_FLAGS;
    if (header->status > xLOGIC_NET_STATUS_EXPIRED ||
        (header->flags & ~(xESK_LOGIC_CAPTURE_FEATURE_FLAGS | xLOGIC_NET_STATE_FLAGS)) != 0U ||
        (header->flags & xESK_LOGIC_CAPTURE_FEATURE_FLAGS) != xESK_LOGIC_CAPTURE_FEATURE_FLAGS || (state & (state - 1U)) != 0U)
    {
        return false;
    }
    if (header->payload_crc32 != xLOGIC_NET_CRC32(reply->data, header->payload_bytes))
    {
        return false;
    }
    return header->status == xLOGIC_NET_STATUS_OK ? successful_reply_matches(request, header) : header->payload_bytes == 0U;
}

static bool block_header_matches(const xESK_Logic_Request_t *request, const xESK_Logic_Metadata_t *metadata)
{
    return request->opcode == xLOGIC_NET_OP_READ_BLOCK && metadata->opcode == xLOGIC_NET_OP_READ_BLOCK &&
           metadata->status == xLOGIC_NET_STATUS_OK && metadata->flags == (xESK_LOGIC_CAPTURE_FEATURE_FLAGS | xLOGIC_NET_FLAG_READY) &&
           metadata_valid(metadata) && metadata->capture_id == request->capture_id && metadata->offset == request->argument0 &&
           metadata->payload_bytes == request->argument1 && metadata->offset <= metadata->stored_bytes &&
           metadata->payload_bytes <= metadata->stored_bytes - metadata->offset;
}

// PUBLIC API IMPLEMENTATION /////////////////////////////////////////////////////
xRETURN_t xESK_Logic_Stored_Bytes(uint16_t format, uint16_t channel_mask, uint64_t samples, uint64_t *bytes)
{
    if (!xESK_Control_Range_Valid(bytes, sizeof(*bytes), _Alignof(uint64_t)) || !geometry_valid(format, channel_mask, samples))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    *bytes = stored_bytes(format, samples);
    return xRETURN_OK;
}

xRETURN_t xESK_Logic_Network_Request_Encode(const xESK_Logic_Request_t *request, xESK_Control_Message_t *control)
{
    if (request == NULL || control == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!objects_disjoint(request, sizeof(*request), _Alignof(xESK_Logic_Request_t), control, sizeof(*control),
                          _Alignof(xESK_Control_Message_t)) ||
        !envelope_valid(control, xESK_CONTROL_REQUEST) || !request_valid(request))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Control_Message_t candidate = *control;
    (void)memset(&candidate.payload, 0, sizeof(candidate.payload));
    candidate.payload.length = xESK_LOGIC_CAPTURE_REQUEST_BYTES;
    request_write(request, candidate.payload.data);
    xRETURN_t status;
    status = xESK_Control_Validate(&candidate);
    if (status == xRETURN_OK)
    {
        *control = candidate;
    }
    return status;
}

xRETURN_t xESK_Logic_Network_Request_Decode(const xESK_Control_Message_t *control, xESK_Logic_Request_t *request)
{
    if (request == NULL || control == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!objects_disjoint(request, sizeof(*request), _Alignof(xESK_Logic_Request_t), control, sizeof(*control),
                          _Alignof(xESK_Control_Message_t)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xRETURN_t status = xESK_Control_Validate(control);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!envelope_valid(control, xESK_CONTROL_REQUEST))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    xESK_Logic_Request_t candidate = {0};
    status = request_read(control->payload.data, control->payload.length, &candidate);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!request_valid(&candidate))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *request = candidate;
    return xRETURN_OK;
}

xRETURN_t xESK_Logic_Network_Reply_Encode(const xESK_Control_Message_t *request,
                                          const xESK_Logic_Network_Reply_t *reply,
                                          xESK_Control_Message_t *control)
{
    if (request == NULL || reply == NULL || control == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!objects_disjoint(reply, sizeof(*reply), _Alignof(xESK_Logic_Network_Reply_t), control, sizeof(*control),
                          _Alignof(xESK_Control_Message_t)) ||
        !objects_disjoint(request, sizeof(*request), _Alignof(xESK_Control_Message_t), control, sizeof(*control),
                          _Alignof(xESK_Control_Message_t)) ||
        !xESK_Control_Ranges_Disjoint(request, sizeof(*request), reply, sizeof(*reply)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Logic_Request_t decoded = {0};
    xRETURN_t status = xESK_Logic_Network_Request_Decode(request, &decoded);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!reply_matches(&decoded, reply))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Control_Message_t candidate = *request;
    candidate.kind = xESK_CONTROL_RESPONSE;
    (void)memset(&candidate.payload, 0, sizeof(candidate.payload));
    candidate.payload.length = xESK_LOGIC_CAPTURE_RESPONSE_BYTES + reply->header.payload_bytes;
    metadata_write(&reply->header, candidate.payload.data);
    (void)memcpy(candidate.payload.data + xESK_LOGIC_CAPTURE_RESPONSE_BYTES, reply->data, reply->header.payload_bytes);
    status = xESK_Control_Reply_Validate(request, &candidate);
    if (status == xRETURN_OK)
    {
        *control = candidate;
    }
    return status;
}

xRETURN_t xESK_Logic_Network_Reply_Decode(const xESK_Control_Message_t *request,
                                          const xESK_Control_Message_t *control,
                                          xESK_Logic_Network_Reply_t *reply)
{
    if (request == NULL || reply == NULL || control == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!objects_disjoint(reply, sizeof(*reply), _Alignof(xESK_Logic_Network_Reply_t), control, sizeof(*control),
                          _Alignof(xESK_Control_Message_t)) ||
        !objects_disjoint(request, sizeof(*request), _Alignof(xESK_Control_Message_t), reply, sizeof(*reply),
                          _Alignof(xESK_Logic_Network_Reply_t)) ||
        !xESK_Control_Ranges_Disjoint(request, sizeof(*request), control, sizeof(*control)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Logic_Request_t decoded = {0};
    xRETURN_t status = xESK_Logic_Network_Request_Decode(request, &decoded);
    if (status != xRETURN_OK)
    {
        return status;
    }
    status = xESK_Control_Reply_Validate(request, control);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!envelope_valid(control, xESK_CONTROL_RESPONSE) || control->payload.length < xESK_LOGIC_CAPTURE_RESPONSE_BYTES)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    xESK_Logic_Network_Reply_t candidate = {0};
    if (!wire_header_valid(control->payload.data, control->payload.length, xESK_LOGIC_CAPTURE_RESPONSE_BYTES))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    metadata_read(control->payload.data, &candidate.header);
    if (candidate.header.payload_bytes > xESK_LOGIC_NETWORK_DATA_BYTES ||
        control->payload.length != xESK_LOGIC_CAPTURE_RESPONSE_BYTES + candidate.header.payload_bytes)
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    (void)memcpy(candidate.data, control->payload.data + xESK_LOGIC_CAPTURE_RESPONSE_BYTES, candidate.header.payload_bytes);
    if (!reply_matches(&decoded, &candidate))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *reply = candidate;
    return xRETURN_OK;
}

xRETURN_t xESK_Logic_Network_Block_Header_Encode(const xESK_Control_Message_t *request,
                                                 const xESK_Logic_Metadata_t *metadata,
                                                 xESK_Control_Message_t *control)
{
    if (request == NULL || metadata == NULL || control == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!block_objects_disjoint(request, control, metadata))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Logic_Request_t decoded = {0};
    xRETURN_t status = xESK_Logic_Network_Request_Decode(request, &decoded);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!block_header_matches(&decoded, metadata))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Control_Message_t candidate = *request;
    candidate.kind = xESK_CONTROL_RESPONSE;
    (void)memset(&candidate.payload, 0, sizeof(candidate.payload));
    candidate.payload.length = xESK_LOGIC_CAPTURE_RESPONSE_BYTES;
    metadata_write(metadata, candidate.payload.data);
    status = xESK_Control_Reply_Validate(request, &candidate);
    if (status == xRETURN_OK)
    {
        *control = candidate;
    }
    return status;
}

xRETURN_t xESK_Logic_Network_Block_Header_Decode(const xESK_Control_Message_t *request,
                                                 const xESK_Control_Message_t *control,
                                                 xESK_Logic_Metadata_t *metadata)
{
    if (request == NULL || metadata == NULL || control == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if (!block_objects_disjoint(request, control, metadata))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    xESK_Logic_Request_t decoded = {0};
    xRETURN_t status = xESK_Logic_Network_Request_Decode(request, &decoded);
    if (status == xRETURN_OK)
    {
        status = xESK_Control_Reply_Validate(request, control);
    }
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (!envelope_valid(control, xESK_CONTROL_RESPONSE) || control->payload.length != xESK_LOGIC_CAPTURE_RESPONSE_BYTES ||
        !wire_header_valid(control->payload.data, control->payload.length, xESK_LOGIC_CAPTURE_RESPONSE_BYTES))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    xESK_Logic_Metadata_t candidate = {0};
    metadata_read(control->payload.data, &candidate);
    if (!block_header_matches(&decoded, &candidate))
    {
        return xRETURN_xERR_xESK_CORRUPT;
    }
    *metadata = candidate;
    return xRETURN_OK;
}

// EOF /////////////////////////////////////////////////////////////////////////////
