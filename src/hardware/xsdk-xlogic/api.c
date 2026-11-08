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

#include <config.h>
#include <limits.h>
#include <string.h>
#include "protocol.h"
#include "xlogic_winusb_backend.h"

static const uint32_t scanopts[] = {
    SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
    SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
    SR_CONF_CONN | SR_CONF_GET,
    SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
    SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
    SR_CONF_CONTINUOUS | SR_CONF_GET | SR_CONF_SET,
};

static const uint32_t network_devopts[] = {
    SR_CONF_DATA_SOURCE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
    SR_CONF_CONN | SR_CONF_GET,
    SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
    SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
};

#ifndef _WIN32
static uint64_t admission_clock(void *user)
{
    (void)user;
    return (uint64_t)(g_get_monotonic_time() / 1000);
}

static void admission_sleep(void *user, unsigned milliseconds)
{
    (void)user;
    g_usleep((gulong)milliseconds * 1000U);
}

static int admission_transfer(void *user, bool incoming, uint8_t request, uint16_t value, uint8_t *data, size_t bytes)
{
    struct sr_dev_inst *sdi = user;
    struct sr_usb_dev_inst *usb = sdi->conn;
    const uint8_t type = (incoming ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT) | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE;
    const int ret = libusb_control_transfer(usb->devhdl, type, request, value, xSDK_xLOGIC_INTERFACE, data, (uint16_t)bytes, 250U);
    if (incoming && request == 0x72U && ret == LIBUSB_ERROR_PIPE)
    {
        return -2;
    }
    return ret < 0 ? -1 : ret;
}

static int admission_open(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    xLOGIC_xESK_Host_t *host = &devc->admission;
    if (host->poisoned || host->operation_poisoned)
    {
        sr_err("xLOGIC admission is uncertain; close the device before another capture.");
        return SR_ERR;
    }
    memset(host, 0, sizeof(*host));
    host->user = sdi;
    host->transfer = admission_transfer;
    host->clock_ms = admission_clock;
    host->sleep_ms = admission_sleep;
    host->scoped = (devc->caps.feature_flags & xSDK_xLOGIC_CAP_SCOPED_CONTROL) != 0U;
    if (xlogic_xesk_open(host) != 0)
    {
        (void)xlogic_xesk_close(host);
        return SR_ERR;
    }
    return SR_OK;
}

static int control_in(struct libusb_device_handle *handle, uint8_t request, void *data, uint16_t length, gboolean report_errors)
{
    int ret;

    ret = libusb_control_transfer(handle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE, request, 0, 0, data,
                                  length, xSDK_xLOGIC_CONTROL_TIMEOUT_MS);
    if (ret == length)
    {
        return SR_OK;
    }
    if (!report_errors)
    {
        return SR_ERR;
    }
    if (ret >= 0)
    {
        sr_err("Control IN 0x%02x returned %d of %u bytes.", request, ret, length);
    }
    else
    {
        sr_err("Control IN 0x%02x failed: %s.", request, libusb_error_name(ret));
    }
    return SR_ERR;
}

static int control_out(struct libusb_device_handle *handle, uint8_t request, const void *data, uint16_t length)
{
    int ret;

    ret = libusb_control_transfer(handle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE, request, 0, 0,
                                  (unsigned char *)data, length, xSDK_xLOGIC_CONTROL_TIMEOUT_MS);
    if (ret == length)
    {
        return SR_OK;
    }
    if (ret >= 0)
    {
        sr_err("Control OUT 0x%02x returned %d of %u bytes.", request, ret, length);
    }
    else
    {
        sr_err("Control OUT 0x%02x failed: %s.", request, libusb_error_name(ret));
    }
    return SR_ERR;
}

#endif

static gboolean packed_features_valid(uint32_t features)
{
    const uint32_t known_features = xSDK_xLOGIC_CAP_RATE_DIAGNOSTICS | xSDK_xLOGIC_CAP_UNIFORM_CERTIFIED |
                                    xSDK_xLOGIC_CAP_CONTINUOUS_TANDEM | xSDK_xLOGIC_CAP_PACKED_NIBBLE_SELECT |
                                    xSDK_xLOGIC_CAP_PACKED_4CH_CONTIGUOUS | xSDK_xLOGIC_CAP_SCOPED_CONTROL |
                                    xSDK_xLOGIC_CAP_RAW_FULL_CLOCK_ONLY | xSDK_xLOGIC_CAP_GRACEFUL_FINISH;
    return (features & ~known_features) == 0U &&
           ((features & xSDK_xLOGIC_CAP_PACKED_4CH_CONTIGUOUS) == 0U || (features & xSDK_xLOGIC_CAP_CONTINUOUS_TANDEM) != 0U);
}

static gboolean packed_caps_valid(const struct xsdk_xlogic_caps_wire *wire)
{
    const uint32_t features = GUINT32_FROM_LE(wire->feature_flags);
    const uint32_t mask = GUINT32_FROM_LE(wire->packed_channel_mask);
    if (!packed_features_valid(features) ||
        ((features & xSDK_xLOGIC_CAP_RAW_FULL_CLOCK_ONLY) != 0U &&
         (features & (xSDK_xLOGIC_CAP_PACKED_4CH_CONTIGUOUS | xSDK_xLOGIC_CAP_PACKED_NIBBLE_SELECT)) != 0U))
    {
        return FALSE;
    }
    if ((features & xSDK_xLOGIC_CAP_PACKED_4CH_CONTIGUOUS) == 0U)
    {
        return TRUE;
    }
    return GUINT32_FROM_LE(wire->packed_sample_rate) == xSDK_xLOGIC_CONTIGUOUS_SAMPLE_RATE &&
           (mask == xSDK_xLOGIC_PACKED_LOWER_CHANNEL_MASK || mask == xSDK_xLOGIC_PACKED_UPPER_CHANNEL_MASK) &&
           GUINT32_FROM_LE(wire->packed_quantum_bytes) == xSDK_xLOGIC_CONTIGUOUS_QUANTUM_BYTES &&
           GUINT32_FROM_LE(wire->packed_samples_per_quantum) == xSDK_xLOGIC_CONTIGUOUS_SAMPLES_PER_QUANTUM &&
           (GUINT32_FROM_LE(wire->stream_buffer_bytes) % xSDK_xLOGIC_CONTIGUOUS_QUANTUM_BYTES) == 0U;
}

SR_PRIV gboolean xsdk_xlogic_caps_valid(const struct xsdk_xlogic_caps_wire *wire)
{
    return packed_caps_valid(wire) && GUINT32_FROM_LE(wire->magic) == xSDK_xLOGIC_MAGIC &&
           GUINT16_FROM_LE(wire->version) == xSDK_xLOGIC_ABI_VERSION && GUINT16_FROM_LE(wire->struct_size) == sizeof(*wire) &&
           GUINT32_FROM_LE(wire->sample_order) == xSDK_xLOGIC_SAMPLE_CHRONOLOGICAL && GUINT32_FROM_LE(wire->stream_buffer_bytes) != 0 &&
           GUINT32_FROM_LE(wire->stream_buffer_count) >= 2 && GUINT32_FROM_LE(wire->stream_buffer_bytes) <= INT_MAX &&
           (GUINT32_FROM_LE(wire->max_capture_bytes) != 0 ||
            (GUINT32_FROM_LE(wire->feature_flags) & xSDK_xLOGIC_CAP_CONTINUOUS_TANDEM) != 0) &&
           GUINT32_FROM_LE(wire->max_sample_rate) != 0 && GUINT32_FROM_LE(wire->max_channels) == 8 &&
           (GUINT32_FROM_LE(wire->feature_flags) & xSDK_xLOGIC_CAP_RATE_DIAGNOSTICS) != 0;
}

