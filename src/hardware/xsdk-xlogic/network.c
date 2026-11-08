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
#ifdef _WIN32
#include <winsock2.h>
#else
#include <errno.h>
#include <fcntl.h>
#endif
#include <string.h>
#include <zlib.h>
#include "protocol.h"
#include "network_codec.h"
#include "network_auto.h"
#include "network_discovery.h"
#include "xlogic_network_wire.h"
#include "xesk_network_pump.h"

#define xSDK_xLOGIC_NET_MAGIC          xLOGIC_NET_MAGIC
#define xSDK_xLOGIC_NET_VERSION        xLOGIC_NET_VERSION
#define xSDK_xLOGIC_NET_REQUEST_BYTES  xLOGIC_NET_REQUEST_BYTES
#define xSDK_xLOGIC_NET_RESPONSE_BYTES xLOGIC_NET_RESPONSE_BYTES
#define xSDK_xLOGIC_NET_MAX_READ       xLOGIC_NET_MAX_READ_BYTES
#define xSDK_xLOGIC_NET_MAX_BLOCK      (xLOGIC_NET_BLOCK_HEADER_BYTES + xLOGIC_NET_BLOCK_MAX_ENCODED)
#define xSDK_xLOGIC_NET_SAMPLE_RATE    500000000U
#define xSDK_xLOGIC_NET_QUANTUM        192U
#define xSDK_xLOGIC_NET_CAPACITY       2147483520ULL
#define xSDK_xLOGIC_NET_FEATURES       xLOGIC_NET_FEATURE_FLAGS
#define xSDK_xLOGIC_NET_CAPTURING      xLOGIC_NET_FLAG_CAPTURING
#define xSDK_xLOGIC_NET_READY          xLOGIC_NET_FLAG_READY
#define xSDK_xLOGIC_NET_FAILED         xLOGIC_NET_FLAG_FAILED
#define xSDK_xLOGIC_NET_KNOWN_FLAGS    xLOGIC_NET_KNOWN_FLAGS
#define xSDK_xLOGIC_NET_TIMEOUT_US     (10 * G_TIME_SPAN_SECOND)
#define xSDK_xLOGIC_NET_POLL_US        (100 * G_TIME_SPAN_MILLISECOND)
#define xSDK_xLOGIC_NET_CREDITS        8U
#define xSDK_xLOGIC_NET_MAX_CREDITS    xLOGIC_NET_BLOCK_WINDOW_CREDITS
#define xSDK_xLOGIC_NET_PUMP_STEPS     32U

enum xsdk_xlogic_network_opcode
{
    xSDK_xLOGIC_NET_CAPS = xLOGIC_NET_OP_CAPS,
    xSDK_xLOGIC_NET_START = xLOGIC_NET_OP_START,
    xSDK_xLOGIC_NET_STATUS = xLOGIC_NET_OP_STATUS,
    xSDK_xLOGIC_NET_READ = xLOGIC_NET_OP_READ,
    xSDK_xLOGIC_NET_STOP = xLOGIC_NET_OP_STOP,
    xSDK_xLOGIC_NET_RELEASE = xLOGIC_NET_OP_RELEASE,
    xSDK_xLOGIC_NET_NEGOTIATE = xLOGIC_NET_OP_NEGOTIATE,
    xSDK_xLOGIC_NET_READ_BLOCK = xLOGIC_NET_OP_READ_BLOCK,
};

struct xsdk_xlogic_network_metadata
{
    uint64_t capture_id;
    uint64_t sample_rate;
    uint64_t sample_count;
    uint64_t timestamp_ticks;
    uint64_t timestamp_hz;
};

/* Per-device, module-managed fixed queue. Payload storage remains one bounded block
 * response and one decoded block; 256 pending slots retain only frames and send
 * offsets (12 KiB on the supported 64-bit host), independently of capture depth. */
struct xsdk_xlogic_network_request
{
    uint8_t frame[xSDK_xLOGIC_NET_REQUEST_BYTES];
    size_t sent;
};

enum xsdk_xesk_phase
{
    xSDK_xESK_PHASE_CLAIM,
    xSDK_xESK_PHASE_START,
    xSDK_xESK_PHASE_STATUS,
    xSDK_xESK_PHASE_READ,
    xSDK_xESK_PHASE_FINISH,
    xSDK_xESK_PHASE_RELEASE,
    xSDK_xESK_PHASE_REAP,
    xSDK_xESK_PHASE_RENEW,
};

struct xsdk_xlogic_network
{
    struct sr_tcp_dev_inst *peer;
    xSDK_xESK_Network_Pump_t *esk;
    xESK_Device_t esk_device;
    enum xsdk_xesk_phase esk_phase;
    enum xsdk_xesk_phase esk_resume;
    gint64 esk_renew_at;
    gint64 esk_capture_deadline;
    gint64 esk_retire_deadline;
    struct xsdk_xlogic_network_metadata capture;
    uint8_t *response;
    uint8_t *decoded;
    struct xsdk_xlogic_block_config block;
    gboolean use_blocks;
    size_t selected_source;
    gboolean negotiated;
    struct xsdk_xlogic_auto automatic;
    uint64_t framing_wire_bytes;
    gint64 retrieval_started;
    gint64 first_publication;
    struct xsdk_xlogic_network_request requests[xSDK_xLOGIC_NET_MAX_CREDITS];
    size_t head;
    size_t count;
    size_t received;
    size_t expected;
    uint64_t maximum_samples;
    uint64_t offset;
    uint64_t issued_offset;
    uint64_t rounded_samples;
    uint32_t maximum_read;
    uint32_t requested_credits;
    uint32_t accepted_credits;
    gboolean window_explicit;
    uint32_t credits;
    uint32_t request_id;
    gint64 poll_after;
    gint64 active_deadline;
    gboolean header_valid;
    gboolean stop_requested;
    gboolean source_added;
    gboolean progressed;
    void *source_session;
#ifdef _WIN32
    WSAEVENT read_event;
    gboolean event_associated;
#endif
};

static struct xsdk_xlogic_network_request *request_head(struct xsdk_xlogic_network *network)
{
    return &network->requests[network->head];
}

static void response_reset(struct xsdk_xlogic_network *network)
{
    network->received = 0U;
    network->expected = xSDK_xLOGIC_NET_RESPONSE_BYTES;
    network->header_valid = FALSE;
}

static void request_pop(struct xsdk_xlogic_network *network)
{
    network->head = (network->head + 1U) % xSDK_xLOGIC_NET_MAX_CREDITS;
    network->count--;
    network->active_deadline = network->count != 0U ? g_get_monotonic_time() + xSDK_xLOGIC_NET_TIMEOUT_US : 0;
    response_reset(network);
}

static void queue_reset(struct xsdk_xlogic_network *network)
{
    network->head = 0U;
    network->count = 0U;
    network->active_deadline = 0;
    response_reset(network);
}

/* The stable source key is the network object, independent of socket/event
 * reuse. Remove the source before releasing its wait object. */
static int network_source_clear(struct xsdk_xlogic_network *network)
{
    int result = SR_OK;

    if (network->source_added)
    {
        int removed = sr_session_source_remove_internal(network->source_session, network);

        // A missing key has no live poll source; continue releasing its socket
        // association while reporting the unexpected lifecycle failure.
        if (removed != SR_OK)
        {
            sr_err("xLOGIC network session source was already absent.");
            result = SR_ERR;
        }
        network->source_added = FALSE;
        network->source_session = NULL;
    }
#ifdef _WIN32
    if (network->event_associated)
    {
        int detached = WSAEventSelect((SOCKET)network->peer->sock_fd, WSA_INVALID_EVENT, 0L);

        if (detached == SOCKET_ERROR)
        {
            int disconnected;

            sr_err("xLOGIC network event could not detach from its socket.");
            // Closing the socket cancels an association that could not detach.
            disconnected = sr_tcp_disconnect(network->peer);
            if (disconnected != SR_OK)
            {
                sr_err("xLOGIC network socket could not close after event detach failure.");
                return SR_ERR;
            }
            result = SR_ERR;
        }
        network->event_associated = FALSE;
    }
    if (network->read_event != WSA_INVALID_EVENT)
    {
        int closed = WSACloseEvent(network->read_event);

        if (!closed)
        {
            // Retain the handle so a later close/start can retry its release.
            sr_err("xLOGIC network event could not close.");
            return SR_ERR;
        }
        network->read_event = WSA_INVALID_EVENT;
    }
#endif
    return result;
}

static uint32_t response_state(const uint8_t *response)
{
    return RL32(response + 20U) & (xSDK_xLOGIC_NET_CAPTURING | xSDK_xLOGIC_NET_READY | xSDK_xLOGIC_NET_FAILED);
}

static int response_header_check(struct xsdk_xlogic_network *network)
{
    const uint8_t *response = network->response;
    const uint32_t flags = RL32(response + 20U);
    const uint32_t state = response_state(response);
    const uint32_t length = RL32(response + 56U);

    if (RL32(response) != xSDK_xLOGIC_NET_MAGIC || RL16(response + 4U) != xSDK_xLOGIC_NET_VERSION ||
        RL16(response + 6U) != xSDK_xLOGIC_NET_RESPONSE_BYTES || RL32(response + 8U) != RL32(request_head(network)->frame + 8U) ||
        RL32(response + 12U) != RL32(request_head(network)->frame + 12U) || RL32(response + 16U) > xLOGIC_NET_STATUS_EXPIRED ||
        (flags & ~xSDK_xLOGIC_NET_KNOWN_FLAGS) != 0U || (flags & xSDK_xLOGIC_NET_FEATURES) != xSDK_xLOGIC_NET_FEATURES ||
        (state != 0U && state != xSDK_xLOGIC_NET_CAPTURING && state != xSDK_xLOGIC_NET_READY && state != xSDK_xLOGIC_NET_FAILED) ||
        length > (RL32(request_head(network)->frame + 8U) == xSDK_xLOGIC_NET_READ_BLOCK
                      ? xLOGIC_NET_BLOCK_HEADER_BYTES + network->block.encoded_budget
                      : xSDK_xLOGIC_NET_MAX_READ))
    {
        sr_err("xLOGIC network response header is incompatible or out of sequence.");
        return SR_ERR;
    }
    network->expected = xSDK_xLOGIC_NET_RESPONSE_BYTES + length;
    network->header_valid = TRUE;
    return SR_OK;
}

