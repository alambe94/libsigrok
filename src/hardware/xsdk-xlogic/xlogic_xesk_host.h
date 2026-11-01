// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0
// Serialized MI0 admission shared by native WinUSB and the sigrok overlay.
#ifndef xLOGIC_xESK_HOST_H
#define xLOGIC_xESK_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define xLOGIC_xESK_CAP_SCOPED    (1UL << 8U)
#define xLOGIC_xESK_QUIESCENT     (1UL << 2U)
#define xLOGIC_xESK_START_PENDING (1UL << 3U)
#define xLOGIC_xESK_STOP_PENDING  (1UL << 4U)
#define xLOGIC_xESK_SESSION_FAULT (1UL << 5U)
#define xLOGIC_xESK_LEASE_MS      1000U

// transfer returns exact byte count, or -1 on any transport error. Only common
// FETCH may return -2 for its documented not-ready STALL. No mutation is retried.
typedef int (*xLOGIC_xESK_Transfer_t)(void *, bool, uint8_t, uint16_t, uint8_t *, size_t);
typedef uint64_t (*xLOGIC_xESK_Clock_t)(void *);
typedef void (*xLOGIC_xESK_Sleep_t)(void *, unsigned);

typedef enum
{
    xLOGIC_xESK_OPERATION_UNAVAILABLE = 0,
    xLOGIC_xESK_OPERATION_OUT_TRANSFER = 1,
    xLOGIC_xESK_OPERATION_FETCH_TRANSFER = 2,
    xLOGIC_xESK_OPERATION_ENVELOPE = 3,
    xLOGIC_xESK_OPERATION_STATUS = 4,
    xLOGIC_xESK_OPERATION_RESULT = 5,
    xLOGIC_xESK_OPERATION_ACCEPTED = 6
} xLOGIC_xESK_Operation_Outcome_t;

typedef struct
{
    void *user;
    xLOGIC_xESK_Transfer_t transfer;
    xLOGIC_xESK_Clock_t clock_ms;
    xLOGIC_xESK_Sleep_t sleep_ms;
    uint64_t boot_id;
    uint64_t session_id;
    uint64_t control_id;
    uint64_t operation_id;
    uint64_t last_renew_ms;
    uint64_t max_renew_gap_ms;
    uint64_t renewals;
    uint16_t connection_id;
    uint8_t terminal_state;
    uint32_t terminal_flags;
    bool scoped;
    bool poisoned;
    bool operation_poisoned;
    bool finish_requested;
    bool claimed;
    bool configured;
    bool started;
    bool stopped;
    bool release_attempted;
    bool released;
    bool reaped;
    bool close_attempted;
    bool closed;
    // Last call only; copy diagnostics before another operation or cleanup.
    xLOGIC_xESK_Operation_Outcome_t operation_outcome;
    int operation_out_bytes;
    int operation_reply_bytes;
    uint8_t request[176];
    uint8_t reply[176];
} xLOGIC_xESK_Host_t;

static inline uint64_t xlogic_xesk_get(const uint8_t *p, size_t bytes)
{
    uint64_t value = 0U;
    size_t i;
    for (i = 0; i < bytes; i++)
    {
        value |= (uint64_t)p[i] << (8U * i);
    }
    return value;
}

static inline void xlogic_xesk_put(uint8_t *p, uint64_t value, size_t bytes)
{
    size_t i;
    for (i = 0; i < bytes; i++)
    {
        p[i] = (uint8_t)(value >> (8U * i));
    }
}

static inline bool xlogic_xesk_native(const uint8_t *p, size_t bytes)
{
    return xlogic_xesk_get(p, 4U) == 0x584c4f47U && xlogic_xesk_get(p + 4U, 2U) == 3U && xlogic_xesk_get(p + 6U, 2U) == bytes;
}

static inline bool xlogic_xesk_status(const uint8_t *p)
{
    return xlogic_xesk_native(p, 64U) && xlogic_xesk_get(p + 8U, 4U) <= 1U && xlogic_xesk_get(p + 12U, 4U) <= 1U &&
           (xlogic_xesk_get(p + 56U, 4U) & ~UINT64_C(127)) == 0U;
}

