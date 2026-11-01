/* Copyright (C) 2026 alambe94. SPDX-License-Identifier: GPL-3.0-or-later */
#include "xlogic_wire_pool.h"

#include <string.h>

static bool separate(const void *a, size_t a_size, const void *b, size_t b_size)
{
    const uintptr_t left = (uintptr_t)a;
    const uintptr_t right = (uintptr_t)b;
    if (a_size > UINTPTR_MAX - left || b_size > UINTPTR_MAX - right)
    {
        return false;
    }
    return left + a_size <= right || right + b_size <= left;
}

bool xsdk_xlogic_wire_pool_init(xsdk_xlogic_wire_pool *pool,
                                uint8_t *storage,
                                size_t storage_bytes,
                                size_t block_size,
                                xsdk_xlogic_wire_block *blocks,
                                size_t *fifo,
                                size_t capacity)
{
    size_t bytes, blocks_bytes, fifo_bytes;
    if (!pool || !storage || !blocks || !fifo || !capacity || !block_size || capacity > SIZE_MAX / block_size ||
        capacity > SIZE_MAX / sizeof(*blocks) || capacity > SIZE_MAX / sizeof(*fifo) || (uintptr_t)pool % _Alignof(xsdk_xlogic_wire_pool) ||
        (uintptr_t)blocks % _Alignof(xsdk_xlogic_wire_block) || (uintptr_t)fifo % _Alignof(size_t))
    {
        return false;
    }
    bytes = capacity * block_size;
    blocks_bytes = capacity * sizeof(*blocks);
    fifo_bytes = capacity * sizeof(*fifo);
    if (storage_bytes < bytes || !separate(storage, bytes, blocks, blocks_bytes) || !separate(storage, bytes, fifo, fifo_bytes) ||
        !separate(blocks, blocks_bytes, fifo, fifo_bytes) || !separate(pool, sizeof(*pool), storage, bytes) ||
        !separate(pool, sizeof(*pool), blocks, blocks_bytes) || !separate(pool, sizeof(*pool), fifo, fifo_bytes))
    {
        return false;
    }
    memset(blocks, 0, blocks_bytes);
    *pool = (xsdk_xlogic_wire_pool){storage, block_size, capacity, blocks, fifo, 0, 0, false};
    return true;
}

bool xsdk_xlogic_wire_pool_reserve(xsdk_xlogic_wire_pool *pool, xsdk_xlogic_wire_handle *handle, uint8_t **data)
{
    size_t i;
    if (!pool || !handle || !data || pool->is_closed)
    {
        return false;
    }
    for (i = 0; i < pool->capacity; ++i)
    {
        if (pool->blocks[i].state == xSDK_xLOGIC_BLOCK_FREE && pool->blocks[i].generation != UINT64_MAX)
        {
            pool->blocks[i].state = xSDK_xLOGIC_BLOCK_RESERVED;
            ++pool->blocks[i].generation;
            *handle = (xsdk_xlogic_wire_handle){i, pool->blocks[i].generation};
            *data = pool->storage + i * pool->block_size;
            return true;
        }
    }
    return false;
}

bool xsdk_xlogic_wire_pool_publish(xsdk_xlogic_wire_pool *pool, xsdk_xlogic_wire_handle handle, size_t length, uint64_t sequence)
{
    const size_t index = handle.index;
    size_t tail;
    if (!pool || index >= pool->capacity || pool->blocks[index].state != xSDK_xLOGIC_BLOCK_RESERVED ||
        pool->blocks[index].generation != handle.generation)
    {
        return false;
    }
    if (pool->is_closed)
    {
        pool->blocks[index].state = xSDK_xLOGIC_BLOCK_FREE;
        return false;
    }
    if (!length || length > pool->block_size || pool->count >= pool->capacity)
    {
        return false;
    }
    tail = pool->head + pool->count;
    if (tail >= pool->capacity)
    {
        tail -= pool->capacity;
    }
    pool->fifo[tail] = index;
    pool->blocks[index].length = length;
    pool->blocks[index].sequence = sequence;
    pool->blocks[index].state = xSDK_xLOGIC_BLOCK_QUEUED;
    ++pool->count;
    return true;
}

bool xsdk_xlogic_wire_pool_acquire(xsdk_xlogic_wire_pool *pool,
                                   xsdk_xlogic_wire_handle *handle,
                                   const uint8_t **data,
                                   size_t *length,
                                   uint64_t *sequence)
{
    size_t next;
    if (!pool || !handle || !data || !length || !sequence || !pool->count)
    {
        return false;
    }
    next = pool->fifo[pool->head];
    if (next >= pool->capacity || pool->blocks[next].state != xSDK_xLOGIC_BLOCK_QUEUED)
    {
        return false;
    }
    pool->head = pool->head + 1 == pool->capacity ? 0 : pool->head + 1;
    --pool->count;
    pool->blocks[next].state = xSDK_xLOGIC_BLOCK_ACQUIRED;
    *handle = (xsdk_xlogic_wire_handle){next, pool->blocks[next].generation};
    *data = pool->storage + next * pool->block_size;
    *length = pool->blocks[next].length;
    *sequence = pool->blocks[next].sequence;
    return true;
}

bool xsdk_xlogic_wire_pool_release(xsdk_xlogic_wire_pool *pool, xsdk_xlogic_wire_handle handle)
{
    const size_t index = handle.index;
    if (!pool || index >= pool->capacity || pool->blocks[index].generation != handle.generation ||
        (pool->blocks[index].state != xSDK_xLOGIC_BLOCK_RESERVED && pool->blocks[index].state != xSDK_xLOGIC_BLOCK_ACQUIRED))
    {
        return false;
    }
    pool->blocks[index].state = xSDK_xLOGIC_BLOCK_FREE;
    return true;
}

void xsdk_xlogic_wire_pool_close(xsdk_xlogic_wire_pool *pool)
{
    if (!pool)
    {
        return;
    }
    pool->is_closed = true;
    while (pool->count)
    {
        const size_t index = pool->fifo[pool->head];
        pool->blocks[index].state = xSDK_xLOGIC_BLOCK_FREE;
        pool->head = pool->head + 1 == pool->capacity ? 0 : pool->head + 1;
        --pool->count;
    }
}

bool xsdk_xlogic_wire_pool_is_drained(const xsdk_xlogic_wire_pool *pool)
{
    size_t i;
    if (!pool || pool->count)
    {
        return false;
    }
    for (i = 0; i < pool->capacity; ++i)
    {
        if (pool->blocks[i].state != xSDK_xLOGIC_BLOCK_FREE)
        {
            return false;
        }
    }
    return true;
}