static int response_payload_check(const struct xsdk_xlogic_network *network)
{
    const uint32_t length = RL32(network->response + 56U);
    const uint32_t expected_crc = RL32(network->response + 60U);
    const uint32_t crc = (uint32_t)crc32(0L, network->response + xSDK_xLOGIC_NET_RESPONSE_BYTES, length);

    if (crc != expected_crc)
    {
        sr_err("xLOGIC network payload CRC32 mismatch.");
        return SR_ERR;
    }
    return SR_OK;
}

static int response_check(struct xsdk_xlogic_network *network)
{
    const uint32_t status = RL32(network->response + 16U);
    int result = response_payload_check(network);

    if (result != SR_OK)
    {
        return result;
    }
    if (status != 0U)
    {
        sr_err("xLOGIC network command %u failed with status %u.", RL32(request_head(network)->frame + 8U), status);
        return SR_ERR;
    }
    return SR_OK;
}

static int network_connect(struct xsdk_xlogic_network *network)
{
    int result = sr_tcp_connect(network->peer);

    if (result != SR_OK)
    {
        return result;
    }
#ifdef _WIN32
    u_long nonblocking = 1U;
    result = ioctlsocket((SOCKET)network->peer->sock_fd, FIONBIO, &nonblocking);
#else
    const int flags = fcntl(network->peer->sock_fd, F_GETFL, 0);
    result = flags < 0 ? -1 : fcntl(network->peer->sock_fd, F_SETFL, flags | O_NONBLOCK);
#endif
    if (result != 0)
    {
        sr_err("xLOGIC network socket could not enter nonblocking mode.");
        sr_tcp_disconnect(network->peer);
        return SR_ERR;
    }
    network->negotiated = FALSE;
    network->credits = xSDK_xLOGIC_NET_CREDITS;
    network->accepted_credits = xSDK_xLOGIC_NET_CREDITS;
    return SR_OK;
}