static inline bool xlogic_xesk_quiescent(const uint8_t *status)
{
    const uint32_t flags = (uint32_t)xlogic_xesk_get(status + 56U, 4U);
    return xlogic_xesk_status(status) && xlogic_xesk_get(status + 12U, 4U) == 0U &&
           (flags & (xLOGIC_xESK_QUIESCENT | xLOGIC_xESK_START_PENDING | xLOGIC_xESK_STOP_PENDING)) == xLOGIC_xESK_QUIESCENT;
}

static inline int xlogic_xesk_out(xLOGIC_xESK_Host_t *host, uint8_t request, uint8_t *data, size_t bytes)
{
    return host->transfer(host->user, false, request, host->connection_id, data, bytes) == (int)bytes ? 0 : -1;
}

static inline void xlogic_xesk_control_header(xLOGIC_xESK_Host_t *host, uint8_t opcode, size_t bytes)
{
    memset(host->request, 0, sizeof(host->request));
    memcpy(host->request, "xESKCTRL", 8U);
    host->request[8] = 1U;
    host->request[9] = 1U;
    host->request[10] = opcode;
    xlogic_xesk_put(host->request + 12U, bytes, 2U);
    xlogic_xesk_put(host->request + 16U, ++host->control_id, 8U);
    xlogic_xesk_put(host->request + 24U, host->boot_id, 8U);
    xlogic_xesk_put(host->request + 32U, opcode == 2U ? 0U : host->session_id, 8U);
    xlogic_xesk_put(host->request + 40U, 2U, 2U);
}

static inline bool xlogic_xesk_control_fixed(const uint8_t *p, int bytes, uint8_t opcode)
{
    return bytes >= 48 && bytes <= 176 && memcmp(p, "xESKCTRL", 8U) == 0 && p[8] == 1U && p[9] == 2U && p[10] == opcode && p[11] <= 10U &&
           xlogic_xesk_get(p + 12U, 2U) == (uint64_t)bytes && xlogic_xesk_get(p + 14U, 2U) == 0U && xlogic_xesk_get(p + 44U, 4U) == 0U;
}

static inline bool xlogic_xesk_control_reply(xLOGIC_xESK_Host_t *host, int bytes, uint8_t opcode)
{
    const uint8_t *p = host->reply;
    if (!xlogic_xesk_control_fixed(p, bytes, opcode))
    {
        return false;
    }
    if (xlogic_xesk_get(p + 16U, 8U) != host->control_id || xlogic_xesk_get(p + 24U, 8U) != host->boot_id ||
        xlogic_xesk_get(p + 40U, 2U) != 2U || xlogic_xesk_get(p + 42U, 2U) != 0U)
    {
        return false;
    }
    if (opcode != 2U && xlogic_xesk_get(p + 32U, 8U) != host->session_id)
    {
        return false;
    }
    return p[11] == 0U ? bytes == (opcode == 6U ? 48 : 100) : bytes == 48;
}

static inline size_t xlogic_xesk_prepare_control(xLOGIC_xESK_Host_t *host, uint8_t opcode)
{
    const size_t bytes = opcode == 2U ? 64U : (opcode == 3U ? 56U : 48U);
    xlogic_xesk_control_header(host, opcode, bytes);
    if (opcode == 2U)
    {
        host->request[48] = 1U;
        xlogic_xesk_put(host->request + 56U, xLOGIC_xESK_LEASE_MS, 8U);
    }
    else if (opcode == 3U)
    {
        xlogic_xesk_put(host->request + 48U, xLOGIC_xESK_LEASE_MS, 8U);
    }
    return bytes;
}

