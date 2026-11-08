// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_model.c
// @brief ESK record semantic validation.

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stdbool.h>
// MODULE INCLUDES
#include "xesk.h"

// PRIVATE FUNCTIONS IMPLEMENTATION ///////////////////////////////////////////////
static bool service_id_valid(uint16_t service_id)
{
    return (service_id >= xESK_SERVICE_DEVICE) && (service_id <= xESK_SERVICE_RS485);
}
static bool transports_valid(uint32_t transports)
{
    return (transports != 0U) && ((transports & ~UINT32_C(3)) == 0U);
}
static bool device_valid(const xESK_Device_t *model)
{
    uint8_t identity = 0U;
    for (size_t index = 0U; index < sizeof(model->device_id); ++index)
    {
        identity |= model->device_id[index];
    }
    return (identity != 0U) && (model->boot_id != 0U) && transports_valid(model->transports) && (model->timebase_id != 0U);
}
static bool logic_rate_profile_valid(const xESK_Service_t *model)
{
    return (model->flags & xESK_SERVICE_LOGIC_RATE_PROFILE_1) == 0U ||
           (model->service_id == xESK_SERVICE_LOGIC && model->instance_id == 0U && (model->flags & xESK_SERVICE_FINITE) != 0U &&
            (model->transports & xESK_TRANSPORT_NETWORK) != 0U && model->max_rate_hz == xESK_LOGIC_RATE_PROFILE_1_MAX_HZ &&
            model->buffer_bytes >= xESK_LOGIC_RATE_PROFILE_1_MIN_BYTES);
}
static bool service_valid(const xESK_Service_t *model)
{
    return service_id_valid(model->service_id) && ((model->flags & ~UINT32_C(127)) == 0U) && logic_rate_profile_valid(model) &&
           (((model->flags & xESK_SERVICE_REBOOT) == 0U) || (model->service_id == xESK_SERVICE_FWU && model->instance_id == 0U)) &&
           (((model->flags & xESK_SERVICE_FEATURE_STATUS) == 0U) ||
            (model->service_id == xESK_SERVICE_DEVICE && model->instance_id == 0U)) &&
           transports_valid(model->transports) && (model->max_request_bytes != 0U) && (model->max_reply_bytes != 0U) &&
           (model->timebase_id != 0U) && (model->requirement_count <= xESK_MAX_REQUIREMENTS);
}
static bool sharing_valid(uint16_t sharing)
{
    return (sharing == xESK_SHARING_EXCLUSIVE) || (sharing == xESK_SHARING_COMPATIBLE);
}
static bool resource_valid(const xESK_Resource_t *model)
{
    static const uint16_t units[] = {xESK_UNIT_COUNT,
                                     xESK_UNIT_COUNT,
                                     xESK_UNIT_COUNT,
                                     xESK_UNIT_BYTES,
                                     xESK_UNIT_BYTES_PER_SECOND,
                                     xESK_UNIT_HZ,
                                     xESK_UNIT_TICKS_PER_SECOND,
                                     xESK_UNIT_COUNT};
    if ((model->resource_type < xESK_RESOURCE_PIN) || (model->resource_type > xESK_RESOURCE_ENGINE))
    {
        return false;
    }
    return (model->resource_id != 0U) && (model->unit == units[model->resource_type - 1U]) && sharing_valid(model->sharing) &&
           (model->capacity != 0U) && (model->minimum <= model->maximum);
}
static bool requirement_valid(const xESK_Requirement_t *model)
{
    if ((model->alternative_group == 0U) != (model->alternative_option == 0U))
    {
        return false;
    }
    return service_id_valid(model->service_id) && (model->resource_id != 0U) && sharing_valid(model->sharing) &&
           (model->minimum <= model->maximum) && (model->amount != 0U) && (model->alternative_group <= xESK_MAX_REQUIREMENTS) &&
           (model->alternative_option <= xESK_MAX_REQUIREMENTS);
}
static bool session_lifetime_valid(const xESK_Session_t *model)
{
    if ((model->state < xESK_SESSION_ACTIVE) || (model->state > xESK_SESSION_FAULTED) || ((model->flags & ~UINT32_C(1)) != 0U))
    {
        return false;
    }
    if ((model->state == xESK_SESSION_ACTIVE) && (model->flags != 0U))
    {
        return false;
    }
    switch (model->expiry_policy)
    {
    case xESK_EXPIRY_DEADLINE:
        return model->expires_at_ticks != 0U;
    case xESK_EXPIRY_PROTOCOL_LIFETIME:
        return model->expires_at_ticks == 0U;
    default:
        return false;
    }
}
static bool session_valid(const xESK_Session_t *model)
{
    return (model->boot_id != 0U) && (model->session_id != 0U) && service_id_valid(model->service_id) && (model->timebase_id != 0U) &&
           ((model->owner_kind == xESK_OWNER_SDK) || (model->owner_kind == xESK_OWNER_STANDARD_TOOL)) &&
           ((model->transport == xESK_TRANSPORT_USB) || (model->transport == xESK_TRANSPORT_NETWORK)) && session_lifetime_valid(model);
}
static bool outcome_valid(const xESK_Outcome_t *model)
{
    if ((model->boot_id == 0U) || (model->request_id == 0U) || !service_id_valid(model->service_id) || (model->reason > xESK_REASON_STALE))
    {
        return false;
    }
    switch (model->disposition)
    {
    case xESK_DISPOSITION_REJECTED:
        return (model->reason != xESK_REASON_OK) && (model->completed_units == 0U);
    case xESK_DISPOSITION_COMPLETED:
        return model->session_id != 0U;
    case xESK_DISPOSITION_INDETERMINATE:
        return (model->session_id != 0U) && (model->reason != xESK_REASON_OK);
    default:
        return false;
    }
}
static bool stream_geometry_valid(const xESK_Stream_t *model)
{
    uint16_t channels = 0U;
    for (uint64_t mask = model->channel_mask; mask != 0U; mask >>= 1U)
    {
        channels += (uint16_t)(mask & 1U);
    }
    if ((model->sample_bits == 0U) || (model->sample_bits > 64U) || (channels == 0U) || (channels != model->channel_count))
    {
        return false;
    }
    switch (model->encoding)
    {
    case xESK_ENCODING_RAW8:
        return (model->sample_bits == 8U) && (model->channel_mask <= UINT64_C(255));
    case xESK_ENCODING_PACKED:
        return model->sample_bits == model->channel_count;
    default:
        return false;
    }
}
static bool stream_continuity_valid(const xESK_Stream_t *model)
{
    if (((model->flags & ~UINT16_C(15)) != 0U) || ((model->lost_samples != 0U) && ((model->flags & xESK_STREAM_DISCONTINUITY) == 0U)))
    {
        return false;
    }
    switch (model->flags & (xESK_STREAM_FINITE | xESK_STREAM_CONTINUOUS))
    {
    case xESK_STREAM_FINITE:
        return (model->sample_count != 0U) && (model->offset_samples <= model->sample_count);
    case xESK_STREAM_CONTINUOUS:
        return (model->sample_count == 0U) && ((model->flags & xESK_STREAM_COMPLETE) == 0U);
    default:
        return false;
    }
}
static bool stream_valid(const xESK_Stream_t *model)
{
    return (model->boot_id != 0U) && (model->session_id != 0U) && (model->capture_id != 0U) && (model->stream_id != 0U) &&
           (model->sample_rate_hz != 0U) && (model->timebase_id != 0U) && stream_geometry_valid(model) && stream_continuity_valid(model);
}
static bool record_valid(const xESK_Record_t *record)
{
    switch (record->kind)
    {
    case xESK_RECORD_DEVICE:
        return device_valid(&record->data.device);
    case xESK_RECORD_SERVICE:
        return service_valid(&record->data.service);
    case xESK_RECORD_RESOURCE:
        return resource_valid(&record->data.resource);
    case xESK_RECORD_REQUIREMENT:
        return requirement_valid(&record->data.requirement);
    case xESK_RECORD_SESSION:
        return session_valid(&record->data.session);
    case xESK_RECORD_OUTCOME:
        return outcome_valid(&record->data.outcome);
    case xESK_RECORD_STREAM:
        return stream_valid(&record->data.stream);
    default:
        return false;
    }
}

// EXPORTED FUNCTIONS IMPLEMENTATION /////////////////////////////////////////////////////
xRETURN_t xESK_Record_Validate(const xESK_Record_t *record)
{
    if (record == NULL)
    {
        return xRETURN_xERR_xESK_NULL_POINTER;
    }
    if ((uintptr_t)record > UINTPTR_MAX - sizeof(*record))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    return record_valid(record) ? xRETURN_OK : xRETURN_xERR_xESK_INVALID_ARG;
}

// EOF /////////////////////////////////////////////////////////////////////////////