static gboolean socket_would_block(void)
{
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static int esk_write(void *user, uint8_t *data, size_t bytes)
{
    struct xsdk_xlogic_network *network = user;
    const int count = sr_tcp_write_bytes(network->peer, data, bytes);
    if (count < 0 && socket_would_block())
    {
        return 0;
    }
    network->progressed |= count > 0;
    return count > 0 ? count : -1;
}

static int esk_read(void *user, uint8_t *data, size_t bytes)
{
    struct xsdk_xlogic_network *network = user;
    const int count = sr_tcp_read_bytes(network->peer, data, bytes, FALSE);
    if (count < 0 && socket_would_block())
    {
        return 0;
    }
    network->progressed |= count > 0;
    return count > 0 ? count : -1;
}

/* Scanning and opening are already synchronous libsigrok operations. Capture
 * itself uses the session source; no socket wait is added to its callback. */
static int esk_exchange(struct xsdk_xlogic_network *network, const xESK_Control_Message_t *request)
{
    const gint64 deadline = g_get_monotonic_time() + xSDK_xLOGIC_NET_TIMEOUT_US;
    if (xSDK_xESK_Network_Queue(network->esk, request) != xRETURN_OK)
    {
        return SR_ERR;
    }
    while (g_get_monotonic_time() < deadline)
    {
        const xRETURN_t status = xSDK_xESK_Network_Pump(network->esk, network, esk_write, esk_read);
        if (status == xRETURN_OK)
        {
            return SR_OK;
        }
        if (status != xRETURN_xERR_xESK_BUSY)
        {
            return SR_ERR;
        }
        g_usleep(1000U);
    }
    xSDK_xESK_Network_Poison(&network->esk->session);
    return SR_ERR;
}

static int esk_describe(struct xsdk_xlogic_network *network)
{
    xESK_Control_Message_t request = {.opcode = xESK_CONTROL_DESCRIBE, .descriptor_kind = xESK_RECORD_DEVICE};
    const xESK_Device_t previous = network->esk_device;
    memset(network->esk, 0, sizeof(*network->esk));
    if (esk_exchange(network, &request) != SR_OK || network->esk->reply.status != xESK_CONTROL_OK)
    {
        return SR_ERR;
    }
    const xESK_Device_t *device = &network->esk->session.device;
    if (previous.boot_id != 0U &&
        (previous.boot_id != device->boot_id || memcmp(previous.device_id, device->device_id, sizeof(previous.device_id)) != 0))
    {
        sr_err("xESK device identity changed; scan the device again.");
        return SR_ERR;
    }
    request.descriptor_kind = xESK_RECORD_SERVICE;
    for (uint32_t index = 0U; index < 32U; ++index)
    {
        request.index = index;
        if (esk_exchange(network, &request) != SR_OK || network->esk->reply.status != xESK_CONTROL_OK)
        {
            return SR_ERR;
        }
        const xESK_Service_t *service = &network->esk->session.logic_service;
        if (network->esk->session.has_finite_logic && (service->flags & xESK_SERVICE_LOGIC_RATE_PROFILE_1) != 0U)
        {
            network->esk_device = *device;
            network->maximum_samples = MIN(service->buffer_bytes, xSDK_xLOGIC_NET_CAPACITY);
            return SR_OK;
        }
    }
    return SR_ERR;
}

static int send_request(struct xsdk_xlogic_network *network, uint32_t opcode, uint64_t identity, uint64_t argument0, uint64_t argument1)
{
    struct xsdk_xlogic_network_request *entry;
    uint8_t *request;

    if (network->count >= network->credits)
    {
        return SR_ERR;
    }
    entry = &network->requests[(network->head + network->count) % xSDK_xLOGIC_NET_MAX_CREDITS];
    request = entry->frame;
    network->request_id = network->request_id == UINT32_MAX ? 1U : network->request_id + 1U;
    memset(request, 0, xSDK_xLOGIC_NET_REQUEST_BYTES);
    WL32(request, xSDK_xLOGIC_NET_MAGIC);
    WL16(request + 4U, xSDK_xLOGIC_NET_VERSION);
    WL16(request + 6U, xSDK_xLOGIC_NET_REQUEST_BYTES);
    WL32(request + 8U, opcode);
    WL32(request + 12U, network->request_id);
    WL64(request + 16U, identity);
    WL64(request + 24U, argument0);
    WL64(request + 32U, argument1);
    entry->sent = 0U;
    if (network->count == 0U)
    {
        network->active_deadline = g_get_monotonic_time() + xSDK_xLOGIC_NET_TIMEOUT_US;
    }
    network->count++;
    return SR_OK;
}

/* One send attempt, preserving FIFO order even when a frame is fragmented. */
static int transmit_request(struct xsdk_xlogic_network *network)
{
    size_t index;

    for (index = 0U; index < network->count; index++)
    {
        struct xsdk_xlogic_network_request *entry = &network->requests[(network->head + index) % xSDK_xLOGIC_NET_MAX_CREDITS];
        size_t remaining = xSDK_xLOGIC_NET_REQUEST_BYTES - entry->sent;
        int result;

        if (remaining == 0U)
        {
            continue;
        }
        result = sr_tcp_write_bytes(network->peer, entry->frame + entry->sent, remaining);
        if (result < 0 && socket_would_block())
        {
            return SR_OK;
        }
        if (result <= 0 || (size_t)result > remaining)
        {
            sr_err("xLOGIC network request could not be sent.");
            return SR_ERR;
        }
        entry->sent += (size_t)result;
        network->framing_wire_bytes += (size_t)result;
        network->progressed = TRUE;
        break;
    }
    return SR_OK;
}

/* At most one nonblocking send and read per step. Only the FIFO head owns the
 * parser and its absolute deadline, which includes an unsent head. Queued tails
 * receive their deadline when they become head; fragments never renew it. */
static int receive_response(struct xsdk_xlogic_network *network)
{
    size_t remaining;
    int result;

    network->progressed = FALSE;
    if (g_get_monotonic_time() >= network->active_deadline)
    {
        sr_err("xLOGIC network response timed out.");
        return SR_ERR;
    }
    result = transmit_request(network);
    if (result != SR_OK)
    {
        return result;
    }
    if (request_head(network)->sent != xSDK_xLOGIC_NET_REQUEST_BYTES)
    {
        return SR_ERR_NA;
    }
    remaining = network->expected - network->received;
    /* The socket is nonblocking. Bypass the pinned Windows helper's unavailable
     * HAVE_POLL/HAVE_SELECT readiness check; recv reports would-block and EOF. */
    result = sr_tcp_read_bytes(network->peer, network->response + network->received, remaining, FALSE);
    if (result < 0 && socket_would_block())
    {
        return SR_ERR_NA;
    }
    if (result <= 0 || (size_t)result > remaining)
    {
        sr_err("xLOGIC network connection ended before its response completed.");
        return SR_ERR;
    }
    network->progressed = TRUE;
    network->received += (size_t)result;
    network->framing_wire_bytes += (size_t)result;
    if (!network->header_valid && network->received == xSDK_xLOGIC_NET_RESPONSE_BYTES)
    {
        if (response_header_check(network) != SR_OK)
        {
            return SR_ERR;
        }
    }
    return network->received == network->expected ? SR_OK : SR_ERR_NA;
}

static gboolean capabilities_geometry_valid(const uint8_t *response)
{
    const uint8_t *payload = response + xSDK_xLOGIC_NET_RESPONSE_BYTES;
    const uint64_t maximum = RL64(response + 40U);
    const uint32_t maximum_read = RL32(payload);

    return RL64(response + 32U) == xSDK_xLOGIC_NET_SAMPLE_RATE && RL64(response + 48U) == xSDK_xLOGIC_NET_QUANTUM && maximum != 0U &&
           maximum <= xSDK_xLOGIC_NET_CAPACITY && maximum % xSDK_xLOGIC_NET_QUANTUM == 0U && maximum_read != 0U &&
           maximum_read <= xSDK_xLOGIC_NET_MAX_READ && RL32(payload + 4U) == 255U && RL32(payload + 8U) == 0U && RL32(payload + 12U) == 0U;
}

static int exchange_caps(struct xsdk_xlogic_network *network)
{
    const uint8_t *payload;
    uint64_t maximum;
    uint32_t maximum_read;
    int result;

    queue_reset(network);
    result = send_request(network, xSDK_xLOGIC_NET_CAPS, 0U, 0U, 0U);
    if (result != SR_OK)
    {
        return result;
    }
    do
    {
        result = receive_response(network);
        if (result == SR_ERR_NA)
        {
            g_usleep(1000U);
        }
    } while (result == SR_ERR_NA);
    if (result != SR_OK)
    {
        return result;
    }
    result = response_check(network);
    if (result != SR_OK)
    {
        return result;
    }
    if (RL32(network->response + 56U) != xLOGIC_NET_CAPS_BYTES)
    {
        sr_err("xLOGIC network capabilities payload has an invalid length.");
        return SR_ERR;
    }
    payload = network->response + xSDK_xLOGIC_NET_RESPONSE_BYTES;
    maximum = RL64(network->response + 40U);
    maximum_read = RL32(payload);
    if (!capabilities_geometry_valid(network->response))
    {
        sr_err("xLOGIC network capabilities do not describe finite 500 MS/s raw eight-channel capture.");
        return SR_ERR;
    }
    network->maximum_samples = maximum;
    network->maximum_read = maximum_read;
    queue_reset(network);
    return SR_OK;
}

static gboolean negotiation_payload_valid(const struct xsdk_xlogic_network *network, uint8_t mask, uint32_t credits)
{
    const uint8_t *payload = network->response + xSDK_xLOGIC_NET_RESPONSE_BYTES;

    return RL32(payload) == xLOGIC_NET_BLOCK_VERSION && RL32(payload + 4U) == xLOGIC_NET_BLOCK_MODE_MASK &&
           RL32(payload + 8U) == network->block.mode && RL32(payload + 12U) == mask && RL32(payload + 16U) == 0U &&
           RL32(payload + 20U) == network->block.decoded_maximum && RL32(payload + 24U) == network->block.encoded_budget &&
           RL32(payload + 28U) == network->block.fallback && RL32(payload + 32U) == credits && RL32(payload + 36U) == xLOGIC_NET_LEASE_MS;
}

static int exchange_negotiation(struct xsdk_xlogic_network *network, uint8_t mask, uint32_t requested)
{
    const uint32_t encoded_credits = requested == xSDK_xLOGIC_NET_CREDITS ? 0U : requested;
    const uint64_t argument = (uint64_t)network->block.mode | ((uint64_t)mask << 8U) | ((uint64_t)network->block.fallback << 24U) |
                              ((uint64_t)network->block.decoded_maximum << 32U) | ((uint64_t)encoded_credits << 48U);
    uint32_t status;
    int result;

    queue_reset(network);
    result = send_request(network, xSDK_xLOGIC_NET_NEGOTIATE, 0U, argument, network->block.encoded_budget);
    if (result != SR_OK)
    {
        return result;
    }
    do
    {
        result = receive_response(network);
        if (result == SR_ERR_NA)
        {
            g_usleep(1000U);
        }
    } while (result == SR_ERR_NA);
    if (result != SR_OK)
    {
        sr_err("xLOGIC forced retrieval extension negotiation failed before capture.");
        return result;
    }
    result = response_payload_check(network);
    if (result != SR_OK)
    {
        return result;
    }
    status = RL32(network->response + 16U);
    if (status != xLOGIC_NET_STATUS_OK)
    {
        if (RL32(network->response + 56U) != 0U)
        {
            return SR_ERR;
        }
        return status == xLOGIC_NET_STATUS_RANGE || status == xLOGIC_NET_STATUS_UNSUPPORTED ? SR_ERR_NA : SR_ERR;
    }
    if (RL32(network->response + 56U) != xLOGIC_NET_NEGOTIATION_BYTES || !negotiation_payload_valid(network, mask, requested))
    {
        sr_err("xLOGIC retrieval negotiation response changed the selected contract.");
        return SR_ERR;
    }
    return SR_OK;
}

static int negotiate_blocks(struct xsdk_xlogic_network *network, uint8_t mask)
{
    uint32_t credits = network->requested_credits;
    int result;

    network->negotiated = FALSE;
    network->credits = xSDK_xLOGIC_NET_CREDITS;
    network->block.decoded_maximum = xLOGIC_NET_BLOCK_MAX_DECODED;
    network->block.encoded_budget = xLOGIC_NET_BLOCK_MAX_ENCODED;
    result = exchange_negotiation(network, mask, credits);
    if (result != SR_OK)
    {
        sr_err("xLOGIC retrieval negotiation failed; capture was not started.");
        return SR_ERR;
    }
    network->block.mask = mask;
    network->accepted_credits = credits;
    network->credits = MIN(network->requested_credits, credits);
    network->negotiated = TRUE;
    queue_reset(network);
    return SR_OK;
}

static void metadata_read(const uint8_t *response, struct xsdk_xlogic_network_metadata *metadata)
{
    metadata->capture_id = RL64(response + 24U);
    metadata->sample_rate = RL64(response + 32U);
    metadata->sample_count = RL64(response + 40U);
    metadata->timestamp_ticks = RL64(response + 64U);
    metadata->timestamp_hz = RL64(response + 72U);
}

static gboolean capture_valid(const struct xsdk_xlogic_network *network)
{
    const uint8_t *response = network->response;
    const uint32_t state = response_state(response);

    return RL64(response + 24U) != 0U && (network->capture.capture_id == 0U || RL64(response + 24U) == network->capture.capture_id) &&
           RL64(response + 32U) == xSDK_xLOGIC_NET_SAMPLE_RATE && RL64(response + 40U) == network->rounded_samples &&
           (state == xSDK_xLOGIC_NET_CAPTURING || state == xSDK_xLOGIC_NET_READY);
}

static gboolean response_metadata_matches(const struct xsdk_xlogic_network *network)
{
    struct xsdk_xlogic_network_metadata current;

    metadata_read(network->response, &current);
    return current.capture_id == network->capture.capture_id && current.sample_rate == network->capture.sample_rate &&
           current.sample_count == network->capture.sample_count && current.timestamp_ticks == network->capture.timestamp_ticks &&
           current.timestamp_hz == network->capture.timestamp_hz;
}

static void send_rate(struct sr_dev_inst *sdi, uint64_t samplerate)
{
    struct sr_config *config = sr_config_new(SR_CONF_SAMPLERATE, g_variant_new_uint64(samplerate));
    struct sr_datafeed_meta meta = {.config = g_slist_append(NULL, config)};
    const struct sr_datafeed_packet packet = {.type = SR_DF_META, .payload = &meta};

    sr_session_send(sdi, &packet);
    g_slist_free(meta.config);
    sr_config_free(config);
}

static void network_finish(struct sr_dev_inst *sdi, gboolean failed)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;

    if (network_source_clear(network) != SR_OK)
    {
        failed = TRUE;
    }
    devc->failed = failed;
    if (network->automatic.enabled)
    {
        size_t index;
        size_t trials = 0U;
        gint64 probe_us = 0;
        for (index = 0U; index < xsdk_xlogic_auto_count(&network->automatic); index++)
        {
            trials += network->automatic.outcomes[index] == 1U ? 1U : 0U;
            probe_us += network->automatic.durations[index];
            sr_info("xLOGIC auto candidate %s: outcome %u, verified trial %" G_GINT64_FORMAT " us.",
                    xsdk_xlogic_auto_name(&network->automatic, index), network->automatic.outcomes[index],
                    network->automatic.durations[index]);
        }
        sr_info("xLOGIC auto: selected=%s trials=%zu probe_wire_bytes=%" G_GUINT64_FORMAT " negotiation_wire_bytes=%" G_GUINT64_FORMAT
                " probe_us=%" G_GINT64_FORMAT " selection_us=%" G_GINT64_FORMAT
                " budget_skipped=%u eligible=%u requested_credits=%u effective_credits=%u fallback_blocks=%" G_GUINT64_FORMAT
                " stopped=%u published_samples=%" G_GUINT64_FORMAT ".",
                xsdk_xlogic_auto_name(&network->automatic, network->automatic.selected), trials, network->automatic.probe_wire_bytes,
                network->automatic.negotiation_wire_bytes, probe_us, network->automatic.selection_us, network->automatic.budget_skipped,
                network->automatic.eligible, network->requested_credits, network->credits, network->automatic.fallback_blocks,
                network->stop_requested, devc->sent_samples);
    }
    sr_info("xLOGIC retrieval credits: chosen %u, accepted %u, effective %u.",
            network->use_blocks || network->window_explicit ? network->requested_credits : xSDK_xLOGIC_NET_CREDITS,
            network->accepted_credits, network->credits);
    sr_info("xLOGIC retrieval: %" G_GUINT64_FORMAT " framing-inclusive bytes, first validated publication %" G_GINT64_FORMAT
            " us, complete %" G_GINT64_FORMAT " us, failed %u.",
            network->framing_wire_bytes, network->first_publication < 0 ? -1 : network->first_publication - network->retrieval_started,
            network->retrieval_started < 0 ? -1 : g_get_monotonic_time() - network->retrieval_started, failed);
    if (failed)
    {
        sr_tcp_disconnect(network->peer);
    }
    queue_reset(network);
    devc->acquisition_state = xSDK_xLOGIC_ACQ_IDLE;
    if (!devc->end_sent)
    {
        devc->end_sent = TRUE;
        std_session_send_df_end(sdi);
    }
}