static inline int xlogic_xesk_control_until(xLOGIC_xESK_Host_t *host, uint8_t opcode, uint64_t limit)
{
    const uint64_t started = host->clock_ms(host->user);
    uint64_t deadline;
    size_t bytes;
    int received;
    if (host->poisoned || host->close_attempted || host->control_id == UINT64_MAX || started > UINT64_MAX - 500U || started >= limit)
    {
        return -1;
    }
    deadline = started + 500U < limit ? started + 500U : limit;
    bytes = xlogic_xesk_prepare_control(host, opcode);
    if (xlogic_xesk_out(host, 0x71U, host->request, bytes) != 0)
    {
        host->poisoned = true;
        return -1;
    }
    received = -1;
    while (host->clock_ms(host->user) < deadline)
    {
        received = host->transfer(host->user, true, 0x72U, host->connection_id, host->reply, sizeof(host->reply));
        if (received != -2)
        {
            break;
        }
        host->sleep_ms(host->user, 1U);
    }
    if (host->clock_ms(host->user) >= deadline || !xlogic_xesk_control_reply(host, received, opcode))
    {
        host->poisoned = true;
        return -1;
    }
    xlogic_xesk_put(host->request, host->boot_id, 8U);
    xlogic_xesk_put(host->request + 8U, host->control_id, 8U);
    if (xlogic_xesk_out(host, 0x73U, host->request, 16U) != 0)
    {
        host->poisoned = true;
        return -1;
    }
    if (host->clock_ms(host->user) >= deadline)
    {
        host->poisoned = true;
        return -1;
    }
    return host->reply[11] == 0U ? 0 : -1;
}

static inline int xlogic_xesk_control(xLOGIC_xESK_Host_t *host, uint8_t opcode)
{
    return xlogic_xesk_control_until(host, opcode, UINT64_MAX);
}

static inline bool xlogic_xesk_session_descriptor(const uint8_t *p)
{
    return memcmp(p, "xESK", 4U) == 0 && p[4] == 1U && p[5] == 5U && xlogic_xesk_get(p + 6U, 2U) == 0U &&
           xlogic_xesk_get(p + 8U, 2U) == 52U && xlogic_xesk_get(p + 10U, 2U) == 0U;
}

static inline bool xlogic_xesk_session_binding(const uint8_t *s)
{
    return xlogic_xesk_get(s + 24U, 2U) == 2U && xlogic_xesk_get(s + 26U, 2U) == 0U && xlogic_xesk_get(s + 28U, 4U) == 1U && s[32] == 1U &&
           s[33] == 1U && s[35] == 1U;
}

static inline bool xlogic_xesk_session_state(const uint8_t *s, bool active)
{
    if (active)
    {
        return s[34] == 1U && xlogic_xesk_get(s + 16U, 8U) != 0U && xlogic_xesk_get(s + 36U, 4U) == 0U;
    }
    return s[34] >= 2U && s[34] <= 4U && xlogic_xesk_get(s + 36U, 4U) <= 1U;
}

static inline bool xlogic_xesk_session(xLOGIC_xESK_Host_t *host, bool active)
{
    const uint8_t *p = host->reply + 48U;
    const uint8_t *s = p + 12U;
    const uint64_t session = xlogic_xesk_get(s + 8U, 8U);
    if (!xlogic_xesk_session_descriptor(p) || !xlogic_xesk_session_binding(s))
    {
        return false;
    }
    if (xlogic_xesk_get(s, 8U) != host->boot_id || session == 0U || session != xlogic_xesk_get(host->reply + 32U, 8U) ||
        (host->claimed && session != host->session_id))
    {
        return false;
    }
    if (!xlogic_xesk_session_state(s, active))
    {
        return false;
    }
    if (host->released && s[34] != host->terminal_state)
    {
        return false;
    }
    host->session_id = session;
    host->terminal_state = s[34];
    host->terminal_flags = (uint32_t)xlogic_xesk_get(s + 36U, 4U);
    return true;
}

