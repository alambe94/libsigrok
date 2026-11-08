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

// @file xutil_crc32.h
// @brief Shared IEEE CRC32 update and one-shot calculation contract.
//

#ifndef xUTIL_CRC32_H
#define xUTIL_CRC32_H

#ifdef __cplusplus
extern "C"
{
#endif

// INCLUDES ////////////////////////////////////////////////////////////////////

// COMPILER INCLUDES
#include <stddef.h>
#include <stdint.h>

// SYSTEM INCLUDES

// MODULE INCLUDES

// MACROS //////////////////////////////////////////////////////////////////////
#define xUTIL_CRC32_IEEE_INIT    UINT32_C(0xFFFFFFFF)
#define xUTIL_CRC32_IEEE_XOR_OUT UINT32_C(0xFFFFFFFF)

    // TYPES ///////////////////////////////////////////////////////////////////

    // VARIABLES ///////////////////////////////////////////////////////////////

    // INLINE FUNCTIONS ////////////////////////////////////////////////////////

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////

    /**
     * @brief Updates a running reflected IEEE CRC32 state.
     * @param data Input bytes; NULL processes zero bytes for compatibility.
     * @param length Number of bytes to process when data is not NULL.
     * @param initial_crc Initial or previously returned running state.
     * @return Unfinalized running CRC32 state for chunked operation.
     */
    uint32_t xUTIL_CRC32_IEEE_Update(const uint8_t *data, size_t length, uint32_t initial_crc);

    /**
     * @brief Calculates a finalized reflected IEEE CRC32 value.
     * @param data Input bytes; NULL processes zero bytes for compatibility.
     * @param length Number of bytes to process when data is not NULL.
     * @return CRC32 initialized and finalized with 0xFFFFFFFF.
     */
    uint32_t xUTIL_CRC32_IEEE_Calculate(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif // xUTIL_CRC32_H

// EOF /////////////////////////////////////////////////////////////////////////////
