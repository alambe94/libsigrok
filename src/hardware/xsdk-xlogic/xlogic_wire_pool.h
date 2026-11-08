/*
 * Copyright (C) 2026 alambe94
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Private bounded handoff storage. Producer and consumer must use the same
 * mutex for every metadata operation; its acquire/release synchronization also
 * publishes payload writes. A reserved block may be filled outside that lock.
 * On successful publish the producer gives up its handle and must stop writing;
 * an acquired block remains owned by the consumer until release. Handles detect
 * stale generations but are not capabilities against misuse by a former owner.
 * Close discards queued blocks, not blocks held by the producer or consumer.
 * All storage is supplied by the caller and must outlive every borrow. Do not
 * reinitialize until all borrows are released and both threads have stopped;
 * handles do not survive init.
 */
#ifndef xSDK_xLOGIC_WIRE_POOL_H
#define xSDK_xLOGIC_WIRE_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    xSDK_xLOGIC_BLOCK_FREE,
    xSDK_xLOGIC_BLOCK_RESERVED,
    xSDK_xLOGIC_BLOCK_QUEUED,
    xSDK_xLOGIC_BLOCK_ACQUIRED
} xsdk_xlogic_block_state;

typedef struct
{
    xsdk_xlogic_block_state state;
    size_t length;
    uint64_t sequence;
    uint64_t generation;
} xsdk_xlogic_wire_block;

typedef struct
{
    size_t index;
    uint64_t generation;
} xsdk_xlogic_wire_handle;

typedef struct
{
    uint8_t *storage;
    size_t block_size;
    size_t capacity;
    xsdk_xlogic_wire_block *blocks;
    size_t *fifo;
    size_t head;
    size_t count;
    bool is_closed;
} xsdk_xlogic_wire_pool;

bool xsdk_xlogic_wire_pool_init(xsdk_xlogic_wire_pool *pool,
                                uint8_t *storage,
                                size_t storage_bytes,
                                size_t block_size,
                                xsdk_xlogic_wire_block *blocks,
                                size_t *fifo,
                                size_t capacity);
bool xsdk_xlogic_wire_pool_reserve(xsdk_xlogic_wire_pool *pool, xsdk_xlogic_wire_handle *handle, uint8_t **data);
bool xsdk_xlogic_wire_pool_publish(xsdk_xlogic_wire_pool *pool, xsdk_xlogic_wire_handle handle, size_t length, uint64_t sequence);
bool xsdk_xlogic_wire_pool_acquire(xsdk_xlogic_wire_pool *pool,
                                   xsdk_xlogic_wire_handle *handle,
                                   const uint8_t **data,
                                   size_t *length,
                                   uint64_t *sequence);
bool xsdk_xlogic_wire_pool_release(xsdk_xlogic_wire_pool *pool, xsdk_xlogic_wire_handle handle);
void xsdk_xlogic_wire_pool_close(xsdk_xlogic_wire_pool *pool);
bool xsdk_xlogic_wire_pool_is_drained(const xsdk_xlogic_wire_pool *pool);

#endif
