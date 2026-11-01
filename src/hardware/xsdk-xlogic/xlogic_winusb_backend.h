/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2026 alambe94
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#ifndef xSDK_xLOGIC_WINUSB_BACKEND_H
#define xSDK_xLOGIC_WINUSB_BACKEND_H

#include "protocol.h"

#ifdef _WIN32
SR_PRIV GSList *xsdk_xlogic_winusb_scan(struct sr_dev_driver *di, const char *connection);
SR_PRIV int xsdk_xlogic_winusb_open(struct sr_dev_inst *sdi);
SR_PRIV int xsdk_xlogic_winusb_close(struct sr_dev_inst *sdi);
SR_PRIV int xsdk_xlogic_winusb_start(struct sr_dev_inst *sdi);
SR_PRIV int xsdk_xlogic_winusb_stop(struct sr_dev_inst *sdi);
SR_PRIV void xsdk_xlogic_winusb_clear(struct dev_context *devc);
SR_PRIV int xsdk_xlogic_winusb_prepare_clear(const struct sr_dev_driver *di);
#endif

// Shared frontend state; platform adapters keep transport ownership private.
SR_PRIV struct dev_context *xsdk_xlogic_context_new(const struct xsdk_xlogic_caps *caps);
SR_PRIV int xsdk_xlogic_context_set_caps(struct dev_context *devc, const struct xsdk_xlogic_caps *caps);

#endif