static int auto_begin_trial(struct xsdk_xlogic_network *network);
static int auto_negotiate(struct xsdk_xlogic_network *network, uint8_t mask);

static int request_download(struct xsdk_xlogic_network *network)
{
    const uint64_t end =
        network->automatic.enabled && network->automatic.eligible && network->automatic.phase == xSDK_xLOGIC_AUTO_PREFIX_PHASE
            ? xSDK_xLOGIC_AUTO_PREFIX
            : network->capture.sample_count;
    while (!network->stop_requested && network->count < network->credits && network->issued_offset < end)
    {
        const uint32_t length = (uint32_t)MIN((uint64_t)(network->use_blocks ? network->block.decoded_maximum : network->maximum_read),
                                              end - network->issued_offset);
        int result = send_request(network, network->use_blocks ? xSDK_xLOGIC_NET_READ_BLOCK : xSDK_xLOGIC_NET_READ,
                                  network->capture.capture_id, network->issued_offset, length);

        if (result != SR_OK)
        {
            return result;
        }
        network->issued_offset += length;
    }
    return SR_OK;
}

/* Controls are queued only after all already-issued responses were validated.
 * On transport/protocol failure network_finish disconnects instead: the server
 * owns active cancellation or the bounded READY lease, never a stray control. */
static int finish_download(struct xsdk_xlogic_network *network)
{
    if (network->count != 0U)
    {
        return SR_OK;
    }
    return send_request(network, network->stop_requested ? xSDK_xLOGIC_NET_STOP : xSDK_xLOGIC_NET_RELEASE, network->capture.capture_id, 0U,
                        0U);
}

static int decode_download(struct xsdk_xlogic_network *network, uint32_t count)
{
    const uint32_t bytes = RL32(network->response + 56U);

    if (!network->use_blocks)
    {
        return bytes == count ? SR_OK : SR_ERR;
    }
    if (!network->negotiated)
    {
        return SR_ERR;
    }
    if (xsdk_xlogic_block_decode(&network->block, network->response + xSDK_xLOGIC_NET_RESPONSE_BYTES, bytes, count, network->decoded,
                                 xLOGIC_NET_BLOCK_MAX_DECODED) != 0)
    {
        sr_err("xLOGIC retrieval block descriptor, codec or raw CRC32 is invalid.");
        return SR_ERR;
    }
    if (network->automatic.enabled && RL32(network->response + xSDK_xLOGIC_NET_RESPONSE_BYTES + 28U) == 1U)
    {
        network->automatic.fallback_blocks++;
    }
    return SR_OK;
}

static gboolean download_metadata_valid(const struct xsdk_xlogic_network *network, uint32_t count)
{
    return response_state(network->response) == xSDK_xLOGIC_NET_READY && response_metadata_matches(network) &&
           RL64(network->response + 48U) == network->offset && RL64(network->requests[network->head].frame + 24U) == network->offset &&
           count != 0U && network->offset <= network->capture.sample_count && count <= network->capture.sample_count - network->offset;
}

/* A synchronous frontend sink can stall this callback without consuming any
 * network timeout budget. Only the active head's absolute deadline is suspended. */
static int publish_download(struct sr_dev_inst *sdi, const struct sr_datafeed_packet *packet)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    const gint64 started = g_get_monotonic_time();
    gint64 elapsed;
    int result = sr_session_send(sdi, packet);

    elapsed = MAX((gint64)0, g_get_monotonic_time() - started);
    if (network->count != 0U)
    {
        network->active_deadline = network->active_deadline > G_MAXINT64 - elapsed ? G_MAXINT64 : network->active_deadline + elapsed;
    }
    return result;
}

static gboolean auto_prefix_complete(const struct xsdk_xlogic_network *network)
{
    return network->automatic.enabled && network->automatic.eligible && network->offset == xSDK_xLOGIC_AUTO_PREFIX &&
           network->count == 0U && network->automatic.phase == xSDK_xLOGIC_AUTO_PREFIX_PHASE;
}

