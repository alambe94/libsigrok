/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#ifndef LIBSIGROK_xSDK_xLOGIC_NETWORK_AUTO_H
#define LIBSIGROK_xSDK_xLOGIC_NETWORK_AUTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define xSDK_xLOGIC_AUTO_PREFIX      65536U
#define xSDK_xLOGIC_AUTO_TRIAL_BYTES (3U * xSDK_xLOGIC_AUTO_PREFIX)
#define xSDK_xLOGIC_AUTO_MINIMUM     4194304U
#define xSDK_xLOGIC_AUTO_BUDGET_US   3000000

enum xsdk_xlogic_auto_phase
{
    xSDK_xLOGIC_AUTO_INITIAL,
    xSDK_xLOGIC_AUTO_PREFIX_PHASE,
    xSDK_xLOGIC_AUTO_NEGOTIATE,
    xSDK_xLOGIC_AUTO_TRIAL,
    xSDK_xLOGIC_AUTO_FINAL,
    xSDK_xLOGIC_AUTO_DOWNLOAD,
};

// Fixed per-device host policy state; no payload or capture-sized allocation.
struct xsdk_xlogic_auto
{
    bool enabled;
    bool eligible;
    bool full_view;
    enum xsdk_xlogic_auto_phase phase;
    size_t candidate;
    size_t selected;
    size_t issued;
    size_t received;
    uint32_t negotiation_credits;
    uint64_t end;
    int64_t budget_started;
    int64_t trial_started;
    int64_t selection_us;
    int64_t durations[6];
    uint8_t outcomes[6]; // 0 budget-skipped, 1 measured, 2 unsupported.
    uint64_t probe_wire_bytes;
    uint64_t negotiation_wire_bytes;
    uint64_t fallback_blocks;
    bool budget_skipped;
};

void xsdk_xlogic_auto_init(struct xsdk_xlogic_auto *automatic, uint64_t end, uint8_t mask);
size_t xsdk_xlogic_auto_count(const struct xsdk_xlogic_auto *automatic);
uint32_t xsdk_xlogic_auto_mode(const struct xsdk_xlogic_auto *automatic, size_t candidate);
bool xsdk_xlogic_auto_legacy(const struct xsdk_xlogic_auto *automatic, size_t candidate);
const char *xsdk_xlogic_auto_name(const struct xsdk_xlogic_auto *automatic, size_t candidate);
uint64_t xsdk_xlogic_auto_offset(const struct xsdk_xlogic_auto *automatic, size_t issued);
size_t xsdk_xlogic_auto_select(const struct xsdk_xlogic_auto *automatic);
#endif
