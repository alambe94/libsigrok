// Copyright 2026 alambe94
// Licensed under the Apache License, Version 2.0.

#ifndef xLOGIC_WINUSB_PROTOCOL_H
#define xLOGIC_WINUSB_PROTOCOL_H

#include <stdint.h>

#define xLOGIC_WINUSB_MAGIC                               0x584C4F47UL
#define xLOGIC_WINUSB_ABI_VERSION                         3U
#define xLOGIC_WINUSB_REQUEST_GET_CAPS                    0x60U
#define xLOGIC_WINUSB_REQUEST_CONFIGURE                   0x61U
#define xLOGIC_WINUSB_REQUEST_START                       0x62U
#define xLOGIC_WINUSB_REQUEST_STOP                        0x63U
#define xLOGIC_WINUSB_REQUEST_GET_STATUS                  0x64U
#define xLOGIC_WINUSB_SAMPLE_ORDER_CHRONOLOGICAL          0U
#define xLOGIC_WINUSB_CAP_RATE_DIAGNOSTICS                (1UL << 0U)
#define xLOGIC_WINUSB_CAP_UNIFORM_RATE_CERTIFIED          (1UL << 1U)
#define xLOGIC_WINUSB_CAP_CONTINUOUS_TANDEM               (1UL << 2U)
#define xLOGIC_WINUSB_CAP_PACKED_NIBBLE_SELECT            (1UL << 4U)
#define xLOGIC_WINUSB_CAP_PACKED_4CH_CONTIGUOUS           (1UL << 7U)
#define xLOGIC_WINUSB_CAP_SCOPED_CONTROL                  (1UL << 8U)
#define xLOGIC_WINUSB_CAP_RAW_FULL_CLOCK_ONLY             (1UL << 9U)
#define xLOGIC_WINUSB_CAP_GRACEFUL_FINISH                 (1UL << 10U)
#define xLOGIC_WINUSB_STATUS_PHYSICAL_QUIESCENT           (1UL << 2U)
#define xLOGIC_WINUSB_STATUS_START_PENDING                (1UL << 3U)
#define xLOGIC_WINUSB_STATUS_STOP_PENDING                 (1UL << 4U)
#define xLOGIC_WINUSB_STATUS_SESSION_FAULT                (1UL << 5U)
#define xLOGIC_WINUSB_STATUS_STREAM_FINISHED              (1UL << 6U)
#define xLOGIC_WINUSB_STATUS_ACTUAL_RATE_VALID            (1UL << 0U)
#define xLOGIC_WINUSB_STATUS_UNIFORM_TIMING               (1UL << 1U)
#define xLOGIC_WINUSB_STREAM_FORMAT_U8                    0U
#define xLOGIC_WINUSB_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS 3U
#define xLOGIC_WINUSB_PACKED_LOWER_CHANNEL_MASK           0x0FU
#define xLOGIC_WINUSB_PACKED_UPPER_CHANNEL_MASK           0xF0U
#define xLOGIC_WINUSB_CONTIGUOUS_SAMPLE_RATE              500000000U
#define xLOGIC_WINUSB_CONTIGUOUS_QUANTUM_BYTES            32U
#define xLOGIC_WINUSB_CONTIGUOUS_SAMPLES_PER_QUANTUM      64U
#define xLOGIC_WINUSB_CONTIGUOUS_PAYLOAD_BYTES            32U

// Retired formats 1 and 2 and capability bits 3, 5 and 6 are not supported.
// PACKED_4CH_CONTIGUOUS carries one input slice with the earlier sample in the low
// nibble and the next sample in the high nibble. All 32 bytes carry 64 samples;
// there is no padding or opposite-edge input-bank interleaving. It supports continuous,
// immediate capture only. Advertising the format does not certify timing.
// ABI v3 packed_* capability fields describe one advertised packed format;
// Only PACKED_4CH_CONTIGUOUS is supported as a packed format.

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define xLOGIC_WINUSB_PACKED
#else
#define xLOGIC_WINUSB_PACKED __attribute__((packed))
#endif

typedef struct xLOGIC_WINUSB_PACKED
{
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint32_t sample_order;
    uint32_t stream_buffer_bytes;
    uint32_t stream_buffer_count;
    uint32_t max_capture_bytes; // Zero: continuous capture only; finite host limits may stop the stream.
    uint32_t max_sample_rate;
    uint32_t max_channels;
    uint32_t feature_flags;
    uint32_t max_uniform_sample_rate;
    uint32_t packed_sample_rate;
    uint32_t packed_channel_mask;
    uint32_t packed_quantum_bytes;
    uint32_t packed_samples_per_quantum;
} xLOGIC_WINUSB_Caps_t;

// RAW_FULL_CLOCK_ONLY admits U8 at max_sample_rate with all eight channels.
// Timing certification is advertised separately. Other profiles retain their rates.

typedef struct xLOGIC_WINUSB_PACKED
{
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint32_t sample_rate;
    uint32_t sample_count;
    uint32_t channel_mask;
    uint32_t trigger_mode;
    uint32_t trigger_mask;
    uint32_t trigger_value;
    uint32_t trigger_channel;
    uint32_t trigger_edge;
    uint32_t stream_format;
} xLOGIC_WINUSB_Config_t;

// Progress and errors describe one acquisition, reset by an accepted idle
// START. Rejected START and STOP preserve the result for inspection. Pending
// backend STOP, outstanding transfers and owned buffers prohibit a new START.
typedef struct xLOGIC_WINUSB_PACKED
{
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint32_t configured;
    uint32_t running;
    uint64_t produced_bytes;
    uint64_t transmitted_bytes;
    uint32_t overruns;
    uint32_t usb_errors;
    uint32_t ownership_errors;
    uint32_t requested_sample_rate;
    uint32_t actual_sample_rate;
    uint32_t timing_errors;
    uint32_t status_flags;
    uint32_t stream_format;
} xLOGIC_WINUSB_Status_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(__cplusplus)
static_assert(sizeof(xLOGIC_WINUSB_Caps_t) == 56U, "xLOGIC WinUSB caps ABI changed");
static_assert(sizeof(xLOGIC_WINUSB_Config_t) == 44U, "xLOGIC WinUSB config ABI changed");
static_assert(sizeof(xLOGIC_WINUSB_Status_t) == 64U, "xLOGIC WinUSB status ABI changed");
#else
_Static_assert(sizeof(xLOGIC_WINUSB_Caps_t) == 56U, "xLOGIC WinUSB caps ABI changed");
_Static_assert(sizeof(xLOGIC_WINUSB_Config_t) == 44U, "xLOGIC WinUSB config ABI changed");
_Static_assert(sizeof(xLOGIC_WINUSB_Status_t) == 64U, "xLOGIC WinUSB status ABI changed");
#endif

#endif