static int accept_download(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    const uint32_t length = RL32(request_head(network)->frame + 32U);
    uint8_t *samples = network->response + xSDK_xLOGIC_NET_RESPONSE_BYTES;
    const size_t accepted = (size_t)MIN((uint64_t)length, devc->limit_samples - devc->sent_samples);
    const struct sr_datafeed_logic logic = {
        .length = accepted,
        .unitsize = 1U,
        .data = network->use_blocks ? network->decoded : samples,
    };
    const struct sr_datafeed_packet packet = {.type = SR_DF_LOGIC, .payload = &logic};

    if (!download_metadata_valid(network, length))
    {
        sr_err("xLOGIC network download changed capture metadata, offset or payload length.");
        return SR_ERR;
    }
    if (decode_download(network, length) != SR_OK)
    {
        return SR_ERR;
    }
    if (!network->stop_requested && accepted != 0U)
    {
        if (network->first_publication < 0)
        {
            network->first_publication = g_get_monotonic_time();
        }
        if (publish_download(sdi, &packet) != SR_OK)
        {
            return SR_ERR;
        }
        if (devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
        {
            return SR_OK;
        }
        devc->sent_samples += accepted;
    }
    network->offset += length;
    request_pop(network);
    if (network->stop_requested || devc->sent_samples == devc->limit_samples)
    {
        return finish_download(network);
    }
    if (auto_prefix_complete(network))
    {
        network->automatic.budget_started = g_get_monotonic_time();
        return auto_begin_trial(network);
    }
    return request_download(network);
}

static void auto_configure(struct xsdk_xlogic_network *network)
{
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    network->use_blocks = !xsdk_xlogic_auto_legacy(automatic, automatic->candidate);
    network->block.mode = xsdk_xlogic_auto_mode(automatic, automatic->candidate);
    network->block.fallback = network->block.mode != 0U ? 1U : 0U;
    network->block.decoded_maximum = xLOGIC_NET_BLOCK_MAX_DECODED;
    network->block.encoded_budget = xLOGIC_NET_BLOCK_MAX_ENCODED;
    network->negotiated = FALSE;
    network->accepted_credits = xSDK_xLOGIC_NET_CREDITS;
    network->credits = MIN(network->requested_credits, xSDK_xLOGIC_NET_CREDITS);
}

static int auto_negotiate(struct xsdk_xlogic_network *network, uint8_t mask)
{
    const uint32_t credits = network->automatic.negotiation_credits;
    const uint64_t encoded = credits == xSDK_xLOGIC_NET_CREDITS ? 0U : credits;
    const uint64_t argument = (uint64_t)network->block.mode | ((uint64_t)mask << 8U) | ((uint64_t)network->block.fallback << 24U) |
                              ((uint64_t)network->block.decoded_maximum << 32U) | (encoded << 48U);
    return send_request(network, xSDK_xLOGIC_NET_NEGOTIATE, 0U, argument, network->block.encoded_budget);
}

static int auto_request_probes(struct xsdk_xlogic_network *network)
{
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    while (!network->stop_requested && network->count < network->credits && automatic->issued < xSDK_xLOGIC_AUTO_TRIAL_BYTES)
    {
        const uint32_t length = network->use_blocks ? network->block.decoded_maximum : network->maximum_read;
        const uint64_t offset = xsdk_xlogic_auto_offset(automatic, automatic->issued);
        const uint32_t bounded = MIN(length, xSDK_xLOGIC_AUTO_PREFIX - (uint32_t)(automatic->issued % xSDK_xLOGIC_AUTO_PREFIX));
        int result = send_request(network, network->use_blocks ? xSDK_xLOGIC_NET_READ_BLOCK : xSDK_xLOGIC_NET_READ,
                                  network->capture.capture_id, offset, bounded);
        if (result != SR_OK)
        {
            return result;
        }
        automatic->issued += bounded;
    }
    return SR_OK;
}

static int auto_begin_trial(struct xsdk_xlogic_network *network)
{
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    automatic->phase = xSDK_xLOGIC_AUTO_TRIAL;
    automatic->issued = 0U;
    automatic->received = 0U;
    automatic->trial_started = g_get_monotonic_time();
    return auto_request_probes(network);
}

static int auto_next_candidate(struct xsdk_xlogic_network *network)
{
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    automatic->candidate++;
    automatic->budget_skipped = automatic->candidate < xsdk_xlogic_auto_count(automatic) &&
                                g_get_monotonic_time() - automatic->budget_started >= xSDK_xLOGIC_AUTO_BUDGET_US;
    if (automatic->candidate == xsdk_xlogic_auto_count(automatic) ||
        g_get_monotonic_time() - automatic->budget_started >= xSDK_xLOGIC_AUTO_BUDGET_US)
    {
        automatic->selected = xsdk_xlogic_auto_select(automatic);
        automatic->candidate = automatic->selected;
        automatic->phase = xSDK_xLOGIC_AUTO_FINAL;
    }
    else
    {
        automatic->phase = xSDK_xLOGIC_AUTO_NEGOTIATE;
    }
    auto_configure(network);
    if (!network->use_blocks)
    {
        automatic->selection_us = g_get_monotonic_time() - automatic->budget_started;
        automatic->phase = xSDK_xLOGIC_AUTO_DOWNLOAD;
        return request_download(network);
    }
    automatic->negotiation_credits = network->requested_credits;
    return auto_negotiate(network, (uint8_t)network->block.mask);
}

static int auto_accept_probe(struct xsdk_xlogic_network *network)
{
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    const uint32_t length = RL32(request_head(network)->frame + 32U);
    const uint64_t offset = RL64(request_head(network)->frame + 24U);
    if (response_state(network->response) != xSDK_xLOGIC_NET_READY || !response_metadata_matches(network) ||
        RL64(network->response + 48U) != offset || offset != xsdk_xlogic_auto_offset(automatic, automatic->received))
    {
        return SR_ERR;
    }
    if (decode_download(network, length) != SR_OK)
    {
        return SR_ERR;
    }
    automatic->probe_wire_bytes += xSDK_xLOGIC_NET_REQUEST_BYTES + network->expected;
    automatic->received += length;
    request_pop(network);
    if (network->stop_requested)
    {
        return finish_download(network);
    }
    if (automatic->received == xSDK_xLOGIC_AUTO_TRIAL_BYTES)
    {
        automatic->durations[automatic->candidate] = MAX((gint64)1, g_get_monotonic_time() - automatic->trial_started);
        automatic->outcomes[automatic->candidate] = 1U;
        return auto_next_candidate(network);
    }
    return auto_request_probes(network);
}

static int auto_rejected_negotiation(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    request_pop(network);
    if (network->stop_requested)
    {
        if (automatic->phase == xSDK_xLOGIC_AUTO_INITIAL)
        {
            network_finish(sdi, FALSE);
            return SR_OK;
        }
        return finish_download(network);
    }
    if (automatic->phase != xSDK_xLOGIC_AUTO_NEGOTIATE)
    {
        return SR_ERR;
    }
    automatic->outcomes[automatic->candidate] = 2U;
    return auto_next_candidate(network);
}

static gboolean auto_negotiation_metadata_valid(const struct xsdk_xlogic_network *network)
{
    return network->automatic.phase == xSDK_xLOGIC_AUTO_INITIAL ||
           (response_state(network->response) == xSDK_xLOGIC_NET_READY && response_metadata_matches(network) &&
            RL64(network->response + 48U) == 0U);
}

static int auto_accept_negotiation(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    struct xsdk_xlogic_auto *automatic = &network->automatic;
    const uint32_t status = RL32(network->response + 16U);
    const uint32_t length = RL32(network->response + 56U);
    if (response_payload_check(network) != SR_OK)
    {
        return SR_ERR;
    }
    automatic->negotiation_wire_bytes += xSDK_xLOGIC_NET_REQUEST_BYTES + network->expected;
    if (!auto_negotiation_metadata_valid(network))
    {
        sr_err("xLOGIC auto negotiation changed retained capture metadata or state.");
        return SR_ERR;
    }
    if (status == xLOGIC_NET_STATUS_RANGE || status == xLOGIC_NET_STATUS_UNSUPPORTED)
    {
        if (length != 0U)
        {
            return SR_ERR;
        }
        return auto_rejected_negotiation(sdi);
    }
    if (status != xLOGIC_NET_STATUS_OK || length != xLOGIC_NET_NEGOTIATION_BYTES ||
        !negotiation_payload_valid(network, devc->channel_mask, automatic->negotiation_credits))
    {
        sr_err("xLOGIC auto negotiation did not preserve its requested retrieval contract.");
        return SR_ERR;
    }
    network->negotiated = TRUE;
    network->accepted_credits = automatic->negotiation_credits;
    network->credits = MIN(network->requested_credits, network->accepted_credits);
    request_pop(network);
    if (network->stop_requested)
    {
        if (automatic->phase == xSDK_xLOGIC_AUTO_INITIAL)
        {
            network_finish(sdi, FALSE);
            return SR_OK;
        }
        return finish_download(network);
    }
    if (automatic->phase == xSDK_xLOGIC_AUTO_INITIAL)
    {
        automatic->phase = xSDK_xLOGIC_AUTO_PREFIX_PHASE;
        return send_request(network, xSDK_xLOGIC_NET_START, 0U, network->rounded_samples, xSDK_xLOGIC_NET_SAMPLE_RATE);
    }
    if (automatic->phase == xSDK_xLOGIC_AUTO_FINAL)
    {
        automatic->selection_us = g_get_monotonic_time() - automatic->budget_started;
        automatic->phase = xSDK_xLOGIC_AUTO_DOWNLOAD;
        return request_download(network);
    }
    return auto_begin_trial(network);
}

static int accept_capture_status(struct sr_dev_inst *sdi, uint32_t opcode)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;

    if (!capture_valid(network) || RL32(network->response + 56U) != 0U)
    {
        sr_err("xLOGIC network capture identity, state, rate or depth changed.");
        return SR_ERR;
    }
    if (opcode == xSDK_xLOGIC_NET_START)
    {
        network->capture.capture_id = RL64(network->response + 24U);
    }
    if (response_state(network->response) == xSDK_xLOGIC_NET_READY)
    {
        metadata_read(network->response, &network->capture);
        if (network->capture.timestamp_hz == 0U)
        {
            sr_err("xLOGIC network completed capture has no timestamp frequency.");
            return SR_ERR;
        }
        send_rate(sdi, network->capture.sample_rate);
        if (devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
        {
            return SR_OK;
        }
        sr_info("xLOGIC network capture %" G_GUINT64_FORMAT ": %" G_GUINT64_FORMAT " samples at %" G_GUINT64_FORMAT
                " samples/s, capture-local timestamp %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT ".",
                network->capture.capture_id, network->capture.sample_count, network->capture.sample_rate, network->capture.timestamp_ticks,
                network->capture.timestamp_hz);
        network->retrieval_started = g_get_monotonic_time();
        network->first_publication = -1;
        network->framing_wire_bytes = 0U;
        request_pop(network);
        if (network->stop_requested)
        {
            return finish_download(network);
        }
        return request_download(network);
    }
    request_pop(network);
    network->poll_after = g_get_monotonic_time() + xSDK_xLOGIC_NET_POLL_US;
    return SR_OK;
}

static int accept_completion(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    if (RL32(network->response + 56U) != 0U || response_state(network->response) != 0U ||
        RL64(network->response + 24U) != network->capture.capture_id || RL64(network->response + 32U) != xSDK_xLOGIC_NET_SAMPLE_RATE ||
        RL64(network->response + 40U) != network->rounded_samples ||
        (network->capture.sample_count != 0U && !response_metadata_matches(network)))
    {
        sr_err("xLOGIC network completion response changed capture metadata or state.");
        return SR_ERR;
    }
    network_finish(sdi, FALSE);
    return SR_OK;
}

static int consume_response(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    const uint32_t opcode = RL32(request_head(network)->frame + 8U);
    int result;

    if (network->automatic.enabled && opcode == xSDK_xLOGIC_NET_NEGOTIATE)
    {
        return auto_accept_negotiation(sdi);
    }
    result = response_check(network);

    if (result != SR_OK)
    {
        return result;
    }
    if (opcode == xSDK_xLOGIC_NET_STOP || opcode == xSDK_xLOGIC_NET_RELEASE)
    {
        return accept_completion(sdi);
    }
    if (opcode == xSDK_xLOGIC_NET_READ || opcode == xSDK_xLOGIC_NET_READ_BLOCK)
    {
        if (network->automatic.enabled && network->automatic.phase == xSDK_xLOGIC_AUTO_TRIAL)
        {
            return auto_accept_probe(network);
        }
        return accept_download(sdi);
    }
    return accept_capture_status(sdi, opcode);
}

static int advance_network(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    int result;

    if (network->count == 0U)
    {
        if (!network->stop_requested && g_get_monotonic_time() < network->poll_after)
        {
            network->progressed = FALSE;
            return SR_ERR_NA;
        }
        result = send_request(network, network->stop_requested ? xSDK_xLOGIC_NET_STOP : xSDK_xLOGIC_NET_STATUS, network->capture.capture_id,
                              0U, 0U);
        if (result != SR_OK)
        {
            return result;
        }
    }
    result = receive_response(network);
    if (result == SR_OK)
    {
        result = consume_response(sdi);
    }
    return result;
}

static int esk_queue_phase(struct sr_dev_inst *sdi, enum xsdk_xesk_phase phase)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    xSDK_xESK_Network_Session_t *session = &network->esk->session;
    xESK_Control_Message_t request = {0};
    if (phase == xSDK_xESK_PHASE_CLAIM || phase == xSDK_xESK_PHASE_RENEW)
    {
        request.opcode = phase == xSDK_xESK_PHASE_CLAIM ? xESK_CONTROL_CLAIM : xESK_CONTROL_RENEW;
        request.expiry_policy = phase == xSDK_xESK_PHASE_CLAIM ? xESK_EXPIRY_DEADLINE : 0U;
        request.lease_ticks = 60000U;
    }
    else if (phase == xSDK_xESK_PHASE_RELEASE || phase == xSDK_xESK_PHASE_REAP)
    {
        request.opcode = phase == xSDK_xESK_PHASE_RELEASE ? xESK_CONTROL_RELEASE : xESK_CONTROL_REAP;
    }
    else
    {
        request = (xESK_Control_Message_t){.kind = xESK_CONTROL_REQUEST,
                                           .opcode = xESK_CONTROL_SERVICE,
                                           .request_id = 1U,
                                           .boot_id = session->boot_id,
                                           .session_id = session->session_id,
                                           .service_id = xESK_SERVICE_LOGIC};
        xESK_Logic_Request_t command = {0};
        command.capture_id = session->has_capture ? session->capture.capture_id : 0U;
        if (phase == xSDK_xESK_PHASE_START)
        {
            command.opcode = xLOGIC_NET_OP_START;
            command.format = xESK_LOGIC_FORMAT_RAW8;
            command.channel_mask = 255U;
            command.argument0 = network->rounded_samples;
            command.argument1 = (uint32_t)devc->rates[devc->rate_index].actual;
        }
        else if (phase == xSDK_xESK_PHASE_STATUS)
        {
            command.opcode = xLOGIC_NET_OP_STATUS;
        }
        else if (phase == xSDK_xESK_PHASE_READ)
        {
            command.opcode = xLOGIC_NET_OP_READ_BLOCK;
            command.argument0 = network->offset;
            command.argument1 = (uint32_t)MIN((uint64_t)xLOGIC_NET_BLOCK_MAX_DECODED, devc->limit_samples - network->offset);
        }
        else
        {
            command.opcode = network->stop_requested ? xLOGIC_NET_OP_STOP : xLOGIC_NET_OP_RELEASE;
        }
        if (xESK_Logic_Network_Request_Encode(&command, &request) != xRETURN_OK)
        {
            return SR_ERR;
        }
    }
    if (xSDK_xESK_Network_Queue(network->esk, &request) != xRETURN_OK)
    {
        return SR_ERR;
    }
    network->esk_phase = phase;
    network->active_deadline = g_get_monotonic_time() + xSDK_xLOGIC_NET_TIMEOUT_US;
    network->progressed = TRUE;
    return SR_OK;
}

