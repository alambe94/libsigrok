// Copyright 2026 alambe94
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// @file xnet_return.h
// @brief This defines the return codes for the xNET module.
//

#ifndef xNET_RETURN_H
#define xNET_RETURN_H

#ifdef __cplusplus
extern "C"
{
#endif

// INCLUDES ////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES

// SYSTEM INCLUDES

// MODULE INCLUDES
#include "xreturn.h"

// MACROS //////////////////////////////////////////////////////////////////////
#define xRETURN_xNET_OK                      ((xRETURN_t)0)
#define xRETURN_xERR_xNET_NULL_POINTER       xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 1U)
#define xRETURN_xERR_xNET_INVALID_ARGUMENT   xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 2U)
#define xRETURN_xERR_xNET_INVALID_STATE      xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 3U)
#define xRETURN_xERR_xNET_INVALID_PACKET     xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 4U)
#define xRETURN_xERR_xNET_INVALID_LENGTH     xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 5U)
#define xRETURN_xERR_xNET_BUFFER_TOO_SMALL   xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 6U)
#define xRETURN_xERR_xNET_NO_PACKET_BUFFER   xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 7U)
#define xRETURN_xERR_xNET_UNSUPPORTED        xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 8U)
#define xRETURN_xERR_xNET_TIMEOUT            xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 9U)
#define xRETURN_xERR_xNET_NOT_FOUND          xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 10U)
#define xRETURN_xERR_xNET_LINK_DOWN          xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 11U)
#define xRETURN_xERR_xNET_CHECKSUM_FAILED    xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 12U)
#define xRETURN_xERR_xNET_DNS_NAME_ERROR     xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 13U)
#define xRETURN_xERR_xNET_DNS_SERVER_ERROR   xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 14U)
#define xRETURN_xERR_xNET_DNS_NO_SERVER      xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 15U)
#define xRETURN_xERR_xNET_DNS_BUSY           xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 16U)
#define xRETURN_xERR_xNET_WOULD_BLOCK        xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 17U)
#define xRETURN_xERR_xNET_CONNECTION_REFUSED xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 18U)
#define xRETURN_xERR_xNET_CONNECTION_RESET   xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 19U)
#define xRETURN_xERR_xNET_ADDRESS_IN_USE     xRETURN_MAKE(xRETURN_xNET_MODULE, xRETURN_SEVERITY_ERROR, 20U)

    // TYPES ///////////////////////////////////////////////////////////////////////

    // VARIABLES ///////////////////////////////////////////////////////////////////

    // INLINE FUNCTIONS ////////////////////////////////////////////////////////////

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xNET_RETURN_H
// EOF /////////////////////////////////////////////////////////////////////////////