#ifndef _WIN32
static int read_caps(struct libusb_device_handle *handle, struct xsdk_xlogic_caps *caps, gboolean report_errors)
{
    struct xsdk_xlogic_caps_wire wire;

    if (control_in(handle, xSDK_xLOGIC_GET_CAPS, &wire, sizeof(wire), report_errors) != SR_OK)
    {
        return SR_ERR;
    }
    if (!xsdk_xlogic_caps_valid(&wire))
    {
        if (report_errors)
        {
            sr_err("Device returned incompatible xLOGIC capabilities.");
        }
        return SR_ERR;
    }
    caps->stream_buffer_bytes = GUINT32_FROM_LE(wire.stream_buffer_bytes);
    caps->stream_buffer_count = GUINT32_FROM_LE(wire.stream_buffer_count);
    caps->max_capture_bytes = GUINT32_FROM_LE(wire.max_capture_bytes);
    caps->max_sample_rate = GUINT32_FROM_LE(wire.max_sample_rate);
    caps->max_channels = GUINT32_FROM_LE(wire.max_channels);
    caps->feature_flags = GUINT32_FROM_LE(wire.feature_flags);
    caps->max_uniform_sample_rate = GUINT32_FROM_LE(wire.max_uniform_sample_rate);
    caps->packed_sample_rate = GUINT32_FROM_LE(wire.packed_sample_rate);
    caps->packed_channel_mask = GUINT32_FROM_LE(wire.packed_channel_mask);
    caps->packed_quantum_bytes = GUINT32_FROM_LE(wire.packed_quantum_bytes);
    caps->packed_samples_per_quantum = GUINT32_FROM_LE(wire.packed_samples_per_quantum);
    return SR_OK;
}

static int read_status(struct libusb_device_handle *handle, struct xsdk_xlogic_status *status)
{
    struct xsdk_xlogic_status_wire wire;

    if (control_in(handle, xSDK_xLOGIC_GET_STATUS, &wire, sizeof(wire), TRUE) != SR_OK)
    {
        return SR_ERR;
    }
    if (GUINT32_FROM_LE(wire.magic) != xSDK_xLOGIC_MAGIC || GUINT16_FROM_LE(wire.version) != xSDK_xLOGIC_ABI_VERSION ||
        GUINT16_FROM_LE(wire.struct_size) != sizeof(wire))
    {
        sr_err("Device returned an incompatible xLOGIC status record.");
        return SR_ERR;
    }
    status->configured = GUINT32_FROM_LE(wire.configured);
    status->running = GUINT32_FROM_LE(wire.running);
    status->produced_bytes = GUINT64_FROM_LE(wire.produced_bytes);
    status->transmitted_bytes = GUINT64_FROM_LE(wire.transmitted_bytes);
    status->overruns = GUINT32_FROM_LE(wire.overruns);
    status->usb_errors = GUINT32_FROM_LE(wire.usb_errors);
    status->ownership_errors = GUINT32_FROM_LE(wire.ownership_errors);
    status->requested_sample_rate = GUINT32_FROM_LE(wire.requested_sample_rate);
    status->actual_sample_rate = GUINT32_FROM_LE(wire.actual_sample_rate);
    status->timing_errors = GUINT32_FROM_LE(wire.timing_errors);
    status->status_flags = GUINT32_FROM_LE(wire.status_flags);
    status->stream_format = GUINT32_FROM_LE(wire.stream_format);
    return SR_OK;
}

static int write_config(struct libusb_device_handle *handle,
                        struct dev_context *devc,
                        uint32_t sample_rate,
                        uint32_t sample_count,
                        uint32_t channel_mask,
                        uint32_t stream_format)
{
    struct xsdk_xlogic_config_wire wire = {
        .magic = GUINT32_TO_LE(xSDK_xLOGIC_MAGIC),
        .version = GUINT16_TO_LE(xSDK_xLOGIC_ABI_VERSION),
        .struct_size = GUINT16_TO_LE(sizeof(wire)),
        .sample_rate = GUINT32_TO_LE(sample_rate),
        .sample_count = GUINT32_TO_LE(sample_count),
        .channel_mask = GUINT32_TO_LE(channel_mask),
        .stream_format = GUINT32_TO_LE(stream_format),
    };

    if (devc->admission.scoped)
    {
        return xlogic_xesk_operation(&devc->admission, 1U, (const uint8_t *)&wire) == 0 ? SR_OK : SR_ERR;
    }
    return control_out(handle, xSDK_xLOGIC_CONFIGURE, &wire, sizeof(wire));
}

static int command(struct libusb_device_handle *handle, uint8_t request)
{
    return control_out(handle, request, NULL, 0);
}

static int capture_command(struct sr_dev_inst *sdi, uint8_t request)
{
    struct dev_context *devc = sdi->priv;
    struct sr_usb_dev_inst *usb = sdi->conn;
    if (devc->admission.scoped)
    {
        return xlogic_xesk_operation(&devc->admission, request == xSDK_xLOGIC_START ? 2U : 3U, NULL) == 0 ? SR_OK : SR_ERR;
    }
    return command(usb->devhdl, request);
}

static int admission_finish(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct sr_usb_dev_inst *usb = sdi->conn;
    uint8_t status[64];
    int ret;
    if (!devc->admission.scoped)
    {
        return SR_OK;
    }
    ret = control_in(usb->devhdl, xSDK_xLOGIC_GET_STATUS, status, sizeof(status), TRUE);
    if (ret == SR_OK && devc->active_transfers == 0U)
    {
        ret = xlogic_xesk_release(&devc->admission, status, true);
    }
    else
    {
        ret = -1;
    }
    if (ret != 0)
    {
        sr_err("xLOGIC xESK cleanup remains unconfirmed; no new capture on this handle.");
        devc->admission.poisoned = true;
        // CLOSE is a separate cleanup request, never a replay of the mutation.
        if (xlogic_xesk_close(&devc->admission) != 0)
        {
            sr_err("xLOGIC xESK CLOSE failed; device lease/idle expiry remains authoritative.");
        }
        return SR_ERR;
    }
    return SR_OK;
}

#endif

static gboolean packed_format(uint32_t format)
{
    return format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS;
}

static uint32_t packed_caps_format(const struct xsdk_xlogic_caps *caps)
{
    if ((caps->feature_flags & xSDK_xLOGIC_CAP_PACKED_4CH_CONTIGUOUS) != 0U)
    {
        return xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS;
    }
    return xSDK_xLOGIC_STREAM_FORMAT_U8;
}

static void add_period_rate(struct dev_context *devc, uint32_t period)
{
    uint64_t actual;
    unsigned int i;

    if (!period || devc->rate_count >= xSDK_xLOGIC_RATE_COUNT ||
        ((devc->caps.feature_flags & xSDK_xLOGIC_CAP_RAW_FULL_CLOCK_ONLY) != 0U && period != 1U))
    {
        return;
    }
    actual = devc->caps.max_sample_rate / period;
    if (devc->caps.max_uniform_sample_rate && actual > devc->caps.max_uniform_sample_rate)
    {
        return;
    }
    for (i = 0; i < devc->rate_count; i++)
    {
        if (devc->rates[i].actual == actual)
        {
            return;
        }
    }
    devc->rates[devc->rate_count].request = actual;
    devc->rates[devc->rate_count].actual = actual;
    devc->rates[devc->rate_count].stream_format = xSDK_xLOGIC_STREAM_FORMAT_U8;
    devc->rate_count++;
}