static int esk_publish_block(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    const size_t bytes = network->esk->block_bytes;
    const gboolean publish = !network->stop_requested;
    const struct sr_datafeed_logic logic = {.length = bytes, .unitsize = 1U, .data = network->esk->block};
    const struct sr_datafeed_packet packet = {.type = SR_DF_LOGIC, .payload = &logic};
    if (bytes == 0U || bytes > devc->limit_samples - network->offset)
    {
        return SR_ERR;
    }
    if (publish && sr_session_send(sdi, &packet) != SR_OK)
    {
        return SR_ERR;
    }
    if (devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
    {
        return SR_OK;
    }
    network->offset += bytes;
    devc->sent_samples += publish ? bytes : 0U;
    return SR_OK;
}

static int esk_consume_control(struct sr_dev_inst *sdi, gint64 now)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    xSDK_xESK_Network_Pump_t *pump = network->esk;
    if (network->esk_phase == xSDK_xESK_PHASE_RENEW)
    {
        network->esk_renew_at = now + 10 * G_TIME_SPAN_SECOND;
        network->esk_phase = network->esk_resume;
        return SR_OK;
    }
    if (network->esk_phase == xSDK_xESK_PHASE_CLAIM)
    {
        network->esk_renew_at = now + 10 * G_TIME_SPAN_SECOND;
        network->esk_phase = network->stop_requested ? xSDK_xESK_PHASE_RELEASE : xSDK_xESK_PHASE_START;
        return SR_OK;
    }
    if (network->esk_phase == xSDK_xESK_PHASE_REAP)
    {
        network_finish(sdi, FALSE);
        return SR_OK;
    }
    if (network->esk_phase == xSDK_xESK_PHASE_RELEASE)
    {
        if (pump->session.safe_to_reap)
        {
            network->esk_phase = xSDK_xESK_PHASE_REAP;
        }
        else
        {
            network->poll_after = now + 50 * G_TIME_SPAN_MILLISECOND;
        }
        return SR_OK;
    }
    return SR_ERR;
}

static int esk_consume_service(struct sr_dev_inst *sdi, gint64 now)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    xSDK_xESK_Network_Pump_t *pump = network->esk;
    const uint32_t status = RL32(pump->reply.payload.data + 12U);
    if (network->esk_phase == xSDK_xESK_PHASE_FINISH && status == xLOGIC_NET_STATUS_BUSY)
    {
        network->poll_after = now + 50 * G_TIME_SPAN_MILLISECOND;
        return SR_OK;
    }
    if (status != xLOGIC_NET_STATUS_OK)
    {
        return SR_ERR;
    }
    if (network->esk_phase == xSDK_xESK_PHASE_FINISH)
    {
        network->esk_phase = xSDK_xESK_PHASE_RELEASE;
    }
    else if (network->esk_phase == xSDK_xESK_PHASE_READ)
    {
        if (esk_publish_block(sdi) != SR_OK || devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
        {
            return devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE ? SR_OK : SR_ERR;
        }
        network->esk_phase =
            network->stop_requested || network->offset == devc->limit_samples ? xSDK_xESK_PHASE_FINISH : xSDK_xESK_PHASE_READ;
    }
    else if ((RL16(pump->reply.payload.data + 16U) & xLOGIC_NET_FLAG_FAILED) != 0U)
    {
        return SR_ERR;
    }
    else
    {
        network->esk_phase = pump->session.capture_ready ? xSDK_xESK_PHASE_READ : xSDK_xESK_PHASE_STATUS;
        network->poll_after = pump->session.capture_ready ? 0 : now + 100 * G_TIME_SPAN_MILLISECOND;
        if (pump->session.capture_ready)
        {
            send_rate(sdi, pump->session.capture.sample_rate);
        }
    }
    return SR_OK;
}

static int esk_consume(struct sr_dev_inst *sdi)
{
    struct xsdk_xlogic_network *network = ((struct dev_context *)sdi->priv)->network;
    if (network->esk->reply.status != xESK_CONTROL_OK)
    {
        return SR_ERR;
    }
    const gint64 now = g_get_monotonic_time();
    return network->esk->reply.opcode == xESK_CONTROL_SERVICE ? esk_consume_service(sdi, now) : esk_consume_control(sdi, now);
}

static int esk_retirement(struct xsdk_xlogic_network *network, gint64 now)
{
    xSDK_xESK_Network_Pump_t *pump = network->esk;
    if (network->esk_retire_deadline != 0 && now >= network->esk_retire_deadline)
    {
        return SR_ERR;
    }
    if (network->stop_requested && pump->session.session_id != 0U && !pump->session.retiring)
    {
        network->esk_phase = pump->session.has_capture ? xSDK_xESK_PHASE_FINISH : xSDK_xESK_PHASE_RELEASE;
    }
    if ((network->esk_phase == xSDK_xESK_PHASE_FINISH || network->esk_phase == xSDK_xESK_PHASE_RELEASE) &&
        network->esk_retire_deadline == 0)
    {
        network->esk_retire_deadline = now + 5 * G_TIME_SPAN_SECOND;
    }
    return SR_OK;
}

static int esk_schedule(struct sr_dev_inst *sdi, gint64 now)
{
    struct xsdk_xlogic_network *network = ((struct dev_context *)sdi->priv)->network;
    xSDK_xESK_Network_Pump_t *pump = network->esk;
    if (esk_retirement(network, now) != SR_OK)
    {
        return SR_ERR;
    }
    if (network->esk_phase == xSDK_xESK_PHASE_STATUS && now >= network->esk_capture_deadline)
    {
        return SR_ERR;
    }
    if (network->poll_after != 0 && now < network->poll_after)
    {
        return SR_ERR_NA;
    }
    network->poll_after = 0;
    if (pump->session.session_id != 0U && !pump->session.retiring && now >= network->esk_renew_at &&
        network->esk_phase != xSDK_xESK_PHASE_FINISH && network->esk_phase != xSDK_xESK_PHASE_RELEASE)
    {
        network->esk_resume = network->esk_phase;
        return esk_queue_phase(sdi, xSDK_xESK_PHASE_RENEW);
    }
    return esk_queue_phase(sdi, network->esk_phase);
}

static int esk_advance(struct sr_dev_inst *sdi)
{
    struct xsdk_xlogic_network *network = ((struct dev_context *)sdi->priv)->network;
    xSDK_xESK_Network_Pump_t *pump = network->esk;
    const gint64 now = g_get_monotonic_time();
    network->progressed = FALSE;
    if (!pump->active)
    {
        return esk_schedule(sdi, now);
    }
    if (now >= network->active_deadline)
    {
        xSDK_xESK_Network_Poison(&pump->session);
        return SR_ERR;
    }
    const xRETURN_t status = xSDK_xESK_Network_Pump(pump, network, esk_write, esk_read);
    return status == xRETURN_OK ? esk_consume(sdi) : (status == xRETURN_xERR_xESK_BUSY ? SR_ERR_NA : SR_ERR);
}

static gboolean network_event_source(int fd, int revents, void *cb_data)
{
    struct sr_dev_inst *sdi = cb_data;
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    size_t step;
    size_t completed = 0U;

    (void)fd;
    (void)revents;
#ifdef _WIN32
    WSANETWORKEVENTS events;
    int event_result = WSAEnumNetworkEvents((SOCKET)network->peer->sock_fd, network->read_event, &events);

    // Enum atomically resets the event before recv re-enables FD_READ. Resetting
    // after the bounded pump would lose notification for unread buffered data.
    if (event_result == SOCKET_ERROR)
    {
        sr_err("xLOGIC network event could not be enumerated.");
        network_finish(sdi, TRUE);
        return FALSE;
    }
    if (((events.lNetworkEvents & FD_READ) != 0L && events.iErrorCode[FD_READ_BIT] != 0) ||
        ((events.lNetworkEvents & FD_CLOSE) != 0L && events.iErrorCode[FD_CLOSE_BIT] != 0))
    {
        sr_err("xLOGIC network socket event reported an error.");
        network_finish(sdi, TRUE);
        return FALSE;
    }
    // A clean FD_CLOSE can accompany a complete buffered reply. Let recv drain
    // it; EOF fails only when another response is still required.
#endif
    /* Bound both callback work and completed responses; refill does not turn a
     * fast socket into an unbounded session callback. */
    for (step = 0U; step < xSDK_xLOGIC_NET_PUMP_STEPS && completed < xSDK_xLOGIC_NET_CREDITS; step++)
    {
        int result = network->esk ? esk_advance(sdi) : advance_network(sdi);

        if (result != SR_OK && result != SR_ERR_NA)
        {
            network_finish(sdi, TRUE);
            return FALSE;
        }
        if (devc->acquisition_state == xSDK_xLOGIC_ACQ_IDLE)
        {
            return FALSE;
        }
        if (result == SR_OK)
        {
            completed++;
        }
        if (!network->progressed)
        {
            break;
        }
    }
    return TRUE;
}

