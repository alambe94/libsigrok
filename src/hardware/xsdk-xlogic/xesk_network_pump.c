/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#include "xesk_network_pump.h"
#include "xesk_control_internal.h"

static xRETURN_t fail(xSDK_xESK_Network_Pump_t *pump)
{
    xSDK_xESK_Network_Poison(&pump->session);
    return xRETURN_xERR_xESK_CORRUPT;
}

xRETURN_t xSDK_xESK_Network_Queue(xSDK_xESK_Network_Pump_t *pump, const xESK_Control_Message_t *request)
{
    if (!xESK_Control_Range_Valid(pump, sizeof(*pump), _Alignof(xSDK_xESK_Network_Pump_t)) ||
        !xESK_Control_Range_Valid(request, sizeof(*request), _Alignof(xESK_Control_Message_t)) ||
        !xESK_Control_Ranges_Disjoint(pump, sizeof(*pump), request, sizeof(*request)))
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (pump->active)
    {
        return xRETURN_xERR_xESK_BUSY;
    }
    size_t bytes = 0U;
    const xRETURN_t status = xSDK_xESK_Network_Request(&pump->session, request, pump->request, sizeof(pump->request), &bytes);
    if (status == xRETURN_OK)
    {
        pump->request_bytes = bytes;
        pump->request_sent = 0U;
        pump->response_bytes = 0U;
        pump->response_expected = xESK_CONTROL_HEADER_BYTES;
        pump->block_received = 0U;
        pump->block_bytes = 0U;
        pump->active = true;
        pump->complete = false;
    }
    return status;
}

static xRETURN_t
transfer(xSDK_xESK_Network_Pump_t *pump, void *user, xSDK_xESK_Network_IO_t io, uint8_t *buffer, size_t expected, size_t *offset)
{
    if (*offset > expected)
    {
        return fail(pump);
    }
    const size_t remaining = expected - *offset;
    const int count = io(user, buffer + *offset, remaining);
    if (count < 0 || (size_t)count > remaining)
    {
        return fail(pump);
    }
    *offset += (size_t)count;
    return *offset == expected ? xRETURN_OK : xRETURN_xERR_xESK_BUSY;
}

static xRETURN_t receive_control(xSDK_xESK_Network_Pump_t *pump, void *user, xSDK_xESK_Network_IO_t read_fn)
{
    xRETURN_t status = transfer(pump, user, read_fn, pump->response, pump->response_expected, &pump->response_bytes);
    if (status != xRETURN_OK)
    {
        return status;
    }
    if (pump->response_expected == xESK_CONTROL_HEADER_BYTES)
    {
        const size_t bytes = (size_t)pump->response[12U] | ((size_t)pump->response[13U] << 8U);
        if (bytes < xESK_CONTROL_HEADER_BYTES || bytes > sizeof(pump->response))
        {
            return fail(pump);
        }
        pump->response_expected = bytes;
        if (bytes != xESK_CONTROL_HEADER_BYTES)
        {
            return xRETURN_xERR_xESK_BUSY;
        }
    }
    status = xSDK_xESK_Network_Reply(&pump->session, pump->response, pump->response_bytes, &pump->reply);
    if (status != xRETURN_OK)
    {
        return fail(pump);
    }
    pump->block_bytes = pump->session.has_block ? pump->session.block_bytes : 0U;
    if (pump->block_bytes > sizeof(pump->block))
    {
        return fail(pump);
    }
    return xRETURN_OK;
}

xRETURN_t
xSDK_xESK_Network_Pump(xSDK_xESK_Network_Pump_t *pump, void *user, xSDK_xESK_Network_IO_t write_fn, xSDK_xESK_Network_IO_t read_fn)
{
    if (!xESK_Control_Range_Valid(pump, sizeof(*pump), _Alignof(xSDK_xESK_Network_Pump_t)) || write_fn == NULL || read_fn == NULL)
    {
        return xRETURN_xERR_xESK_INVALID_ARG;
    }
    if (!pump->active || pump->session.poisoned)
    {
        return xRETURN_xERR_xESK_INVALID_STATE;
    }
    if (pump->request_sent < pump->request_bytes)
    {
        const xRETURN_t status = transfer(pump, user, write_fn, pump->request, pump->request_bytes, &pump->request_sent);
        if (status != xRETURN_OK)
        {
            return status;
        }
    }
    if (pump->session.has_pending)
    {
        const xRETURN_t status = receive_control(pump, user, read_fn);
        if (status != xRETURN_OK)
        {
            return status;
        }
    }
    if (pump->session.has_block)
    {
        const xRETURN_t status = transfer(pump, user, read_fn, pump->block, pump->block_bytes, &pump->block_received);
        if (status != xRETURN_OK)
        {
            return status;
        }
        if (xSDK_xESK_Network_Block(&pump->session, pump->block, pump->block_bytes) != xRETURN_OK)
        {
            return fail(pump);
        }
    }
    pump->active = false;
    pump->complete = true;
    return xRETURN_OK;
}
