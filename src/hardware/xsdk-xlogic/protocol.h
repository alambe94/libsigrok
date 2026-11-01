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

#ifndef LIBSIGROK_HARDWARE_xSDK_xLOGIC_PROTOCOL_H
#define LIBSIGROK_HARDWARE_xSDK_xLOGIC_PROTOCOL_H

#include <glib.h>
#include <libsigrok/libsigrok.h>
#include <libusb.h>
#include <stddef.h>
#include <stdint.h>
#include "libsigrok-internal.h"
#include "xlogic_xesk_host.h"

#define LOG_PREFIX                                      "xsdk-xlogic"
#define xSDK_xLOGIC_VID                                 0x1209U
#define xSDK_xLOGIC_PID                                 0x0003U
#define xSDK_xLOGIC_INTERFACE                           0U
#define xSDK_xLOGIC_EP_IN                               0x81U
#define xSDK_xLOGIC_MAGIC                               0x584c4f47UL
#define xSDK_xLOGIC_ABI_VERSION                         3U
#define xSDK_xLOGIC_GET_CAPS                            0x60U
#define xSDK_xLOGIC_CONFIGURE                           0x61U
#define xSDK_xLOGIC_START                               0x62U
#define xSDK_xLOGIC_STOP                                0x63U
#define xSDK_xLOGIC_GET_STATUS                          0x64U
#define xSDK_xLOGIC_SAMPLE_CHRONOLOGICAL                0U
#define xSDK_xLOGIC_CAP_RATE_DIAGNOSTICS                (1UL << 0U)
#define xSDK_xLOGIC_CAP_UNIFORM_CERTIFIED               (1UL << 1U)
#define xSDK_xLOGIC_CAP_CONTINUOUS_TANDEM               (1UL << 2U)
#define xSDK_xLOGIC_CAP_PACKED_NIBBLE_SELECT            (1UL << 4U)
#define xSDK_xLOGIC_CAP_PACKED_4CH_CONTIGUOUS           (1UL << 7U)
#define xSDK_xLOGIC_CAP_SCOPED_CONTROL                  (1UL << 8U)
#define xSDK_xLOGIC_CAP_RAW_FULL_CLOCK_ONLY             (1UL << 9U)
#define xSDK_xLOGIC_CAP_GRACEFUL_FINISH                 (1UL << 10U)
#define xSDK_xLOGIC_STATUS_ACTUAL_RATE_VALID            (1UL << 0U)
#define xSDK_xLOGIC_STATUS_UNIFORM_TIMING               (1UL << 1U)
#define xSDK_xLOGIC_STATUS_PHYSICAL_QUIESCENT           (1UL << 2U)
#define xSDK_xLOGIC_STATUS_START_PENDING                (1UL << 3U)
#define xSDK_xLOGIC_STATUS_STOP_PENDING                 (1UL << 4U)
#define xSDK_xLOGIC_STATUS_SESSION_FAULT                (1UL << 5U)
#define xSDK_xLOGIC_STATUS_STREAM_FINISHED              (1UL << 6U)
#define xSDK_xLOGIC_STREAM_FORMAT_U8                    0U
#define xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS 3U
#define xSDK_xLOGIC_PACKED_LOWER_CHANNEL_MASK           0x0fU
#define xSDK_xLOGIC_PACKED_UPPER_CHANNEL_MASK           0xf0U
#define xSDK_xLOGIC_CONTIGUOUS_SAMPLE_RATE              500000000U
#define xSDK_xLOGIC_CONTIGUOUS_QUANTUM_BYTES            32U
#define xSDK_xLOGIC_CONTIGUOUS_SAMPLES_PER_QUANTUM      64U
#define xSDK_xLOGIC_CONTIGUOUS_PAYLOAD_BYTES            32U
#define xSDK_xLOGIC_TRANSFER_COUNT                      16U
#define xSDK_xLOGIC_DIRECT_TRANSFER_BYTES               (2U * 1024U * 1024U)
#define xSDK_xLOGIC_CONTROL_TIMEOUT_MS                  1000U
#define xSDK_xLOGIC_TRANSFER_TIMEOUT_MS                 5000U
#define xSDK_xLOGIC_DEFAULT_SAMPLES                     (1024U * 1024U)
#define xSDK_xLOGIC_MAX_LOGICAL_SAMPLES                 1000000000000ULL
#define xSDK_xLOGIC_RATE_COUNT                          5U
#define xSDK_xLOGIC_MIN_CAPTURE_PERIOD                  1U
#define xSDK_xLOGIC_MAX_CAPTURE_PERIOD                  8U
#define xSDK_xLOGIC_PERIOD1_SAMPLE_GRANULARITY          64U