static int build_rates(struct dev_context *devc)
{
    unsigned int i;
    uint32_t period;

    devc->rate_count = 0;
    devc->rate_index = 0;
    for (period = xSDK_xLOGIC_MAX_CAPTURE_PERIOD; period >= xSDK_xLOGIC_MIN_CAPTURE_PERIOD; period >>= 1U)
    {
        add_period_rate(devc, period);
    }
    if (packed_format(packed_caps_format(&devc->caps)) && devc->rate_count < xSDK_xLOGIC_RATE_COUNT)
    {
        devc->rates[devc->rate_count].request = devc->caps.packed_sample_rate;
        devc->rates[devc->rate_count].actual = devc->caps.packed_sample_rate;
        devc->rates[devc->rate_count].stream_format = packed_caps_format(&devc->caps);
        devc->rate_count++;
    }
    if (!devc->rate_count)
    {
        return SR_ERR;
    }
    for (i = 0; i < devc->rate_count; i++)
    {
        if (devc->rates[i].request == (devc->caps.max_sample_rate / 256U))
        {
            devc->rate_index = i;
            break;
        }
    }
    return SR_OK;
}

static int resize_decode_buffer(struct dev_context *devc)
{
    size_t capacity = 0U;
    uint8_t *buffer = NULL;
    const uint32_t format = packed_caps_format(&devc->caps);
    const size_t wire_bytes =
        format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS ? xSDK_xLOGIC_DIRECT_TRANSFER_BYTES : devc->caps.stream_buffer_bytes;

    if (packed_format(format))
    {
        capacity = (wire_bytes / devc->caps.packed_quantum_bytes) * devc->caps.packed_samples_per_quantum;
        buffer = g_try_malloc(capacity);
        if (!buffer)
        {
            return SR_ERR_MALLOC;
        }
    }
    g_free(devc->decode_buffer);
    devc->decode_buffer = buffer;
    devc->decode_buffer_bytes = capacity;
    return SR_OK;
}

SR_PRIV int xsdk_xlogic_context_set_caps(struct dev_context *devc, const struct xsdk_xlogic_caps *caps)
{
    int result;

    devc->caps = *caps;
    result = resize_decode_buffer(devc);
    if (result != SR_OK)
    {
        return result;
    }
    return build_rates(devc);
}

SR_PRIV struct dev_context *xsdk_xlogic_context_new(const struct xsdk_xlogic_caps *caps)
{
    struct dev_context *devc;
    int result;

    devc = g_try_malloc0(sizeof(*devc));
    if (!devc)
    {
        return NULL;
    }
    devc->limit_samples = xSDK_xLOGIC_DEFAULT_SAMPLES;
    if (caps != NULL)
    {
        result = xsdk_xlogic_context_set_caps(devc, caps);
        if (result != SR_OK)
        {
            g_free(devc->decode_buffer);
            g_free(devc);
            return NULL;
        }
        devc->limit_samples = MIN((uint64_t)xSDK_xLOGIC_DEFAULT_SAMPLES, (uint64_t)caps->max_capture_bytes);
        devc->continuous = caps->max_capture_bytes == 0U;
    }
    return devc;
}

static uint64_t max_finite_samples(const struct dev_context *devc)
{
    if (devc->network)
    {
        return xsdk_xlogic_network_limit(devc);
    }
    return xSDK_xLOGIC_MAX_LOGICAL_SAMPLES;
}

#ifndef _WIN32
static void read_usb_string(struct libusb_device_handle *handle, uint8_t index, char *dest, size_t capacity)
{
    int ret;

    dest[0] = '\0';
    if (!index)
    {
        return;
    }
    ret = libusb_get_string_descriptor_ascii(handle, index, (unsigned char *)dest, capacity);
    if (ret < 0)
    {
        dest[0] = '\0';
    }
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
    struct drv_context *drvc;
    struct sr_config *src;
    struct sr_usb_dev_inst *match;
    struct libusb_device_descriptor descriptor;
    libusb_device **devlist;
    GSList *devices, *conn_devices, *item;
    const char *conn;
    ssize_t count;
    int i;
    gboolean usb_only;

    drvc = di->context;
    devices = NULL;
    conn_devices = NULL;
    conn = NULL;
    for (item = options; item; item = item->next)
    {
        src = item->data;
        if (src->key == SR_CONF_CONN)
        {
            conn = g_variant_get_string(src->data, NULL);
        }
    }
    usb_only = conn && !strcmp(conn, "usb");
    if (conn && (g_str_has_prefix(conn, "tcp-raw/") || g_str_has_prefix(conn, "tcp-xesk/")))
    {
        return xsdk_xlogic_network_scan(di, conn);
    }
    if (conn && !usb_only)
    {
        conn_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);
    }
    if (conn && !usb_only && !conn_devices)
    {
        return std_scan_complete(di, NULL);
    }
    count = libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
    if (count < 0)
    {
        goto done;
    }
    for (i = 0; i < count; i++)
    {
        struct libusb_device_handle *handle;
        struct xsdk_xlogic_caps caps;
        struct dev_context *devc;
        struct sr_dev_inst *sdi;
        struct sr_channel_group *group;
        char manufacturer[64], product[64], serial[64];
        char connection_id[64], channel_name[8];
        unsigned int channel;

        if (libusb_get_device_descriptor(devlist[i], &descriptor) < 0 || descriptor.idVendor != xSDK_xLOGIC_VID ||
            descriptor.idProduct != xSDK_xLOGIC_PID)
        {
            continue;
        }
        if (conn_devices)
        {
            match = NULL;
            for (item = conn_devices; item; item = item->next)
            {
                match = item->data;
                if (match->bus == libusb_get_bus_number(devlist[i]) && match->address == libusb_get_device_address(devlist[i]))
                {
                    break;
                }
            }
            if (!item)
            {
                continue;
            }
        }
        if (libusb_open(devlist[i], &handle) < 0)
        {
            continue;
        }
        if (read_caps(handle, &caps, FALSE) != SR_OK)
        {
            libusb_close(handle);
            continue;
        }
        read_usb_string(handle, descriptor.iManufacturer, manufacturer, sizeof(manufacturer));
        read_usb_string(handle, descriptor.iProduct, product, sizeof(product));
        read_usb_string(handle, descriptor.iSerialNumber, serial, sizeof(serial));
        libusb_close(handle);
        if (usb_get_port_path(devlist[i], connection_id, sizeof(connection_id)) < 0)
        {
            g_snprintf(connection_id, sizeof(connection_id), "%u.%u", libusb_get_bus_number(devlist[i]),
                       libusb_get_device_address(devlist[i]));
        }
        devc = xsdk_xlogic_context_new(&caps);
        if (!devc)
        {
            continue;
        }
        sdi = g_malloc0(sizeof(*sdi));
        sdi->status = SR_ST_INACTIVE;
        sdi->vendor = g_strdup(manufacturer[0] ? manufacturer : "xSDK");
        sdi->model = g_strdup(product[0] ? product : "xLOGIC WinUSB");
        sdi->version = g_strdup_printf("ABI %u", xSDK_xLOGIC_ABI_VERSION);
        sdi->serial_num = g_strdup(serial);
        sdi->connection_id = g_strdup(connection_id);
        sdi->inst_type = SR_INST_USB;
        sdi->conn = sr_usb_dev_inst_new(libusb_get_bus_number(devlist[i]), libusb_get_device_address(devlist[i]), NULL);
        sdi->priv = devc;
        group = sr_channel_group_new(sdi, "Logic", NULL);
        for (channel = 0; channel < 8; channel++)
        {
            struct sr_channel *ch;
            g_snprintf(channel_name, sizeof(channel_name), "D%u", channel);
            ch = sr_channel_new(sdi, channel, SR_CHANNEL_LOGIC, TRUE, channel_name);
            group->channels = g_slist_append(group->channels, ch);
        }
        devices = g_slist_append(devices, sdi);
    }
    libusb_free_device_list(devlist, 1);
