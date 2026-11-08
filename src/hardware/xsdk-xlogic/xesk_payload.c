// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_payload.c
// @brief Explicit little-endian ESK payload conversion.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// MODULE INCLUDES
#include "xesk_internal.h"
#include "xbytes.h"

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static void encode_device(const xESK_Device_t *model, uint8_t *bytes)
{
    for (size_t index = 0U; index < 16U; ++index)
    {
        bytes[index] = model->device_id[index];
    }
    xWrite_LE64(bytes + 16U, model->boot_id);
    xWrite_LE32(bytes + 24U, model->firmware_version);
    xWrite_LE32(bytes + 28U, model->hardware_id);
    xWrite_LE32(bytes + 32U, model->transports);
    xWrite_LE32(bytes + 36U, model->timebase_id);
}

static void decode_device(const uint8_t *bytes, xESK_Device_t *model)
{
    for (size_t index = 0U; index < 16U; ++index)
    {
        model->device_id[index] = bytes[index];
    }
    model->boot_id = xRead_LE64(bytes + 16U);
    model->firmware_version = xRead_LE32(bytes + 24U);
    model->hardware_id = xRead_LE32(bytes + 28U);
    model->transports = xRead_LE32(bytes + 32U);
    model->timebase_id = xRead_LE32(bytes + 36U);
}

static void encode_service(const xESK_Service_t *model, uint8_t *bytes)
{
    xWrite_LE16(bytes + 0U, model->service_id);
    xWrite_LE16(bytes + 2U, model->instance_id);
    xWrite_LE32(bytes + 4U, model->flags);
    xWrite_LE32(bytes + 8U, model->transports);
    xWrite_LE32(bytes + 12U, model->max_request_bytes);
    xWrite_LE32(bytes + 16U, model->max_reply_bytes);
    xWrite_LE64(bytes + 20U, model->max_rate_hz);
    xWrite_LE64(bytes + 28U, model->max_bandwidth_bytes_s);
    xWrite_LE64(bytes + 36U, model->buffer_bytes);
    xWrite_LE32(bytes + 44U, model->timebase_id);
    xWrite_LE16(bytes + 48U, model->requirement_count);
    xWrite_LE16(bytes + 50U, 0U);
}

static void decode_service(const uint8_t *bytes, xESK_Service_t *model)
{
    model->service_id = xRead_LE16(bytes + 0U);
    model->instance_id = xRead_LE16(bytes + 2U);
    model->flags = xRead_LE32(bytes + 4U);
    model->transports = xRead_LE32(bytes + 8U);
    model->max_request_bytes = xRead_LE32(bytes + 12U);
    model->max_reply_bytes = xRead_LE32(bytes + 16U);
    model->max_rate_hz = xRead_LE64(bytes + 20U);
    model->max_bandwidth_bytes_s = xRead_LE64(bytes + 28U);
    model->buffer_bytes = xRead_LE64(bytes + 36U);
    model->timebase_id = xRead_LE32(bytes + 44U);
    model->requirement_count = xRead_LE16(bytes + 48U);
}

static void encode_resource(const xESK_Resource_t *model, uint8_t *bytes)
{
    xWrite_LE32(bytes + 0U, model->resource_id);
    xWrite_LE16(bytes + 4U, model->resource_type);
    xWrite_LE16(bytes + 6U, model->unit);
    xWrite_LE16(bytes + 8U, model->sharing);
    xWrite_LE16(bytes + 10U, 0U);
    xWrite_LE64(bytes + 12U, model->capacity);
    xWrite_LE64(bytes + 20U, model->minimum);
    xWrite_LE64(bytes + 28U, model->maximum);
}

static void decode_resource(const uint8_t *bytes, xESK_Resource_t *model)
{
    model->resource_id = xRead_LE32(bytes + 0U);
    model->resource_type = xRead_LE16(bytes + 4U);
    model->unit = xRead_LE16(bytes + 6U);
    model->sharing = xRead_LE16(bytes + 8U);
    model->capacity = xRead_LE64(bytes + 12U);
    model->minimum = xRead_LE64(bytes + 20U);
    model->maximum = xRead_LE64(bytes + 28U);
}