static inline int xlogic_xesk_open(xLOGIC_xESK_Host_t *host)
{
    int received;
    if (!host->scoped)
    {
        return 0;
    }
    if (host->connection_id != 0U || host->claimed || host->close_attempted)
    {
        return -1;
    }
    received = host->transfer(host->user, true, 0x70U, 0U, host->reply, 24U);
    if (received != 24 || xlogic_xesk_get(host->reply, 2U) != 1U || xlogic_xesk_get(host->reply + 2U, 2U) == 0U ||
        xlogic_xesk_get(host->reply + 4U, 4U) != 0U || xlogic_xesk_get(host->reply + 8U, 8U) == 0U ||
        xlogic_xesk_get(host->reply + 16U, 4U) != 5000U || xlogic_xesk_get(host->reply + 20U, 2U) != 176U ||
        xlogic_xesk_get(host->reply + 22U, 2U) != 0U)
    {
        host->poisoned = true;
        return -1;
    }
    host->connection_id = (uint16_t)xlogic_xesk_get(host->reply + 2U, 2U);
    host->boot_id = xlogic_xesk_get(host->reply + 8U, 8U);
    host->last_renew_ms = host->clock_ms(host->user);
    if (xlogic_xesk_control(host, 2U) != 0)
    {
        return -1;
    }
    if (!xlogic_xesk_session(host, true))
    {
        host->poisoned = true;
        return -1;
    }
    host->claimed = true;
    return 0;
}

static inline bool xlogic_xesk_renewal_due(const xLOGIC_xESK_Host_t *host, uint64_t now)
{
    return host->scoped && host->claimed && !host->released && now >= host->last_renew_ms && now - host->last_renew_ms >= 200U;
}

static inline int xlogic_xesk_tick(xLOGIC_xESK_Host_t *host)
{
    uint64_t now;
    uint64_t started;
    uint64_t gap;
    if (!host->scoped || !host->claimed || host->released)
    {
        return 0;
    }
    now = host->clock_ms(host->user);
    if (now < host->last_renew_ms)
    {
        return -1;
    }
    if (!xlogic_xesk_renewal_due(host, now))
    {
        return 0;
    }
    started = now;
    if (xlogic_xesk_control(host, 3U) != 0)
    {
        return -1;
    }
    if (!xlogic_xesk_session(host, true))
    {
        return -1;
    }
    xlogic_xesk_put(host->request, host->boot_id, 8U);
    if (xlogic_xesk_out(host, 0x74U, host->request, 8U) != 0)
    {
        host->poisoned = true;
        return -1;
    }
    now = host->clock_ms(host->user);
    gap = now - host->last_renew_ms;
    if (gap > host->max_renew_gap_ms)
    {
        host->max_renew_gap_ms = gap;
    }
    host->last_renew_ms = started;
    host->renewals++;
    return gap < xLOGIC_xESK_LEASE_MS ? 0 : -1;
}

static inline bool xlogic_xesk_operation_usable(const xLOGIC_xESK_Host_t *host, uint8_t opcode, const uint8_t *config)
{
    return host->scoped && host->claimed && !host->released && !host->close_attempted && !host->poisoned && !host->operation_poisoned &&
           opcode >= 1U && opcode <= 4U && (opcode != 1U || config != NULL) &&
           (opcode != 4U || (host->started && !host->stopped && !host->finish_requested)) && host->operation_id != UINT64_MAX;
}

static inline bool xlogic_xesk_operation_envelope(const xLOGIC_xESK_Host_t *host, int bytes)
{
    return bytes == 96 && host->reply[6] <= 4U && memcmp(host->reply, host->request, 6U) == 0 &&
           memcmp(host->reply + 7U, host->request + 7U, 25U) == 0;
}