done:
    g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);
    if (!conn)
    {
        devices = g_slist_concat(devices, xsdk_xlogic_network_discover());
    }
    return std_scan_complete(di, devices);
}

#else
static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
    const char *connection = NULL;
    for (GSList *item = options; item; item = item->next)
    {
        const struct sr_config *config = item->data;
        if (config->key == SR_CONF_CONN)
        {
            connection = g_variant_get_string(config->data, NULL);
        }
    }
    if (connection && (g_str_has_prefix(connection, "tcp-raw/") || g_str_has_prefix(connection, "tcp-xesk/")))
    {
        return xsdk_xlogic_network_scan(di, connection);
    }
    return xsdk_xlogic_winusb_scan(di, connection);
}
#endif

static void clear_helper(struct dev_context *devc)
{
#ifdef _WIN32
    xsdk_xlogic_winusb_clear(devc);
#endif
    xsdk_xlogic_network_clear(devc);
    g_free(devc->transfers);
    g_free(devc->decode_buffer);
}

static int dev_clear(const struct sr_dev_driver *di)
{
#ifdef _WIN32
    int result = xsdk_xlogic_winusb_prepare_clear(di);
    if (result != SR_OK)
    {
        return result;
    }
#endif
    return std_dev_clear_with_callback(di, (std_dev_clear_callback)clear_helper);
}

#ifndef _WIN32
static int dev_open(struct sr_dev_inst *sdi)
{
    struct drv_context *drvc;
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    int active, ret;

    drvc = sdi->driver->context;
    devc = sdi->priv;
    if (devc->network)
    {
        return xsdk_xlogic_network_open(sdi);
    }
    usb = sdi->conn;
    ret = sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
    if (ret != SR_OK)
    {
        return ret;
    }
    active = libusb_kernel_driver_active(usb->devhdl, xSDK_xLOGIC_INTERFACE);
    if (active == 1)
    {
        ret = libusb_detach_kernel_driver(usb->devhdl, xSDK_xLOGIC_INTERFACE);
        if (ret < 0)
        {
            goto error;
        }
        devc->detached_kernel_driver = TRUE;
    }
    ret = libusb_claim_interface(usb->devhdl, xSDK_xLOGIC_INTERFACE);
    if (ret < 0)
    {
        goto error;
    }
    if (read_caps(usb->devhdl, &devc->caps, TRUE) != SR_OK || resize_decode_buffer(devc) != SR_OK || build_rates(devc) != SR_OK)
    {
        libusb_release_interface(usb->devhdl, xSDK_xLOGIC_INTERFACE);
        goto error;
    }
    devc->sr_ctx = drvc->sr_ctx;
    memset(&devc->admission, 0, sizeof(devc->admission));
    if (!devc->caps.max_uniform_sample_rate)
    {
        sr_warn("xLOGIC samplerates are experimental: the device reports no certified uniform-rate ceiling.");
    }
    return SR_OK;
error:
    if (devc->detached_kernel_driver)
    {
        libusb_attach_kernel_driver(usb->devhdl, xSDK_xLOGIC_INTERFACE);
        devc->detached_kernel_driver = FALSE;
    }
    libusb_close(usb->devhdl);
    usb->devhdl = NULL;
    return SR_ERR;
}

static int dev_close(struct sr_dev_inst *sdi)
{
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    int result = SR_OK;

    devc = sdi->priv;
    if (devc->network)
    {
        return xsdk_xlogic_network_close(sdi);
    }
    usb = sdi->conn;
    if (!usb->devhdl)
    {
        return SR_ERR_BUG;
    }
    if (devc->active_transfers != 0U)
    {
        return SR_ERR;
    }
    if (xlogic_xesk_close(&devc->admission) != 0)
    {
        sr_err("xLOGIC xESK connection close failed.");
        devc->failed = TRUE;
        result = SR_ERR;
    }
    libusb_release_interface(usb->devhdl, xSDK_xLOGIC_INTERFACE);
    if (devc->detached_kernel_driver)
    {
        libusb_attach_kernel_driver(usb->devhdl, xSDK_xLOGIC_INTERFACE);
        devc->detached_kernel_driver = FALSE;
    }
    libusb_close(usb->devhdl);
    usb->devhdl = NULL;
    return result;
}

#else
static int dev_open(struct sr_dev_inst *sdi)
{
    if (((struct dev_context *)sdi->priv)->network)
    {
        return xsdk_xlogic_network_open(sdi);
    }
    return xsdk_xlogic_winusb_open(sdi);
}

static int dev_close(struct sr_dev_inst *sdi)
{
    if (((struct dev_context *)sdi->priv)->network)
    {
        return xsdk_xlogic_network_close(sdi);
    }
    return xsdk_xlogic_winusb_close(sdi);
}
#endif

static int config_get(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
    struct dev_context *devc;

    (void)cg;
    if (!sdi || !data)
    {
        return SR_ERR_ARG;
    }
    devc = sdi->priv;
    switch (key)
    {
    case SR_CONF_DATA_SOURCE:
        if (!devc->network)
        {
            return SR_ERR_NA;
        }
        *data = g_variant_new_string(xsdk_xlogic_network_source(devc));
        break;
    case SR_CONF_CONN:
        *data = g_variant_new_string(sdi->connection_id);
        break;
    case SR_CONF_LIMIT_SAMPLES:
        *data = g_variant_new_uint64(devc->limit_samples);
        break;
    case SR_CONF_CONTINUOUS:
        *data = g_variant_new_boolean(devc->continuous);
        break;
    case SR_CONF_SAMPLERATE:
        if (!devc->rate_count)
        {
            return SR_ERR_DEV_CLOSED;
        }
        *data = g_variant_new_uint64(devc->rates[devc->rate_index].actual);
        break;
    default:
        return SR_ERR_NA;
    }
    return SR_OK;
}

static int set_sample_limit(struct dev_context *devc, uint64_t value)
{
    /* limit_samples=0 is the sigrok-cli continuous compatibility alias. */
    if (value == 0U)
    {
        if (devc->network)
        {
            return SR_ERR_ARG;
        }
        devc->continuous = TRUE;
        return SR_OK;
    }
    if (value > max_finite_samples(devc))
    {
        return SR_ERR_ARG;
    }
    if (devc->network && !xsdk_xlogic_network_depth_valid(devc, value))
    {
        return SR_ERR_ARG;
    }
    devc->limit_samples = value;
    return SR_OK;
}