static void encode_requirement(const xESK_Requirement_t *model, uint8_t *bytes)
{
    xWrite_LE16(bytes + 0U, model->service_id);
    xWrite_LE16(bytes + 2U, model->instance_id);
    xWrite_LE16(bytes + 4U, model->alternative_group);
    xWrite_LE16(bytes + 6U, model->alternative_option);
    xWrite_LE32(bytes + 8U, model->resource_id);
    xWrite_LE16(bytes + 12U, model->sharing);
    xWrite_LE16(bytes + 14U, 0U);
    xWrite_LE64(bytes + 16U, model->minimum);
    xWrite_LE64(bytes + 24U, model->maximum);
    xWrite_LE64(bytes + 32U, model->amount);
}

static void decode_requirement(const uint8_t *bytes, xESK_Requirement_t *model)
{
    model->service_id = xRead_LE16(bytes + 0U);
    model->instance_id = xRead_LE16(bytes + 2U);
    model->alternative_group = xRead_LE16(bytes + 4U);
    model->alternative_option = xRead_LE16(bytes + 6U);
    model->resource_id = xRead_LE32(bytes + 8U);
    model->sharing = xRead_LE16(bytes + 12U);
    model->minimum = xRead_LE64(bytes + 16U);
    model->maximum = xRead_LE64(bytes + 24U);
    model->amount = xRead_LE64(bytes + 32U);
}

static void encode_session(const xESK_Session_t *model, uint8_t *bytes)
{
    xWrite_LE64(bytes + 0U, model->boot_id);
    xWrite_LE64(bytes + 8U, model->session_id);
    xWrite_LE64(bytes + 16U, model->expires_at_ticks);
    xWrite_LE16(bytes + 24U, model->service_id);
    xWrite_LE16(bytes + 26U, model->instance_id);
    xWrite_LE32(bytes + 28U, model->timebase_id);
    bytes[32U] = model->owner_kind;
    bytes[33U] = model->transport;
    bytes[34U] = model->state;
    bytes[35U] = model->expiry_policy;
    xWrite_LE32(bytes + 36U, model->flags);
}

static void decode_session(const uint8_t *bytes, xESK_Session_t *model)
{
    model->boot_id = xRead_LE64(bytes + 0U);
    model->session_id = xRead_LE64(bytes + 8U);
    model->expires_at_ticks = xRead_LE64(bytes + 16U);
    model->service_id = xRead_LE16(bytes + 24U);
    model->instance_id = xRead_LE16(bytes + 26U);
    model->timebase_id = xRead_LE32(bytes + 28U);
    model->owner_kind = bytes[32U];
    model->transport = bytes[33U];
    model->state = bytes[34U];
    model->expiry_policy = bytes[35U];
    model->flags = xRead_LE32(bytes + 36U);
}

static void encode_outcome(const xESK_Outcome_t *model, uint8_t *bytes)
{
    xWrite_LE64(bytes + 0U, model->boot_id);
    xWrite_LE64(bytes + 8U, model->session_id);
    xWrite_LE64(bytes + 16U, model->request_id);
    xWrite_LE16(bytes + 24U, model->service_id);
    xWrite_LE16(bytes + 26U, model->instance_id);
    bytes[28U] = model->disposition;
    bytes[29U] = model->reason;
    xWrite_LE16(bytes + 30U, 0U);
    xWrite_LE64(bytes + 32U, model->completed_units);
}

static void decode_outcome(const uint8_t *bytes, xESK_Outcome_t *model)
{
    model->boot_id = xRead_LE64(bytes + 0U);
    model->session_id = xRead_LE64(bytes + 8U);
    model->request_id = xRead_LE64(bytes + 16U);
    model->service_id = xRead_LE16(bytes + 24U);
    model->instance_id = xRead_LE16(bytes + 26U);
    model->disposition = bytes[28U];
    model->reason = bytes[29U];
    model->completed_units = xRead_LE64(bytes + 32U);
}

static void encode_stream(const xESK_Stream_t *model, uint8_t *bytes)
{
    xWrite_LE64(bytes + 0U, model->boot_id);
    xWrite_LE64(bytes + 8U, model->session_id);
    xWrite_LE64(bytes + 16U, model->capture_id);
    xWrite_LE64(bytes + 24U, model->stream_id);
    xWrite_LE64(bytes + 32U, model->sample_count);
    xWrite_LE64(bytes + 40U, model->sample_rate_hz);
    xWrite_LE64(bytes + 48U, model->timestamp_ticks);
    xWrite_LE32(bytes + 56U, model->timebase_id);
    xWrite_LE16(bytes + 60U, model->encoding);
    xWrite_LE16(bytes + 62U, model->flags);
    xWrite_LE16(bytes + 64U, model->sample_bits);
    xWrite_LE16(bytes + 66U, model->channel_count);
    xWrite_LE64(bytes + 68U, model->chunk_sequence);
    xWrite_LE64(bytes + 76U, model->offset_samples);
    xWrite_LE64(bytes + 84U, model->lost_samples);
    xWrite_LE64(bytes + 92U, model->channel_mask);
}

