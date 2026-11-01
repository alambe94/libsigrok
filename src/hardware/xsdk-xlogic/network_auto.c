/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#include <string.h>
#include "network_auto.h"

void xsdk_xlogic_auto_init(struct xsdk_xlogic_auto *automatic, uint64_t end, uint8_t mask)
{
    memset(automatic, 0, sizeof(*automatic));
    automatic->enabled = true;
    automatic->eligible = end >= xSDK_xLOGIC_AUTO_MINIMUM;
    automatic->full_view = mask == 255U;
    automatic->end = end;
    automatic->phase = xSDK_xLOGIC_AUTO_INITIAL;
}

size_t xsdk_xlogic_auto_count(const struct xsdk_xlogic_auto *automatic)
{
    return automatic->full_view ? 6U : 5U;
}

bool xsdk_xlogic_auto_legacy(const struct xsdk_xlogic_auto *automatic, size_t candidate)
{
    return automatic->full_view && candidate == 0U;
}

uint32_t xsdk_xlogic_auto_mode(const struct xsdk_xlogic_auto *automatic, size_t candidate)
{
    static const uint32_t modes[] = {0U, 5U, 6U, 4U, 1U};
    return modes[automatic->full_view && candidate != 0U ? candidate - 1U : candidate];
}

const char *xsdk_xlogic_auto_name(const struct xsdk_xlogic_auto *automatic, size_t candidate)
{
    static const char *const names[] = {"raw", "block-raw", "active", "nibble", "palette", "rle"};
    return names[candidate + (automatic->full_view ? 0U : 1U)];
}

uint64_t xsdk_xlogic_auto_offset(const struct xsdk_xlogic_auto *automatic, size_t issued)
{
    const uint64_t remaining = automatic->end - xSDK_xLOGIC_AUTO_PREFIX;
    const size_t span = issued / xSDK_xLOGIC_AUTO_PREFIX;
    uint64_t start = xSDK_xLOGIC_AUTO_PREFIX;
    if (span == 1U)
    {
        start += 4096U * ((remaining - xSDK_xLOGIC_AUTO_PREFIX) / 8192U);
    }
    else if (span == 2U)
    {
        start += 4096U * ((remaining - xSDK_xLOGIC_AUTO_PREFIX) / 4096U);
    }
    return start + issued % xSDK_xLOGIC_AUTO_PREFIX;
}

size_t xsdk_xlogic_auto_select(const struct xsdk_xlogic_auto *automatic)
{
    size_t best = 0U;
    size_t index;
    for (index = 1U; index < xsdk_xlogic_auto_count(automatic); index++)
    {
        if (automatic->outcomes[index] == 1U && automatic->durations[index] < automatic->durations[best])
        {
            best = index;
        }
    }
    // Avoid multiplication overflow; measured intervals are bounded by request deadlines.
    return automatic->durations[best] <= automatic->durations[0] - (automatic->durations[0] + 9) / 10 ? best : 0U;
}