static int config_set(uint32_t key, GVariant *data, const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
    struct dev_context *devc;
    uint64_t value;
    unsigned int i;

    (void)cg;
    if (!sdi || !data)
    {
        return SR_ERR_ARG;
    }
    devc = sdi->priv;
    if (devc->acquisition_state != xSDK_xLOGIC_ACQ_IDLE)
    {
        return SR_ERR;
    }
    switch (key)
    {
    case SR_CONF_DATA_SOURCE:
        if (!devc->network)
        {
            return SR_ERR_NA;
        }
        return xsdk_xlogic_network_source_set(devc, g_variant_get_string(data, NULL));
    case SR_CONF_LIMIT_SAMPLES:
        return set_sample_limit(devc, g_variant_get_uint64(data));
    case SR_CONF_CONTINUOUS:
        if (devc->network && g_variant_get_boolean(data))
        {
            return SR_ERR_ARG;
        }
        devc->continuous = g_variant_get_boolean(data);
        break;
    case SR_CONF_SAMPLERATE:
        value = g_variant_get_uint64(data);
        for (i = 0; i < devc->rate_count; i++)
        {
            if (devc->rates[i].actual == value)
            {
                devc->rate_index = i;
                if (devc->limit_samples > max_finite_samples(devc))
                {
                    devc->limit_samples = max_finite_samples(devc);
                }
                return SR_OK;
            }
        }
        return SR_ERR_SAMPLERATE;
    default:
        return SR_ERR_NA;
    }
    return SR_OK;
}

static int config_list(uint32_t key, GVariant **data, const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
    const struct dev_context *devc;
    uint64_t rates[xSDK_xLOGIC_RATE_COUNT];
    unsigned int i;

    switch (key)
    {
    case SR_CONF_SCAN_OPTIONS:
    case SR_CONF_DEVICE_OPTIONS:
        if (sdi && ((const struct dev_context *)sdi->priv)->network)
        {
            return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, network_devopts);
        }
        return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
    case SR_CONF_DATA_SOURCE:
        if (!sdi || !((const struct dev_context *)sdi->priv)->network)
        {
            return SR_ERR_NA;
        }
        *data = xsdk_xlogic_network_sources(sdi->priv);
        break;
    case SR_CONF_SAMPLERATE:
        if (!sdi)
        {
            return SR_ERR_ARG;
        }
        devc = sdi->priv;
        if (!devc->rate_count)
        {
            return SR_ERR_DEV_CLOSED;
        }
        for (i = 0; i < devc->rate_count; i++)
        {
            rates[i] = devc->rates[i].actual;
        }
        *data = std_gvar_samplerates(rates, devc->rate_count);
        break;
    case SR_CONF_LIMIT_SAMPLES:
        if (!sdi)
        {
            return SR_ERR_ARG;
        }
        devc = sdi->priv;
        *data = std_gvar_tuple_u64(1U, max_finite_samples(devc));
        break;
    default:
        return SR_ERR_NA;
    }
    return SR_OK;
}

static uint8_t enabled_channel_mask(const struct sr_dev_inst *sdi)
{
    const GSList *item;
    uint8_t mask;

    mask = 0;
    for (item = sdi->channels; item; item = item->next)
    {
        const struct sr_channel *channel = item->data;
        if (channel->enabled && channel->index >= 0 && channel->index < 8)
        {
            mask |= 1U << channel->index;
        }
    }
    return mask;
}

#ifndef _WIN32
static int account_transfer_bytes(struct dev_context *devc, const struct libusb_transfer *transfer)
{
    const gboolean direct_cancel = transfer->status == LIBUSB_TRANSFER_CANCELLED &&
                                   devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS;
    uint64_t received;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && !direct_cancel)
    {
        return SR_OK;
    }
    if (transfer->actual_length < 0 || transfer->length < 0 || transfer->actual_length > transfer->length)
    {
        sr_err("xLOGIC transfer returned an invalid byte count.");
        return SR_ERR;
    }
    received = (uint64_t)transfer->actual_length;
    if (received > UINT64_MAX - devc->received_wire_bytes)
    {
        sr_err("xLOGIC wire byte count overflow.");
        return SR_ERR;
    }
    devc->received_wire_bytes += received;
    if (direct_cancel)
    {
        devc->wire_accounting_partial = TRUE;
    }
    return SR_OK;
}

static void check_direct_wire_accounting(struct dev_context *devc, const struct xsdk_xlogic_status *status)
{
    if (devc->rates[devc->rate_index].stream_format != xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS)
    {
        return;
    }
    if (status->produced_bytes < devc->baseline.produced_bytes ||
        devc->received_wire_bytes > status->produced_bytes - devc->baseline.produced_bytes)
    {
        sr_err("xLOGIC direct capture received more bytes than committed device output.");
        devc->failed = TRUE;
    }
    if (devc->wire_accounting_partial)
    {
        sr_dbg("xLOGIC direct wire accounting is partial after host STOP/cancellation.");
    }
}