static inline int xlogic_xesk_operation(xLOGIC_xESK_Host_t *host, uint8_t opcode, const uint8_t *config)
{
    const size_t bytes = opcode == 1U ? 76U : 32U;
    int received;
    host->operation_outcome = xLOGIC_xESK_OPERATION_UNAVAILABLE;
    host->operation_out_bytes = host->operation_reply_bytes = 0;
    if (!xlogic_xesk_operation_usable(host, opcode, config))
    {
        return -1;
    }
    memset(host->request, 0, bytes);
    memcpy(host->request, "xELG", 4U);
    host->request[4] = 1U;
    host->request[5] = opcode;
    xlogic_xesk_put(host->request + 8U, host->boot_id, 8U);
    xlogic_xesk_put(host->request + 16U, host->session_id, 8U);
    xlogic_xesk_put(host->request + 24U, ++host->operation_id, 8U);
    if (config != NULL && opcode == 1U)
    {
        memcpy(host->request + 32U, config, 44U);
    }
    host->operation_out_bytes = host->transfer(host->user, false, 0x76U, host->connection_id, host->request, bytes);
    if (host->operation_out_bytes != (int)bytes)
    {
        host->operation_outcome = xLOGIC_xESK_OPERATION_OUT_TRANSFER;
        host->operation_poisoned = true;
        return -1;
    }
    received = host->transfer(host->user, true, 0x77U, host->connection_id, host->reply, 96U);
    host->operation_reply_bytes = received;
    if (received < 0)
    {
        host->operation_outcome = xLOGIC_xESK_OPERATION_FETCH_TRANSFER;
        host->operation_poisoned = true;
        return -1;
    }
    if (!xlogic_xesk_operation_envelope(host, received))
    {
        host->operation_outcome = xLOGIC_xESK_OPERATION_ENVELOPE;
        host->operation_poisoned = true;
        return -1;
    }
    if (!xlogic_xesk_status(host->reply + 32U))
    {
        host->operation_outcome = xLOGIC_xESK_OPERATION_STATUS;
        host->operation_poisoned = true;
        return -1;
    }
    if (host->reply[6] != 0U)
    {
        host->operation_outcome = xLOGIC_xESK_OPERATION_RESULT;
        return -1;
    }
    host->operation_outcome = xLOGIC_xESK_OPERATION_ACCEPTED;
    host->configured = host->configured || opcode == 1U;
    host->started = host->started || opcode == 2U;
    host->stopped = host->stopped || opcode == 3U;
    host->finish_requested = host->finish_requested || opcode == 4U;
    return 0;
}

static inline int xlogic_xesk_close(xLOGIC_xESK_Host_t *host)
{
    if (!host->scoped || host->closed || host->connection_id == 0U)
    {
        return 0;
    }
    if (host->close_attempted)
    {
        return -1;
    }
    host->close_attempted = true;
    xlogic_xesk_put(host->request, host->boot_id, 8U);
    if (xlogic_xesk_out(host, 0x75U, host->request, 8U) != 0)
    {
        return -1;
    }
    host->closed = true;
    return 0;
}

static inline int xlogic_xesk_wait_safe(xLOGIC_xESK_Host_t *host, uint64_t deadline)
{
    // RELEASE acceptance and safety confirmation are distinct. SESSION is a
    // fresh read-only query; never retry the RELEASE mutation.
    while (host->terminal_flags == 0U)
    {
        if (host->clock_ms(host->user) >= deadline)
        {
            return -1;
        }
        host->sleep_ms(host->user, 1U);
        if (xlogic_xesk_control_until(host, 5U, deadline) != 0)
        {
            return -1;
        }
        if (!xlogic_xesk_session(host, false))
        {
            host->poisoned = true;
            return -1;
        }
    }
    return host->clock_ms(host->user) < deadline ? 0 : -1;
}

static inline int xlogic_xesk_release(xLOGIC_xESK_Host_t *host, const uint8_t *status, bool host_drained)
{
    int closed;
    uint64_t gap;
    uint64_t deadline;
    if (!host->scoped)
    {
        return 0;
    }
    if (!host_drained || !xlogic_xesk_quiescent(status) || host->operation_poisoned || host->release_attempted)
    {
        return -1;
    }
    deadline = host->clock_ms(host->user);
    if (deadline > UINT64_MAX - 1000U)
    {
        return -1;
    }
    gap = deadline - host->last_renew_ms;
    deadline += 1000U;
    if (gap > host->max_renew_gap_ms)
    {
        host->max_renew_gap_ms = gap;
    }
    host->release_attempted = true;
    if (xlogic_xesk_control_until(host, 4U, deadline) != 0)
    {
        return -1;
    }
    if (!xlogic_xesk_session(host, false))
    {
        host->poisoned = true;
        return -1;
    }
    host->released = true;
    if (xlogic_xesk_wait_safe(host, deadline) != 0)
    {
        return -1;
    }
    if (xlogic_xesk_control_until(host, 6U, deadline) != 0)
    {
        return -1;
    }
    host->reaped = true;
    closed = xlogic_xesk_close(host);
    if (closed != 0)
    {
        return -1;
    }
    return host->terminal_state == 3U && gap < xLOGIC_xESK_LEASE_MS ? 0 : -1;
}

#endif
