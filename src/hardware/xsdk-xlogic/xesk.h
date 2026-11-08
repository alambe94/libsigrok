// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk.h
// @brief Portable ESK descriptor records and codec.

#ifndef xESK_H
#define xESK_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stddef.h>
#include <stdint.h>
// MODULE INCLUDES
#include "xreturn.h"

#ifdef __cplusplus
extern "C"
{
#endif

// PUBLIC API /////////////////////////////////////////////////////////
// MACROS /////////////////////////////////////////////////////////////////////////
#define xESK_WIRE_VERSION                   1U
#define xESK_HEADER_BYTES                   12U
#define xESK_MAX_FRAME_BYTES                128U
#define xESK_MAX_REQUIREMENTS               32U
#define xESK_LOGIC_RATE_PROFILE_1_MAX_HZ    500000000U
#define xESK_LOGIC_RATE_PROFILE_1_MIN_BYTES 192U
// Observation-only identity; this does not advertise a QSPI control service.
#define xESK_FEATURE_QSPI 16U

    // TYPES //////////////////////////////////////////////////////////////////////////
    typedef enum
    {
        xRETURN_xERR_xESK_NULL_POINTER = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 1U),
        xRETURN_xERR_xESK_INVALID_ARG = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 2U),
        xRETURN_xERR_xESK_UNSUPPORTED = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 3U),
        xRETURN_xERR_xESK_CORRUPT = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 4U),
        xRETURN_xERR_xESK_OVERFLOW = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 5U),
        xRETURN_xERR_xESK_BUSY = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 6U),
        xRETURN_xERR_xESK_INVALID_STATE = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 7U),
        xRETURN_xERR_xESK_NOT_FOUND = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 8U),
        xRETURN_xERR_xESK_SEARCH_LIMIT = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 9U),
        xRETURN_xERR_xESK_INVALID_HANDLE = xRETURN_MAKE(xRETURN_xESK_MODULE, xRETURN_SEVERITY_ERROR, 10U),
    } xESK_Return_t;

    typedef enum
    {
        xESK_RECORD_DEVICE = 1,
        xESK_RECORD_SERVICE = 2,
        xESK_RECORD_RESOURCE = 3,
        xESK_RECORD_REQUIREMENT = 4,
        xESK_RECORD_SESSION = 5,
        xESK_RECORD_OUTCOME = 6,
        xESK_RECORD_STREAM = 7,
    } xESK_Kind_t;

    typedef enum
    {
        xESK_TRANSPORT_USB = 1,
        xESK_TRANSPORT_NETWORK = 2,
    } xESK_Transport_t;

    typedef enum
    {
        xESK_SERVICE_DEVICE = 1,
        xESK_SERVICE_LOGIC = 2,
        xESK_SERVICE_DEBUG = 3,
        xESK_SERVICE_UART = 4,
        xESK_SERVICE_I2C = 5,
        xESK_SERVICE_SPI = 6,
        xESK_SERVICE_GPIO = 7,
        xESK_SERVICE_USB_ANALYZER = 8,
        xESK_SERVICE_BLE_ANALYZER = 9,
        xESK_SERVICE_POWER_ANALYZER = 10,
        xESK_SERVICE_FWU = 11,
        xESK_SERVICE_PWM = 12,
        xESK_SERVICE_CAN = 13,
        xESK_SERVICE_LIN = 14,
        xESK_SERVICE_RS485 = 15,
    } xESK_Service_Id_t;

    typedef enum
    {
        xESK_SERVICE_FINITE = 1,
        xESK_SERVICE_CONTINUOUS = 2,
        xESK_SERVICE_DRIVES_TARGET = 4,
        xESK_SERVICE_STANDARD_PROTOCOL = 8,
        // DEVICE/0 endpoint projection only; a registered reader supplies opcode8.
        xESK_SERVICE_FEATURE_STATUS = 16,
        // FWU/0 only: distinct versioned reboot preparation is enabled.
        xESK_SERVICE_REBOOT = 32,
        // LOGIC/0 finite network rate set defined by the xLOGIC network codec.
        xESK_SERVICE_LOGIC_RATE_PROFILE_1 = 64,
    } xESK_Service_Flag_t;

    typedef enum
    {
        xESK_FEATURE_IMPLEMENTED = 1,
        xESK_FEATURE_ENABLED = 2,
        xESK_FEATURE_OBSERVABLE = 4,
    } xESK_Feature_Capability_t;
    typedef enum
    {
        xESK_FEATURE_UNKNOWN = 0,
        xESK_FEATURE_UNCLAIMED = 1,
        xESK_FEATURE_PENDING = 2,
        xESK_FEATURE_CLAIMED = 3,
        xESK_FEATURE_RETIRING = 4,
        xESK_FEATURE_QUARANTINED = 5,
        xESK_FEATURE_FAULTED = 6,
    } xESK_Feature_Ownership_t;
    typedef enum
    {
        xESK_FEATURE_TRANSPORT_NONE = 0,
        xESK_FEATURE_TRANSPORT_USB = 1,
        xESK_FEATURE_TRANSPORT_NETWORK = 2,
        xESK_FEATURE_TRANSPORT_INTERNAL = 3,
        xESK_FEATURE_TRANSPORT_UNKNOWN = 255,
    } xESK_Feature_Transport_t;
    typedef enum
    {
        xESK_FEATURE_MAINTENANCE = 1,
        // Retained foreign resource overlap, not allocation feasibility.
        xESK_FEATURE_SHARED_RESOURCE_HELD = 2,
        xESK_FEATURE_SESSION_CAPACITY = 4,
        xESK_FEATURE_STOPPING = 8,
        xESK_FEATURE_OWNER_FAULT = 16,
        xESK_FEATURE_UNOBSERVABLE = 32,
    } xESK_Feature_Restriction_t;
    // Sanitized values only. IDs1..15 use Service_Id values; DEBUG instances0/1
    // identify frontend lanes, not the aggregate reservation's control identity.
    typedef struct
    {
        uint16_t feature_id;
        uint8_t instance_id;
        uint8_t capability_flags;
        uint8_t ownership;
        uint8_t transport;
        uint16_t restrictions;
    } xESK_Feature_Observation_t;

    typedef enum
    {
        xESK_RESOURCE_PIN = 1,
        xESK_RESOURCE_PRU = 2,
        xESK_RESOURCE_DMA = 3,
        xESK_RESOURCE_BUFFER = 4,
        xESK_RESOURCE_BANDWIDTH = 5,
        xESK_RESOURCE_CLOCK = 6,
        xESK_RESOURCE_TIMEBASE = 7,
        xESK_RESOURCE_ENGINE = 8,
    } xESK_Resource_Type_t;

    typedef enum
    {
        xESK_UNIT_COUNT = 1,
        xESK_UNIT_BYTES = 2,
        xESK_UNIT_BYTES_PER_SECOND = 3,
        xESK_UNIT_HZ = 4,
        xESK_UNIT_TICKS_PER_SECOND = 5,
    } xESK_Unit_t;

    typedef enum
    {
        xESK_SHARING_EXCLUSIVE = 1,
        xESK_SHARING_COMPATIBLE = 2,
    } xESK_Sharing_t;

    typedef enum
    {
        xESK_OWNER_SDK = 1,
        xESK_OWNER_STANDARD_TOOL = 2,
    } xESK_Owner_t;

    typedef enum
    {
        xESK_SESSION_ACTIVE = 1,
        xESK_SESSION_EXPIRED = 2,
        xESK_SESSION_RELEASED = 3,
        xESK_SESSION_FAULTED = 4,
    } xESK_Session_State_t;

    typedef enum
    {
        xESK_EXPIRY_DEADLINE = 1,
        xESK_EXPIRY_PROTOCOL_LIFETIME = 2,
    } xESK_Expiry_Policy_t;

    typedef enum
    {
        xESK_SESSION_SAFE_STATE_CONFIRMED = 1,
    } xESK_Session_Flag_t;

    typedef enum
    {
        xESK_DISPOSITION_REJECTED = 1,
        xESK_DISPOSITION_COMPLETED = 2,
        xESK_DISPOSITION_INDETERMINATE = 3,
    } xESK_Disposition_t;

    typedef enum
    {
        xESK_REASON_OK = 0,
        xESK_REASON_INVALID = 1,
        xESK_REASON_BUSY = 2,
        xESK_REASON_UNSUPPORTED = 3,
        xESK_REASON_TIMEOUT = 4,
        xESK_REASON_DISCONNECTED = 5,
        xESK_REASON_FAULT = 6,
        xESK_REASON_ABORTED = 7,
        xESK_REASON_STALE = 8,
    } xESK_Reason_t;

    typedef enum
    {
        xESK_ENCODING_RAW8 = 1,
        xESK_ENCODING_PACKED = 2,
    } xESK_Encoding_t;

    typedef enum
    {
        xESK_STREAM_FINITE = 1,
        xESK_STREAM_CONTINUOUS = 2,
        xESK_STREAM_COMPLETE = 4,
        xESK_STREAM_DISCONTINUITY = 8,
    } xESK_Stream_Flag_t;

    typedef struct
    {
        uint8_t device_id[16];
        uint64_t boot_id;
        uint32_t firmware_version;
        uint32_t hardware_id;
        uint32_t transports;
        uint32_t timebase_id;
    } xESK_Device_t;

    typedef struct
    {
        uint16_t service_id;
        uint16_t instance_id;
        uint32_t flags;
        uint32_t transports;
        uint32_t max_request_bytes;
        uint32_t max_reply_bytes;
        uint64_t max_rate_hz;
        uint64_t max_bandwidth_bytes_s;
        uint64_t buffer_bytes;
        uint32_t timebase_id;
        uint16_t requirement_count;
    } xESK_Service_t;

    typedef struct
    {
        uint32_t resource_id;
        uint16_t resource_type;
        uint16_t unit;
        uint16_t sharing;
        uint64_t capacity;
        uint64_t minimum;
        uint64_t maximum;
    } xESK_Resource_t;

    typedef struct
    {
        uint16_t service_id;
        uint16_t instance_id;
        uint16_t alternative_group;
        uint16_t alternative_option;
        uint32_t resource_id;
        uint16_t sharing;
        uint64_t minimum;
        uint64_t maximum;
        uint64_t amount;
    } xESK_Requirement_t;

    typedef struct
    {
        uint64_t boot_id;
        uint64_t session_id;
        uint64_t expires_at_ticks;
        uint16_t service_id;
        uint16_t instance_id;
        uint32_t timebase_id;
        uint8_t owner_kind;
        uint8_t transport;
        uint8_t state;
        uint8_t expiry_policy;
        uint32_t flags;
    } xESK_Session_t;

    typedef struct
    {
        uint64_t boot_id;
        uint64_t session_id;
        uint64_t request_id;
        uint16_t service_id;
        uint16_t instance_id;
        uint8_t disposition;
        uint8_t reason;
        uint64_t completed_units;
    } xESK_Outcome_t;

    typedef struct
    {
        uint64_t boot_id;
        uint64_t session_id;
        uint64_t capture_id;
        uint64_t stream_id;
        uint64_t sample_count;
        uint64_t sample_rate_hz;
        uint64_t timestamp_ticks;
        uint32_t timebase_id;
        uint16_t encoding;
        uint16_t flags;
        uint16_t sample_bits;
        uint16_t channel_count;
        uint64_t chunk_sequence;
        uint64_t offset_samples;
        uint64_t lost_samples;
        uint64_t channel_mask;
    } xESK_Stream_t;

    typedef struct
    {
        uint8_t kind;
        union
        {
            xESK_Device_t device;
            xESK_Service_t service;
            xESK_Resource_t resource;
            xESK_Requirement_t requirement;
            xESK_Session_t session;
            xESK_Outcome_t outcome;
            xESK_Stream_t stream;
        } data;
    } xESK_Record_t;

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    // Models are call-scoped value objects; no pointers or storage are retained.
    // See docs/wire.md for field units, enum rules and cross-record obligations.
    xRETURN_t xESK_Record_Validate(const xESK_Record_t *record);
    // On any failure, all outputs remain unchanged. All object ranges must be
    // disjoint; encode checks the full buffer capacity, including unused bytes.
    xRETURN_t xESK_Record_Encode(const xESK_Record_t *record, uint8_t *buffer, size_t capacity, size_t *written);
    xRETURN_t xESK_Record_Decode(const uint8_t *buffer, size_t length, xESK_Record_t *record);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_H

// EOF /////////////////////////////////////////////////////////////////////////////