static int network_source_add(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    gintptr source_fd = network->peer->sock_fd;
    int events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    int result = network_source_clear(network);

    if (result != SR_OK)
    {
        return result;
    }
#ifdef _WIN32
    network->read_event = WSACreateEvent();
    if (network->read_event == WSA_INVALID_EVENT)
    {
        sr_err("xLOGIC network event could not be allocated.");
        return SR_ERR;
    }
    result = WSAEventSelect((SOCKET)network->peer->sock_fd, network->read_event, FD_READ | FD_CLOSE);
    if (result == SOCKET_ERROR)
    {
        sr_err("xLOGIC network event could not associate with its socket.");
        if (network_source_clear(network) != SR_OK)
        {
            sr_err("xLOGIC network event association cleanup failed.");
        }
        return SR_ERR;
    }
    network->event_associated = TRUE;
    // Generic Windows session sources wait on HANDLEs, not socket descriptors.
    source_fd = (gintptr)network->read_event;
    events = G_IO_IN;
#endif
    // Read readiness wakes immediately; the nominal 1 ms timeout also retries
    // short/would-block sends and checks STATUS cadence and absolute deadlines.
    result = sr_session_fd_source_add(sdi->session, network, source_fd, events, 1, network_event_source, sdi);
    if (result != SR_OK)
    {
        if (network_source_clear(network) != SR_OK)
        {
            sr_err("xLOGIC network source registration cleanup failed.");
        }
        return result;
    }
    network->source_added = TRUE;
    network->source_session = sdi->session;
    return SR_OK;
}

static gboolean connection_window_parse(char **fields, uint32_t *credits, gboolean *explicit_window)
{
    const size_t count = g_strv_length(fields);
    guint64 value;

    if (count != 3U && count != 4U)
    {
        return FALSE;
    }
    *credits = xSDK_xLOGIC_NET_MAX_CREDITS;
    *explicit_window = count == 4U;
    if (!*explicit_window)
    {
        return TRUE;
    }
    if (fields[3][0] == '\0' || strspn(fields[3], "0123456789") != strlen(fields[3]))
    {
        return FALSE;
    }
    value = g_ascii_strtoull(fields[3], NULL, 10);
    if (value == 0U || value > xSDK_xLOGIC_NET_MAX_CREDITS)
    {
        return FALSE;
    }
    *credits = (uint32_t)value;
    return TRUE;
}

static const char *connection_host(const char *host, char resolved[16])
{
    size_t length = strlen(host);
    gboolean local = length >= 6U && g_ascii_strcasecmp(host + length - 6U, ".local") == 0;
    gboolean trailing = length >= 7U && g_ascii_strcasecmp(host + length - 7U, ".local.") == 0;
    if (local || trailing)
    {
        if (!xsdk_xlogic_mdns_resolve(host, resolved))
        {
            sr_err("xLOGIC local hostname was unavailable or ambiguous.");
            return NULL;
        }
        return resolved;
    }
    return host;
}

static gboolean connection_parse(char **fields, uint32_t *credits, gboolean *explicit_window, gboolean *esk)
{
    char *end = NULL;
    if (!connection_window_parse(fields, credits, explicit_window))
    {
        return FALSE;
    }
    *esk = strcmp(fields[0], "tcp-xesk") == 0;
    if ((!*esk && strcmp(fields[0], "tcp-raw") != 0) || (*esk && *explicit_window) || fields[1][0] == '\0' || fields[2][0] == '\0')
    {
        return FALSE;
    }
    const guint64 port = g_ascii_strtoull(fields[2], &end, 10);
    return *end == '\0' && port != 0U && port <= 65535U;
}

static struct xsdk_xlogic_network *network_new(const char *connection)
{
    struct xsdk_xlogic_network *network = NULL;
    char **fields = g_strsplit(connection, "/", 0);
    uint32_t credits;
    gboolean explicit_window, esk;
    char resolved[16];
    if (!connection_parse(fields, &credits, &explicit_window, &esk))
    {
        goto done;
    }
    const char *host = connection_host(fields[1], resolved);
    if (!host)
    {
        goto done;
    }
    network = g_try_new0(struct xsdk_xlogic_network, 1U);
    if (!network)
    {
        goto done;
    }
#ifdef _WIN32
    network->read_event = WSA_INVALID_EVENT;
#endif
    network->requested_credits = credits;
    network->window_explicit = explicit_window;
    network->credits = xSDK_xLOGIC_NET_CREDITS;
    network->accepted_credits = xSDK_xLOGIC_NET_CREDITS;
    network->peer = sr_tcp_dev_inst_new(host, fields[2]);
    network->esk = esk ? g_try_new0(xSDK_xESK_Network_Pump_t, 1U) : NULL;
    network->response = g_try_malloc(xSDK_xLOGIC_NET_RESPONSE_BYTES + xSDK_xLOGIC_NET_MAX_BLOCK);
    network->decoded = g_try_malloc(xLOGIC_NET_BLOCK_MAX_DECODED);
    if (!network->peer || (esk && !network->esk) || !network->response || !network->decoded)
    {
        sr_tcp_dev_inst_free(network->peer);
        g_free(network->response);
        g_free(network->decoded);
        g_free(network->esk);
        g_free(network);
        network = NULL;
    }
done:
    g_strfreev(fields);
    return network;
}

SR_PRIV void xsdk_xlogic_network_clear(struct dev_context *devc)
{
    int result;

    if (!devc->network)
    {
        return;
    }
    result = network_source_clear(devc->network);
    sr_tcp_disconnect(devc->network->peer);
    if (result != SR_OK)
    {
        // Final destruction has no caller to retain the object for another
        // attempt. Retry once after socket closure, then report an OS resource
        // release failure and free the remaining owned memory.
        if (network_source_clear(devc->network) != SR_OK)
        {
            sr_err("xLOGIC network event remains unreleased during final cleanup.");
        }
    }
    sr_tcp_dev_inst_free(devc->network->peer);
    g_free(devc->network->response);
    g_free(devc->network->decoded);
    g_free(devc->network->esk);
    g_free(devc->network);
    devc->network = NULL;
}

static struct sr_dev_inst *network_device(const char *connection, const struct xsdk_xlogic_discovery_device *discovered)
{
    struct xsdk_xlogic_network *network = network_new(connection);
    struct dev_context *devc;
    struct sr_dev_inst *sdi;
    unsigned int channel;
    int result;

    if (!network)
    {
        return NULL;
    }
    devc = g_try_new0(struct dev_context, 1U);
    if (!devc)
    {
        struct dev_context temporary = {.network = network};
        xsdk_xlogic_network_clear(&temporary);
        return NULL;
    }
    devc->network = network;
    result = SR_OK;
    if (discovered)
    {
        // DNS advertises identity and endpoint only. The fixed XLN1 profile is
        // provisional until normal device open validates authoritative CAPS.
        network->maximum_samples = xSDK_xLOGIC_NET_CAPACITY;
        network->maximum_read = xSDK_xLOGIC_NET_MAX_READ;
    }
    else
    {
        result = network_connect(network);
        if (result == SR_OK)
        {
            result = network->esk ? esk_describe(network) : exchange_caps(network);
        }
        sr_tcp_disconnect(network->peer);
    }
    if (result != SR_OK)
    {
        xsdk_xlogic_network_clear(devc);
        g_free(devc);
        return NULL;
    }
    devc->limit_samples = MIN((uint64_t)6144U, network->maximum_samples);
    devc->rate_count = network->esk ? xESK_LOGIC_RATE_PROFILE_1_COUNT : 1U;
    for (unsigned int index = 0U; index < devc->rate_count; ++index)
    {
        const uint32_t rate = network->esk ? xESK_Logic_Rate_Profile_1(index) : xSDK_xLOGIC_NET_SAMPLE_RATE;
        devc->rates[index].request = rate;
        devc->rates[index].actual = rate;
        devc->rates[index].stream_format = xSDK_xLOGIC_STREAM_FORMAT_U8;
    }
    sdi = g_malloc0(sizeof(*sdi));
    sdi->status = SR_ST_INACTIVE;
    sdi->inst_type = SR_INST_USER;
    sdi->vendor = g_strdup("xSDK");
    sdi->model = discovered ? g_strdup_printf("%s (%s)", discovered->instance, discovered->board)
                            : g_strdup(network->esk ? "xESK finite Ethernet" : "xLOGIC Ethernet DDR (experimental)");
    sdi->serial_num = discovered ? g_strdup(discovered->identity) : NULL;
    sdi->connection_id = g_strdup(connection);
    sdi->priv = devc;
    for (channel = 0U; channel < 8U; channel++)
    {
        char name[8];
        g_snprintf(name, sizeof(name), "D%u", channel);
        sr_channel_new(sdi, channel, SR_CHANNEL_LOGIC, TRUE, name);
    }
    return sdi;
}

SR_PRIV GSList *xsdk_xlogic_network_scan(struct sr_dev_driver *di, const char *connection)
{
    struct sr_dev_inst *device = network_device(connection, NULL);
    return std_scan_complete(di, device ? g_slist_append(NULL, device) : NULL);
}

