/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#ifndef LIBSIGROK_xSDK_xLOGIC_xESK_NETWORK_PUMP_H
#define LIBSIGROK_xSDK_xLOGIC_xESK_NETWORK_PUMP_H

#include "xesk_network_session.h"

/* Nonblocking callbacks return bytes transferred, zero for would-block, or -1
 * on EOF/error. The caller owns the socket and the absolute deadline. A timeout
 * or uncertain I/O must poison the session and close the socket. A completed
 * reply and verified block remain available only until the next queue call. */
typedef int (*xSDK_xESK_Network_IO_t)(void *user, uint8_t *data, size_t bytes);

typedef struct
{
    xSDK_xESK_Network_Session_t session;
    xESK_Control_Message_t reply;
    uint8_t request[xESK_CONTROL_MAX_BYTES];
    uint8_t response[xESK_CONTROL_MAX_BYTES];
    uint8_t block[xLOGIC_NET_BLOCK_MAX_DECODED];
    size_t request_bytes;
    size_t request_sent;
    size_t response_bytes;
    size_t response_expected;
    size_t block_received;
    size_t block_bytes;
    bool active;
    bool complete;
} xSDK_xESK_Network_Pump_t;

xRETURN_t xSDK_xESK_Network_Queue(xSDK_xESK_Network_Pump_t *pump, const xESK_Control_Message_t *request);
xRETURN_t
xSDK_xESK_Network_Pump(xSDK_xESK_Network_Pump_t *pump, void *user, xSDK_xESK_Network_IO_t write_fn, xSDK_xESK_Network_IO_t read_fn);

#endif