static void finish_acquisition(struct sr_dev_inst *sdi)
{
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    struct xsdk_xlogic_status final_status;
    gint64 deadline;
    uint64_t transmitted;

    devc = sdi->priv;
    usb = sdi->conn;
    if (devc->end_sent)
    {
        return;
    }
    memset(&final_status, 0, sizeof(final_status));
    deadline = g_get_monotonic_time() + 1000000;
    do
    {
        if (read_status(usb->devhdl, &final_status) != SR_OK)
        {
            devc->failed = TRUE;
            break;
        }
        if (!final_status.running &&
            (!devc->admission.scoped || ((final_status.status_flags & (xLOGIC_xESK_QUIESCENT | xLOGIC_xESK_START_PENDING |
                                                                       xLOGIC_xESK_STOP_PENDING)) == xLOGIC_xESK_QUIESCENT)))
        {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    if (final_status.running ||
        (devc->admission.scoped && (final_status.status_flags & (xLOGIC_xESK_QUIESCENT | xLOGIC_xESK_START_PENDING |
                                                                 xLOGIC_xESK_STOP_PENDING)) != xLOGIC_xESK_QUIESCENT))
    {
        sr_err("xLOGIC STOP did not quiesce within one second.");
        devc->failed = TRUE;
    }
    if (final_status.overruns != devc->baseline.overruns || final_status.usb_errors != devc->baseline.usb_errors ||
        final_status.ownership_errors != devc->baseline.ownership_errors)
    {
        sr_err("xLOGIC reported a new device-side capture error.");
        devc->failed = TRUE;
    }
    if (final_status.transmitted_bytes < devc->baseline.transmitted_bytes)
    {
        sr_err("xLOGIC transmitted byte counter decreased during capture.");
        devc->failed = TRUE;
        transmitted = 0U;
    }
    else
    {
        transmitted = final_status.transmitted_bytes - devc->baseline.transmitted_bytes;
    }
    if (devc->limit_reached && devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_U8 && !devc->device_streaming &&
        transmitted != devc->sent_samples)
    {
        sr_err("xLOGIC finite capture accounting mismatch: host=%" G_GUINT64_FORMAT " device=%" G_GUINT64_FORMAT ".", devc->sent_samples,
               transmitted);
        devc->failed = TRUE;
    }
    if (devc->limit_reached && devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_U8 && devc->device_streaming &&
        transmitted != devc->received_wire_bytes)
    {
        sr_err("xLOGIC streamed finite capture accounting mismatch: host=%" G_GUINT64_FORMAT " device=%" G_GUINT64_FORMAT ".",
               devc->received_wire_bytes, transmitted);
        devc->failed = TRUE;
    }
    if (packed_format(devc->rates[devc->rate_index].stream_format) && !devc->wire_accounting_partial &&
        transmitted != devc->received_wire_bytes)
    {
        sr_err("xLOGIC packed capture accounting mismatch: host=%" G_GUINT64_FORMAT " device=%" G_GUINT64_FORMAT ".",
               devc->received_wire_bytes, transmitted);
        devc->failed = TRUE;
    }
    check_direct_wire_accounting(devc, &final_status);

    if (!(final_status.status_flags & xSDK_xLOGIC_STATUS_ACTUAL_RATE_VALID))
    {
        sr_err("xLOGIC did not report the actual samplerate.");
        devc->failed = TRUE;
    }
    else if (final_status.actual_sample_rate != devc->rates[devc->rate_index].actual)
    {
        sr_err("xLOGIC reported an unexpected actual samplerate.");
        devc->failed = TRUE;
    }
    if (final_status.stream_format != devc->rates[devc->rate_index].stream_format)
    {
        sr_err("xLOGIC reported an unexpected stream format.");
        devc->failed = TRUE;
    }
    if (admission_finish(sdi) != SR_OK)
    {
        devc->failed = TRUE;
    }
    if (devc->admission.scoped && (final_status.status_flags & xLOGIC_xESK_SESSION_FAULT) != 0U)
    {
        devc->failed = TRUE;
    }
    if (devc->failed)
    {
        sr_err("xLOGIC acquisition ended with errors.");
    }
    std_session_send_df_end(sdi);
    devc->end_sent = TRUE;
    usb_source_remove(sdi->session, devc->sr_ctx);
    g_free(devc->transfers);
    devc->transfers = NULL;
    /* Contiguous firmware streams until host STOP; every received byte is
     * two complete samples, including a final partial publication. */
    devc->acquisition_state = xSDK_xLOGIC_ACQ_IDLE;
}

static void free_transfer(struct sr_dev_inst *sdi, struct libusb_transfer *transfer)
{
    struct dev_context *devc;
    unsigned int i;

    devc = sdi->priv;
    for (i = 0; i < xSDK_xLOGIC_TRANSFER_COUNT; i++)
    {
        if (devc->transfers[i] == transfer)
        {
            devc->transfers[i] = NULL;
            devc->transfer_completed[i] = FALSE;
            break;
        }
    }
    g_free(transfer->buffer);
    libusb_free_transfer(transfer);
    if (devc->active_transfers)
    {
        devc->active_transfers--;
    }
}

static void cancel_transfers(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    unsigned int i;

    for (i = 0; i < xSDK_xLOGIC_TRANSFER_COUNT; i++)
    {
        if (devc->transfers && devc->transfers[i])
        {
            if (devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS)
            {
                devc->wire_accounting_partial = TRUE;
            }
            if (devc->transfer_completed[i])
            {
                if (account_transfer_bytes(devc, devc->transfers[i]) != SR_OK)
                {
                    devc->failed = TRUE;
                }
                free_transfer(sdi, devc->transfers[i]);
            }
            else
            {
                libusb_cancel_transfer(devc->transfers[i]);
            }
        }
    }
}

static void request_stop(struct sr_dev_inst *sdi)
{
    struct dev_context *devc;

    devc = sdi->priv;
    if (devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
    {
        return;
    }
    if (devc->acquisition_state == xSDK_xLOGIC_ACQ_RUNNING)
    {
        devc->acquisition_state = xSDK_xLOGIC_ACQ_STOPPING;
    }

    /* Keep reads queued until usb_event_source sends STOP outside libusb
     * callbacks. Cancelling first can starve the device's bounded buffer pool. */
}

static void send_samples(struct sr_dev_inst *sdi, uint8_t *data, size_t length)
{
    const struct sr_datafeed_logic logic = {
        .length = length,
        .unitsize = 1,
        .data = data,
    };
    const struct sr_datafeed_packet packet = {
        .type = SR_DF_LOGIC,
        .payload = &logic,
    };

    sr_session_send(sdi, &packet);
}

static void send_samplerate(struct sr_dev_inst *sdi, uint64_t samplerate)
{
    struct sr_config *config;
    struct sr_datafeed_meta meta;
    struct sr_datafeed_packet packet;

    config = sr_config_new(SR_CONF_SAMPLERATE, g_variant_new_uint64(samplerate));
    meta.config = g_slist_append(NULL, config);
    packet.type = SR_DF_META;
    packet.payload = &meta;
    sr_session_send(sdi, &packet);
    g_slist_free(meta.config);
    sr_config_free(config);
}

static int decode_direct_samples(struct dev_context *devc, const uint8_t *physical, size_t physical_bytes, size_t *decoded_bytes)
{
    const unsigned shift = devc->channel_mask == xSDK_xLOGIC_PACKED_UPPER_CHANNEL_MASK ? 4U : 0U;
    if (physical_bytes > devc->decode_buffer_bytes / 2U)
    {
        return SR_ERR;
    }
    // Each byte contains two complete samples, even at a USB fragment or STOP
    // boundary. Publication quanta introduce no decoder framing or padding.
    for (size_t offset = 0U; offset < physical_bytes; ++offset)
    {
        const uint8_t packed = physical[offset];
        devc->decode_buffer[2U * offset] = (uint8_t)((packed & 0x0fU) << shift);
        devc->decode_buffer[2U * offset + 1U] = (uint8_t)((packed >> 4U) << shift);
    }
    *decoded_bytes = physical_bytes * 2U;
    return SR_OK;
}

static int decode_transfer(struct dev_context *devc, struct libusb_transfer *transfer, uint8_t **samples, size_t *length)
{
    const uint32_t format = devc->rates[devc->rate_index].stream_format;

    if (transfer->actual_length <= 0 || transfer->actual_length > transfer->length ||
        (transfer->actual_length != transfer->length && format != xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS))
    {
        sr_err("xLOGIC transfer returned %d of %d scheduled bytes.", transfer->actual_length, transfer->length);
        return SR_ERR;
    }
    *samples = transfer->buffer;
    *length = (size_t)transfer->actual_length;
    if (format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS)
    {
        *samples = devc->decode_buffer;
        return decode_direct_samples(devc, transfer->buffer, *length, length);
    }

    return SR_OK;
}

static void accept_samples(struct sr_dev_inst *sdi, uint8_t *samples, size_t length)
{
    struct dev_context *devc = sdi->priv;
    const size_t accepted = devc->continuous ? length : MIN((uint64_t)length, devc->limit_samples - devc->sent_samples);

    if (accepted)
    {
        send_samples(sdi, samples, accepted);
        devc->sent_samples += accepted;
    }
    devc->limit_reached = !devc->continuous && devc->sent_samples == devc->limit_samples;
}

static size_t transfer_buffer_bytes(const struct dev_context *devc)
{
    /* Host reads may span many device buffers. Sixteen 2 MiB direct reads
     * cover about 134 ms at 250 MB/s without enlarging device SRAM. */
    return devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS
               ? xSDK_xLOGIC_DIRECT_TRANSFER_BYTES
               : devc->caps.stream_buffer_bytes;
}

static uint64_t next_transfer_size(const struct dev_context *devc, int *wire_length)
{
    const gboolean packed = packed_format(devc->rates[devc->rate_index].stream_format);
    const uint64_t capacity = packed ? devc->decode_buffer_bytes : devc->caps.stream_buffer_bytes;
    const uint64_t remaining = devc->continuous ? capacity : devc->limit_samples - devc->scheduled_samples;
    const uint64_t scheduled = MIN(capacity, remaining);

    *wire_length = packed || devc->device_streaming ? (int)transfer_buffer_bytes(devc) : (int)scheduled;
    return scheduled;
}

static int resubmit_transfer(struct dev_context *devc, struct libusb_transfer *transfer)
{
    const uint64_t scheduled = next_transfer_size(devc, &transfer->length);
    unsigned int index;
    int ret = libusb_submit_transfer(transfer);

    if (ret != LIBUSB_SUCCESS)
    {
        sr_err("Unable to resubmit xLOGIC transfer: %s.", libusb_error_name(ret));
        return SR_ERR;
    }
    for (index = 0U; index < xSDK_xLOGIC_TRANSFER_COUNT; index++)
    {
        if (devc->transfers[index] == transfer)
        {
            devc->transfer_sequence[index] = devc->next_submission++;
            break;
        }
    }
    devc->scheduled_samples += scheduled;
    return SR_OK;
}

static gboolean more_samples_needed(const struct dev_context *devc)
{
    return !devc->limit_reached && (devc->continuous || devc->scheduled_samples < devc->limit_samples ||
                                    devc->rates[devc->rate_index].stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS);
}

static void receive_ordered_transfer(struct libusb_transfer *transfer)
{
    struct sr_dev_inst *sdi = transfer->user_data;
    struct dev_context *devc = sdi->priv;
    const gboolean cancelled = transfer->status == LIBUSB_TRANSFER_CANCELLED;
    size_t length;
    uint8_t *samples;

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && !cancelled)
    {
        sr_err("xLOGIC transfer failed with status %d.", transfer->status);
        devc->failed = TRUE;
    }
    if (account_transfer_bytes(devc, transfer) != SR_OK)
    {
        devc->failed = TRUE;
        free_transfer(sdi, transfer);
        request_stop(sdi);
        return;
    }
    if (devc->acquisition_state != xSDK_xLOGIC_ACQ_RUNNING)
    {
        free_transfer(sdi, transfer);
        return;
    }
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        if (decode_transfer(devc, transfer, &samples, &length) != SR_OK)
        {
            devc->failed = TRUE;
        }
        else
        {
            accept_samples(sdi, samples, length);
            if (devc->acquisition_state != xSDK_xLOGIC_ACQ_RUNNING)
            {
                free_transfer(sdi, transfer);
                return;
            }
            if (more_samples_needed(devc))
            {
                if (resubmit_transfer(devc, transfer) == SR_OK)
                {
                    return;
                }
                devc->failed = TRUE;
            }
        }
    }
    free_transfer(sdi, transfer);
    if (devc->failed || devc->limit_reached || cancelled)
    {
        request_stop(sdi);
    }
}

static void LIBUSB_CALL receive_transfer(struct libusb_transfer *transfer)
{
    struct sr_dev_inst *sdi = transfer->user_data;
    struct dev_context *devc = sdi->priv;
    unsigned int index;
    gboolean consumed;

    if (devc->rates[devc->rate_index].stream_format != xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS ||
        transfer->status != LIBUSB_TRANSFER_COMPLETED || devc->acquisition_state != xSDK_xLOGIC_ACQ_RUNNING)
    {
        receive_ordered_transfer(transfer);
        return;
    }
    for (index = 0U; index < xSDK_xLOGIC_TRANSFER_COUNT; index++)
    {
        if (devc->transfers[index] == transfer)
        {
            devc->transfer_completed[index] = TRUE;
            break;
        }
    }
    /* Fragments share a quantum carry; consume endpoint reads in submission
     * order even when libusb dispatches their completion callbacks out of order. */
    do
    {
        consumed = FALSE;
        for (index = 0U; index < xSDK_xLOGIC_TRANSFER_COUNT; index++)
        {
            if (devc->transfer_completed[index] && devc->transfer_sequence[index] == devc->next_consumption)
            {
                devc->transfer_completed[index] = FALSE;
                devc->next_consumption++;
                receive_ordered_transfer(devc->transfers[index]);
                consumed = TRUE;
                break;
            }
        }
    } while (consumed);
}

static int usb_event_source(int fd, int revents, void *cb_data)
{
    struct sr_dev_inst *sdi;
    struct drv_context *drvc;
    struct dev_context *devc;
    struct timeval timeout = {0, 0};

    (void)fd;
    (void)revents;
    sdi = cb_data;
    drvc = sdi->driver->context;
    devc = sdi->priv;
    libusb_handle_events_timeout(drvc->sr_ctx->libusb_ctx, &timeout);
    if (devc->acquisition_state == xSDK_xLOGIC_ACQ_RUNNING && xlogic_xesk_tick(&devc->admission) != 0)
    {
        devc->failed = TRUE;
        request_stop(sdi);
    }
    if (devc->acquisition_state == xSDK_xLOGIC_ACQ_STOPPING && !devc->stop_command_sent)
    {
        /* Synchronous control transfers must not run from a libusb transfer
         * callback. Defer STOP until event dispatch has returned. */
        devc->stop_command_sent = TRUE;
        if (capture_command(sdi, xSDK_xLOGIC_STOP) != SR_OK)
        {
            devc->failed = TRUE;
        }
        cancel_transfers(sdi);
    }
    if (devc->acquisition_state == xSDK_xLOGIC_ACQ_STOPPING && !devc->active_transfers)
    {
        finish_acquisition(sdi);
    }
    return TRUE;
}

static int submit_transfers(struct sr_dev_inst *sdi)
{
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    gboolean packed;
    gboolean continuous;
    unsigned int i;

    devc = sdi->priv;
    usb = sdi->conn;
    packed = packed_format(devc->rates[devc->rate_index].stream_format);
    continuous = devc->continuous;
    devc->transfers = g_try_new0(struct libusb_transfer *, xSDK_xLOGIC_TRANSFER_COUNT);
    if (!devc->transfers)
    {
        return SR_ERR_MALLOC;
    }
    for (i = 0; i < xSDK_xLOGIC_TRANSFER_COUNT && (continuous || (packed || devc->scheduled_samples < devc->limit_samples)); i++)
    {
        struct libusb_transfer *transfer;
        uint8_t *buffer;
        uint64_t remaining;
        int transfer_length;
        int ret;

        remaining = next_transfer_size(devc, &transfer_length);
        buffer = g_try_malloc(transfer_buffer_bytes(devc));
        transfer = libusb_alloc_transfer(0);
        if (!buffer || !transfer)
        {
            g_free(buffer);
            if (transfer)
            {
                libusb_free_transfer(transfer);
            }
            return SR_ERR_MALLOC;
        }
        libusb_fill_bulk_transfer(transfer, usb->devhdl, xSDK_xLOGIC_EP_IN, buffer, transfer_length, receive_transfer, sdi,
                                  xSDK_xLOGIC_TRANSFER_TIMEOUT_MS);
        ret = libusb_submit_transfer(transfer);
        if (ret != LIBUSB_SUCCESS)
        {
            sr_err("Unable to submit xLOGIC transfer: %s.", libusb_error_name(ret));
            g_free(buffer);
            libusb_free_transfer(transfer);
            return SR_ERR;
        }
        devc->transfers[i] = transfer;
        devc->transfer_sequence[i] = devc->next_submission++;
        devc->active_transfers++;
        devc->scheduled_samples += remaining;
    }
    return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *const_sdi)
{
    struct sr_dev_inst *sdi;
    struct dev_context *devc;
    struct sr_usb_dev_inst *usb;
    struct xsdk_xlogic_status configured_status;
    struct xsdk_xlogic_rate *rate;
    gboolean continuous;
    gboolean device_streaming;
    int ret;

    sdi = (struct sr_dev_inst *)const_sdi;
    devc = sdi->priv;
    if (devc->network)
    {
        devc->channel_mask = enabled_channel_mask(sdi);
        return xsdk_xlogic_network_start(sdi);
    }
    usb = sdi->conn;
    if (devc->acquisition_state != xSDK_xLOGIC_ACQ_IDLE)
    {
        return SR_ERR;
    }
    devc->channel_mask = enabled_channel_mask(sdi);
    if (!devc->channel_mask)
    {
        return SR_ERR_ARG;
    }
    if (read_status(usb->devhdl, &devc->baseline) != SR_OK || devc->baseline.running)
    {
        return SR_ERR;
    }
    rate = &devc->rates[devc->rate_index];
    if ((devc->caps.feature_flags & xSDK_xLOGIC_CAP_RAW_FULL_CLOCK_ONLY) != 0U && devc->channel_mask != 0xffU)
    {
        sr_err("xLOGIC full-clock RAW profile requires all eight channels.");
        return SR_ERR_ARG;
    }
    continuous = devc->continuous;
    device_streaming = continuous || devc->limit_samples > devc->caps.max_capture_bytes ||
                       rate->stream_format == xSDK_xLOGIC_STREAM_FORMAT_PACKED_4CH_CONTIGUOUS;
    if (device_streaming && (devc->caps.feature_flags & xSDK_xLOGIC_CAP_CONTINUOUS_TANDEM) == 0U)
    {
        sr_err("xLOGIC device does not advertise continuous tandem capture.");
        return SR_ERR_ARG;
    }
    if (rate->stream_format == xSDK_xLOGIC_STREAM_FORMAT_U8 && rate->actual == devc->caps.max_sample_rate && !device_streaming &&
        (devc->limit_samples & (xSDK_xLOGIC_PERIOD1_SAMPLE_GRANULARITY - 1U)))
    {
        sr_err("xLOGIC 333.333333 MS/s raw mode requires a sample count divisible by 64.");
        return SR_ERR_ARG;
    }
    if (packed_format(rate->stream_format))
    {
        const gboolean selectable_nibble = (devc->caps.feature_flags & xSDK_xLOGIC_CAP_PACKED_NIBBLE_SELECT) != 0U;
        const gboolean selected_nibble =
            devc->channel_mask == xSDK_xLOGIC_PACKED_LOWER_CHANNEL_MASK || devc->channel_mask == xSDK_xLOGIC_PACKED_UPPER_CHANNEL_MASK;
        if ((!selectable_nibble && devc->channel_mask != devc->caps.packed_channel_mask) || (selectable_nibble && !selected_nibble))
        {
            sr_err("xLOGIC packed mode requires exactly one advertised four-channel nibble enabled.");
            return SR_ERR_ARG;
        }
        sr_warn("xLOGIC packed mode at %" G_GUINT64_FORMAT " samples/s remains experimental.", rate->actual);
    }
    if (admission_open(sdi) != SR_OK)
    {
        return SR_ERR;
    }
    if (device_streaming)
    {
        ret = write_config(usb->devhdl, devc, rate->request, 0U, devc->channel_mask, rate->stream_format);
    }
    else
    {
        ret = write_config(usb->devhdl, devc, rate->request, (uint32_t)devc->limit_samples, devc->channel_mask, rate->stream_format);
    }
    if (ret != SR_OK)
    {
        (void)admission_finish(sdi);
        return ret;
    }
    if (read_status(usb->devhdl, &configured_status) != SR_OK || !configured_status.configured ||
        configured_status.requested_sample_rate != rate->request || configured_status.stream_format != rate->stream_format)
    {
        sr_err("xLOGIC rejected the capture configuration.");
        (void)admission_finish(sdi);
        return SR_ERR;
    }
    devc->sent_samples = 0;
    devc->scheduled_samples = 0;
    devc->received_wire_bytes = 0;
    devc->wire_accounting_partial = FALSE;
    memset(devc->transfer_completed, 0, sizeof(devc->transfer_completed));
    devc->next_submission = 0U;
    devc->next_consumption = 0U;
    devc->device_streaming = device_streaming;
    devc->active_transfers = 0;
    devc->failed = FALSE;
    devc->limit_reached = FALSE;
    devc->end_sent = FALSE;
    devc->stop_command_sent = FALSE;
    devc->acquisition_state = xSDK_xLOGIC_ACQ_RUNNING;
    ret = usb_source_add(sdi->session, devc->sr_ctx, 100, usb_event_source, sdi);
    if (ret != SR_OK)
    {
        devc->acquisition_state = xSDK_xLOGIC_ACQ_IDLE;
        (void)admission_finish(sdi);
        return ret;
    }
    std_session_send_df_header(sdi);
    send_samplerate(sdi, rate->actual);
    ret = submit_transfers(sdi);
    if (ret != SR_OK)
    {
        devc->failed = TRUE;
        request_stop(sdi);
        return ret;
    }
    ret = capture_command(sdi, xSDK_xLOGIC_START);
    if (ret != SR_OK)
    {
        devc->failed = TRUE;
        request_stop(sdi);
        return ret;
    }
    /* Accepted START resets firmware acquisition counters. Rejected START
     * leaves the previous acquisition's evidence intact. */
    memset(&devc->baseline, 0, sizeof(devc->baseline));
    return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
    if (((struct dev_context *)sdi->priv)->network)
    {
        return xsdk_xlogic_network_stop(sdi);
    }
    request_stop(sdi);
    return SR_OK;
}

#else
static int dev_acquisition_start(const struct sr_dev_inst *device)
{
    struct sr_dev_inst *sdi = (struct sr_dev_inst *)device;
    struct dev_context *devc = sdi->priv;
    devc->channel_mask = enabled_channel_mask(sdi);
    if (devc->network)
    {
        return xsdk_xlogic_network_start(sdi);
    }
    return xsdk_xlogic_winusb_start(sdi);
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
    if (((struct dev_context *)sdi->priv)->network)
    {
        return xsdk_xlogic_network_stop(sdi);
    }
    return xsdk_xlogic_winusb_stop(sdi);
}
#endif

static struct sr_dev_driver xsdk_xlogic_driver_info = {
    .name = "xsdk-xlogic",
    .longname = "xSDK xLOGIC WinUSB / TCP",
    .api_version = 1,
    .init = std_init,
    .cleanup = std_cleanup,
    .scan = scan,
    .dev_list = std_dev_list,
    .dev_clear = dev_clear,
    .config_get = config_get,
    .config_set = config_set,
    .config_list = config_list,
    .dev_open = dev_open,
    .dev_close = dev_close,
    .dev_acquisition_start = dev_acquisition_start,
    .dev_acquisition_stop = dev_acquisition_stop,
    .context = NULL,
};

SR_REGISTER_DEV_DRIVER(xsdk_xlogic_driver_info);