SR_PRIV GSList *xsdk_xlogic_network_discover(void)
{
    struct xsdk_xlogic_discovery_device *discovered = g_try_new0(struct xsdk_xlogic_discovery_device, xSDK_xLOGIC_DISCOVERY_DEVICES);
    GSList *devices = NULL;
    size_t count, index;
    if (!discovered)
    {
        return NULL;
    }
    count = xsdk_xlogic_mdns_browse(discovered, xSDK_xLOGIC_DISCOVERY_DEVICES);
    for (index = 0; index < count; index++)
    {
        const uint8_t *address = (const uint8_t *)&discovered[index].address;
        char connection[64];
        struct sr_dev_inst *device;
        g_snprintf(connection, sizeof(connection), "tcp-raw/%u.%u.%u.%u/%u", address[0], address[1], address[2], address[3],
                   discovered[index].port);
        device = network_device(connection, &discovered[index]);
        if (device)
        {
            devices = g_slist_append(devices, device);
        }
    }
    g_free(discovered);
    return devices;
}

SR_PRIV int xsdk_xlogic_network_open(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    int result = network_connect(devc->network);

    if (result != SR_OK)
    {
        return result;
    }
    result = devc->network->esk ? esk_describe(devc->network) : exchange_caps(devc->network);
    if (result != SR_OK)
    {
        sr_tcp_disconnect(devc->network->peer);
    }
    return result;
}

SR_PRIV int xsdk_xlogic_network_close(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    int result;
    int disconnected;

    if (devc->acquisition_state != xSDK_xLOGIC_ACQ_IDLE)
    {
        network_finish(sdi, TRUE);
    }
    result = network_source_clear(devc->network);
    disconnected = sr_tcp_disconnect(devc->network->peer);
    if (result != SR_OK)
    {
        devc->failed = TRUE;
        return SR_ERR;
    }
    return disconnected;
}

static const char *const retrieval_sources[] = {
    "raw", "block-raw", "rle", "rle-strict", "palette", "palette-strict", "active", "active-strict", "nibble", "nibble-strict", "auto",
};

SR_PRIV GVariant *xsdk_xlogic_network_sources(const struct dev_context *devc)
{
    if (devc->network->esk)
    {
        return g_variant_new_strv(retrieval_sources, 1U);
    }
    return g_variant_new_strv(retrieval_sources, G_N_ELEMENTS(retrieval_sources));
}

SR_PRIV const char *xsdk_xlogic_network_source(const struct dev_context *devc)
{
    return retrieval_sources[devc->network->selected_source];
}

SR_PRIV int xsdk_xlogic_network_source_set(struct dev_context *devc, const char *source)
{
    size_t index;
    struct xsdk_xlogic_network *network = devc->network;

    if (network->esk && strcmp(source, "raw") != 0)
    {
        return SR_ERR_ARG;
    }

    for (index = 0U; index < G_N_ELEMENTS(retrieval_sources); index++)
    {
        if (strcmp(source, retrieval_sources[index]) == 0)
        {
            network->selected_source = index;
            network->automatic.enabled = index == G_N_ELEMENTS(retrieval_sources) - 1U;
            network->use_blocks = index != 0U;
            network->block.mode = index < 2U ? 0U : (index < 4U ? 1U : (uint32_t)((index - 4U) / 2U + 4U));
            network->block.fallback = index >= 2U && index % 2U == 0U ? 1U : 0U;
            network->negotiated = FALSE;
            return SR_OK;
        }
    }
    return SR_ERR_ARG;
}

SR_PRIV uint64_t xsdk_xlogic_network_limit(const struct dev_context *devc)
{
    return devc->network->maximum_samples;
}

SR_PRIV gboolean xsdk_xlogic_network_depth_valid(const struct dev_context *devc, uint64_t samples)
{
    if (samples == 0U || samples > devc->network->maximum_samples)
    {
        return FALSE;
    }
    if (devc->network->esk)
    {
        const uint64_t quantum = devc->rates[devc->rate_index].actual == 500000000U ? 192U : 64U;
        return ((samples + quantum - 1U) / quantum) * quantum <= devc->network->maximum_samples;
    }
    return TRUE;
}

static int configure_retrieval_window(struct dev_context *devc)
{
    struct xsdk_xlogic_network *network = devc->network;
    int result;

    if (network->automatic.enabled)
    {
        if (devc->channel_mask != 255U && devc->channel_mask != 15U && devc->channel_mask != 3U)
        {
            return SR_ERR_ARG;
        }
        xsdk_xlogic_auto_init(&network->automatic, devc->limit_samples, devc->channel_mask);
        network->block.mask = devc->channel_mask;
        auto_configure(network);
        return SR_OK;
    }

    network->use_blocks = network->selected_source != 0U || devc->channel_mask == 15U || devc->channel_mask == 3U;
    network->accepted_credits = xSDK_xLOGIC_NET_CREDITS;
    network->credits = network->window_explicit ? network->requested_credits : xSDK_xLOGIC_NET_CREDITS;
    if (!network->use_blocks && network->credits > xSDK_xLOGIC_NET_CREDITS)
    {
        sr_err("xLOGIC legacy RAW retrieval accepts at most eight credits; select block-raw or a codec for larger windows.");
        return SR_ERR_ARG;
    }
    if (network->use_blocks)
    {
        if (devc->channel_mask != 255U && devc->channel_mask != 15U && devc->channel_mask != 3U)
        {
            sr_err("xLOGIC block retrieval requires D0-D7, D0-D3 or D0-D1.");
            return SR_ERR_ARG;
        }
        result = negotiate_blocks(network, devc->channel_mask);
        if (result != SR_OK)
        {
            sr_tcp_disconnect(network->peer);
            queue_reset(network);
            return result;
        }
    }
    return SR_OK;
}

static int esk_start(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    const uint64_t quantum = devc->rates[devc->rate_index].actual == 500000000U ? 192U : 64U;
    network->rounded_samples = ((devc->limit_samples + quantum - 1U) / quantum) * quantum;
    network->offset = 0U;
    network->poll_after = 0;
    network->esk_retire_deadline = 0;
    network->esk_capture_deadline = g_get_monotonic_time() + 70 * G_TIME_SPAN_SECOND;
    network->stop_requested = FALSE;
    if (network_source_add(sdi) != SR_OK)
    {
        return SR_ERR;
    }
    devc->acquisition_state = xSDK_xLOGIC_ACQ_RUNNING;
    devc->sent_samples = 0U;
    devc->end_sent = FALSE;
    devc->failed = FALSE;
    std_session_send_df_header(sdi);
    if (esk_queue_phase(sdi, xSDK_xESK_PHASE_CLAIM) != SR_OK)
    {
        network_finish(sdi, TRUE);
        return SR_ERR;
    }
    return SR_OK;
}

SR_PRIV int xsdk_xlogic_network_start(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;
    struct xsdk_xlogic_network *network = devc->network;
    int result;

    if (devc->acquisition_state != xSDK_xLOGIC_ACQ_IDLE || devc->continuous || devc->limit_samples == 0U)
    {
        return SR_ERR_ARG;
    }
    if (network->esk)
    {
        // The device may retire an idle control connection while the user is
        // configuring the frontend. Reconnect only before acquiring a grant;
        // esk_describe rejects a different board or boot instead of replaying.
        sr_tcp_disconnect(network->peer);
        if (network_connect(network) != SR_OK)
        {
            return SR_ERR;
        }
        if (esk_describe(network) != SR_OK)
        {
            sr_tcp_disconnect(network->peer);
            return SR_ERR;
        }
        if (!xsdk_xlogic_network_depth_valid(devc, devc->limit_samples))
        {
            sr_tcp_disconnect(network->peer);
            return SR_ERR_ARG;
        }
        return esk_start(sdi);
    }
    if (!xsdk_xlogic_network_depth_valid(devc, devc->limit_samples) || network->peer->sock_fd < 0)
    {
        return SR_ERR_ARG;
    }
    network->rounded_samples = ((devc->limit_samples + xSDK_xLOGIC_NET_QUANTUM - 1U) / xSDK_xLOGIC_NET_QUANTUM) * xSDK_xLOGIC_NET_QUANTUM;
    if (network->rounded_samples > network->maximum_samples)
    {
        return SR_ERR_ARG;
    }
    memset(&network->capture, 0, sizeof(network->capture));
    queue_reset(network);
    network->retrieval_started = -1;
    network->first_publication = -1;
    network->framing_wire_bytes = 0U;
    network->offset = 0U;
    network->issued_offset = 0U;
    network->stop_requested = FALSE;
    result = configure_retrieval_window(devc);
    if (result != SR_OK)
    {
        return result;
    }
    result = network_source_add(sdi);
    if (result != SR_OK)
    {
        return result;
    }
    devc->acquisition_state = xSDK_xLOGIC_ACQ_RUNNING;
    devc->sent_samples = 0U;
    devc->end_sent = FALSE;
    devc->failed = FALSE;
    std_session_send_df_header(sdi);
    if (network->automatic.enabled)
    {
        if (network->use_blocks)
        {
            network->automatic.negotiation_credits = network->requested_credits;
            result = auto_negotiate(network, devc->channel_mask);
            if (result != SR_OK)
            {
                network_finish(sdi, TRUE);
            }
            return result;
        }
        network->automatic.phase = xSDK_xLOGIC_AUTO_PREFIX_PHASE;
    }
    result = send_request(network, xSDK_xLOGIC_NET_START, 0U, network->rounded_samples, xSDK_xLOGIC_NET_SAMPLE_RATE);
    if (result != SR_OK)
    {
        network_finish(sdi, TRUE);
    }
    return result;
}

SR_PRIV int xsdk_xlogic_network_stop(struct sr_dev_inst *sdi)
{
    struct dev_context *devc = sdi->priv;

    if (devc->acquisition_state != xSDK_xLOGIC_ACQ_IDLE)
    {
        devc->network->stop_requested = TRUE;
        devc->acquisition_state = xSDK_xLOGIC_ACQ_STOPPING;
    }
    return SR_OK;
}