struct xsdk_xlogic_caps_wire
{
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint32_t sample_order;
    uint32_t stream_buffer_bytes;
    uint32_t stream_buffer_count;
    uint32_t max_capture_bytes;
    uint32_t max_sample_rate;
    uint32_t max_channels;
    uint32_t feature_flags;
    uint32_t max_uniform_sample_rate;
    uint32_t packed_sample_rate;
    uint32_t packed_channel_mask;
    uint32_t packed_quantum_bytes;
    uint32_t packed_samples_per_quantum;
} __attribute__((packed));

struct xsdk_xlogic_config_wire
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
} __attribute__((packed));

struct xsdk_xlogic_status_wire
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
} __attribute__((packed));

G_STATIC_ASSERT(sizeof(struct xsdk_xlogic_caps_wire) == 56U);
G_STATIC_ASSERT(sizeof(struct xsdk_xlogic_config_wire) == 44U);
G_STATIC_ASSERT(sizeof(struct xsdk_xlogic_status_wire) == 64U);

struct xsdk_xlogic_caps
{
    uint32_t stream_buffer_bytes;
    uint32_t stream_buffer_count;
    uint32_t max_capture_bytes;
    uint32_t max_sample_rate;
    uint32_t max_channels;
    uint32_t feature_flags;
    uint32_t max_uniform_sample_rate;
    uint32_t packed_sample_rate;
    uint32_t packed_channel_mask;
    uint32_t packed_quantum_bytes;
    uint32_t packed_samples_per_quantum;
};

struct xsdk_xlogic_status
{
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
};

struct xsdk_xlogic_rate
{
    uint32_t request;
    uint64_t actual;
    uint32_t stream_format;
};

enum xsdk_xlogic_acquisition_state
{
    xSDK_xLOGIC_ACQ_IDLE = 0,
    xSDK_xLOGIC_ACQ_RUNNING,
    xSDK_xLOGIC_ACQ_STOPPING,
};

struct dev_context
{
    xLOGIC_xESK_Host_t admission;
#ifdef _WIN32
    struct xsdk_xlogic_winusb_owner *winusb;
#endif
    struct xsdk_xlogic_network *network;
    struct xsdk_xlogic_caps caps;
    struct xsdk_xlogic_status baseline;
    struct xsdk_xlogic_rate rates[xSDK_xLOGIC_RATE_COUNT];
    struct libusb_transfer **transfers;
    struct sr_context *sr_ctx;
    uint64_t limit_samples;
    uint64_t sent_samples;
    uint64_t scheduled_samples;
    uint64_t received_wire_bytes;
    unsigned int rate_count;
    unsigned int rate_index;
    unsigned int active_transfers;
    uint8_t channel_mask;
    uint8_t *decode_buffer;
    size_t decode_buffer_bytes;
    uint64_t transfer_sequence[xSDK_xLOGIC_TRANSFER_COUNT];
    gboolean transfer_completed[xSDK_xLOGIC_TRANSFER_COUNT];
    uint64_t next_submission;
    uint64_t next_consumption;
    enum xsdk_xlogic_acquisition_state acquisition_state;
    gboolean continuous;
    gboolean device_streaming;
    gboolean detached_kernel_driver;
    gboolean end_sent;
    gboolean failed;
    gboolean limit_reached;
    gboolean stop_command_sent;
    gboolean wire_accounting_partial;
};

SR_PRIV GVariant *xsdk_xlogic_network_sources(void);
SR_PRIV gboolean xsdk_xlogic_caps_valid(const struct xsdk_xlogic_caps_wire *wire);
SR_PRIV const char *xsdk_xlogic_network_source(const struct dev_context *devc);
SR_PRIV int xsdk_xlogic_network_source_set(struct dev_context *devc, const char *source);
SR_PRIV GSList *xsdk_xlogic_network_scan(struct sr_dev_driver *di, const char *connection);
SR_PRIV GSList *xsdk_xlogic_network_discover(void);
SR_PRIV int xsdk_xlogic_network_open(struct sr_dev_inst *sdi);
SR_PRIV int xsdk_xlogic_network_close(struct sr_dev_inst *sdi);
SR_PRIV void xsdk_xlogic_network_clear(struct dev_context *devc);
SR_PRIV int xsdk_xlogic_network_start(struct sr_dev_inst *sdi);
SR_PRIV int xsdk_xlogic_network_stop(struct sr_dev_inst *sdi);
SR_PRIV uint64_t xsdk_xlogic_network_limit(const struct dev_context *devc);
SR_PRIV gboolean xsdk_xlogic_network_depth_valid(const struct dev_context *devc, uint64_t samples);

#endif
