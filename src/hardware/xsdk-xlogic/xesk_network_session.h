/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#ifndef LIBSIGROK_xSDK_xLOGIC_xESK_NETWORK_SESSION_H
#define LIBSIGROK_xSDK_xLOGIC_xESK_NETWORK_SESSION_H

#include "xesk_control.h"
#include "xesk_logic_network.h"

/* One serialized TCP connection. The caller owns socket I/O and deadlines;
 * an uncertain exchange must be poisoned, never replayed on this connection. */
typedef struct
{
    xESK_Control_Message_t pending;
    xESK_Logic_Metadata_t capture;
    xESK_Device_t device;
    xESK_Service_t logic_service;
    uint64_t boot_id;
    uint64_t session_id;
    uint64_t highest_session_id;
    uint64_t request_id;
    uint32_t block_bytes;
    uint32_t block_crc32;
    uint32_t session_timebase_id;
    uint32_t logic_timebase_id;
    bool has_pending;
    bool has_block;
    bool has_capture;
    bool capture_ready;
    bool has_finite_logic;
    bool retiring;
    bool safe_to_reap;
    bool poisoned;
} xSDK_xESK_Network_Session_t;

xRETURN_t xSDK_xESK_Network_Request(xSDK_xESK_Network_Session_t *session,
                                    const xESK_Control_Message_t *request,
                                    uint8_t *wire,
                                    size_t capacity,
                                    size_t *written);
xRETURN_t xSDK_xESK_Network_Reply(xSDK_xESK_Network_Session_t *session, const uint8_t *wire, size_t length, xESK_Control_Message_t *reply);
xRETURN_t xSDK_xESK_Network_Block(xSDK_xESK_Network_Session_t *session, const uint8_t *data, size_t length);
void xSDK_xESK_Network_Poison(xSDK_xESK_Network_Session_t *session);

#endif