static void decode_stream(const uint8_t *bytes, xESK_Stream_t *model)
{
    model->boot_id = xRead_LE64(bytes + 0U);
    model->session_id = xRead_LE64(bytes + 8U);
    model->capture_id = xRead_LE64(bytes + 16U);
    model->stream_id = xRead_LE64(bytes + 24U);
    model->sample_count = xRead_LE64(bytes + 32U);
    model->sample_rate_hz = xRead_LE64(bytes + 40U);
    model->timestamp_ticks = xRead_LE64(bytes + 48U);
    model->timebase_id = xRead_LE32(bytes + 56U);
    model->encoding = xRead_LE16(bytes + 60U);
    model->flags = xRead_LE16(bytes + 62U);
    model->sample_bits = xRead_LE16(bytes + 64U);
    model->channel_count = xRead_LE16(bytes + 66U);
    model->chunk_sequence = xRead_LE64(bytes + 68U);
    model->offset_samples = xRead_LE64(bytes + 76U);
    model->lost_samples = xRead_LE64(bytes + 84U);
    model->channel_mask = xRead_LE64(bytes + 92U);
}

// EXPORTED FUNCTIONS IMPLEMENTATION /////////////////////////////////////////////////////
size_t xESK_Payload_Size(uint8_t kind)
{
    switch (kind)
    {
    case xESK_RECORD_DEVICE:
    case xESK_RECORD_REQUIREMENT:
    case xESK_RECORD_SESSION:
    case xESK_RECORD_OUTCOME:
        return 40U;
    case xESK_RECORD_SERVICE:
        return 52U;
    case xESK_RECORD_RESOURCE:
        return 36U;
    case xESK_RECORD_STREAM:
        return 100U;
    default:
        return 0U;
    }
}

bool xESK_Payload_Reserved_Valid(uint8_t kind, const uint8_t *payload)
{
    switch (kind)
    {
    case xESK_RECORD_SERVICE:
        return xRead_LE16(payload + 50U) == 0U;
    case xESK_RECORD_RESOURCE:
        return xRead_LE16(payload + 10U) == 0U;
    case xESK_RECORD_REQUIREMENT:
        return xRead_LE16(payload + 14U) == 0U;
    case xESK_RECORD_OUTCOME:
        return xRead_LE16(payload + 30U) == 0U;
    default:
        return true;
    }
}

void xESK_Payload_Encode(const xESK_Record_t *record, uint8_t *payload)
{
    switch (record->kind)
    {
    case xESK_RECORD_DEVICE:
        encode_device(&record->data.device, payload);
        break;
    case xESK_RECORD_SERVICE:
        encode_service(&record->data.service, payload);
        break;
    case xESK_RECORD_RESOURCE:
        encode_resource(&record->data.resource, payload);
        break;
    case xESK_RECORD_REQUIREMENT:
        encode_requirement(&record->data.requirement, payload);
        break;
    case xESK_RECORD_SESSION:
        encode_session(&record->data.session, payload);
        break;
    case xESK_RECORD_OUTCOME:
        encode_outcome(&record->data.outcome, payload);
        break;
    case xESK_RECORD_STREAM:
        encode_stream(&record->data.stream, payload);
        break;
    default:
        break;
    }
}

void xESK_Payload_Decode(const uint8_t *payload, xESK_Record_t *record)
{
    switch (record->kind)
    {
    case xESK_RECORD_DEVICE:
        decode_device(payload, &record->data.device);
        break;
    case xESK_RECORD_SERVICE:
        decode_service(payload, &record->data.service);
        break;
    case xESK_RECORD_RESOURCE:
        decode_resource(payload, &record->data.resource);
        break;
    case xESK_RECORD_REQUIREMENT:
        decode_requirement(payload, &record->data.requirement);
        break;
    case xESK_RECORD_SESSION:
        decode_session(payload, &record->data.session);
        break;
    case xESK_RECORD_OUTCOME:
        decode_outcome(payload, &record->data.outcome);
        break;
    case xESK_RECORD_STREAM:
        decode_stream(payload, &record->data.stream);
        break;
    default:
        break;
    }
}

// EOF /////////////////////////////////////////////////////////////////////////////
