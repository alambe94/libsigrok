// Copyright 2026 alambe94
// SPDX-License-Identifier: Apache-2.0

// @file xesk_resource_manager.h
// @brief Portable resource and session manager.

#ifndef xESK_RESOURCE_MANAGER_H
#define xESK_RESOURCE_MANAGER_H

// INCLUDES ///////////////////////////////////////////////////////////////////////
// COMPILER INCLUDES
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// MODULE INCLUDES
#include "xesk.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // PUBLIC API /////////////////////////////////////////////////////////
    // TYPES //////////////////////////////////////////////////////////////////////////
    typedef struct
    {
        uint64_t owner_id;
        uint8_t transport;
        uint8_t owner_kind;
    } xESK_Binding_t;

    typedef struct
    {
        uint64_t boot_id;
        uint64_t session_id;
        uint16_t service_id;
        uint16_t instance_id;
    } xESK_Session_Key_t;

    typedef struct
    {
        xESK_Device_t device;
        const xESK_Service_t *services;
        size_t service_count;
        const xESK_Resource_t *resources;
        size_t resource_count;
        const xESK_Requirement_t *requirements;
        size_t requirement_count;
    } xESK_Catalog_t;

    typedef xRETURN_t (*xESK_Cleanup_t)(void *user, const xESK_Session_t *session);

    // Reduce endpoint transport, modes and optional rate/buffer bounds. Zero
    // bounds mean unspecified. Other properties and the shared catalog stay unchanged.
    // Runs synchronously with the resource manager held against reentry; the
    // callback must not retain its arguments or mutate the manager/catalog.
    typedef void (*xESK_Service_View_t)(void *user,
                                        const xESK_Binding_t *binding,
                                        uint16_t service_id,
                                        uint16_t instance_id,
                                        uint32_t *flags,
                                        uint32_t *transports,
                                        uint64_t *max_rate_hz,
                                        uint64_t *buffer_bytes);

    typedef struct
    {
        xESK_Catalog_t catalog;
        size_t search_states;
        xESK_Cleanup_t on_cleanup;
        void *user;
        xESK_Service_View_t service_view; // Optional read-only endpoint policy; uses user.
        // Reserve one session for FWU admission when explicitly enabled.
        bool reserve_fwu_session;
    } xESK_Resource_Manager_Config_t;

    typedef struct
    {
        uint32_t resource_id;
        uint64_t amount;
        uint64_t setting;
        bool is_exclusive;
    } xESK_Allocation_t;

    typedef struct
    {
        xESK_Session_t session;
        xESK_Binding_t binding;
        size_t allocation_count;
        xRETURN_t cleanup_status;
        bool is_occupied;
    } xESK_Session_Slot_t;

    // Mandatory explicit transaction scratch, including cardinality one. The wire
    // bound limits one service's selected rows, not the application's populations.
    typedef struct
    {
        xESK_Allocation_t candidates[xESK_MAX_REQUIREMENTS];
        size_t candidate_count;
    } xESK_Search_Workspace_t;

    typedef struct
    {
        xESK_Session_Slot_t *sessions;
        size_t session_capacity;
        xESK_Allocation_t *allocations;
        size_t allocation_capacity;
        xESK_Search_Workspace_t *search;
    } xESK_Resource_Manager_Workspace_t;

    typedef struct
    {
        const xESK_Resource_Manager_Config_t *config;
        xESK_Resource_Manager_Workspace_t workspace;
        size_t allocation_stride;
        size_t session_count;
        uint64_t next_session_id;
        uint64_t last_tick;
        uint64_t maintenance_session_id;
        uint64_t device_session_id;
        bool is_initialized;
        bool is_mutating;
    } xESK_Resource_Manager_Context_t;

    typedef struct
    {
        xESK_Binding_t binding;
        uint16_t service_id;
        uint16_t instance_id;
        uint8_t expiry_policy;
        uint64_t lease_ticks;
    } xESK_Claim_Request_t;

    // FUNCTION PROTOTYPES /////////////////////////////////////////////////////////////
    // Read-only geometry/disjointness check for a nonempty external object.
    // Alignment is a nonzero power of two. Reject null, misaligned or wrapping
    // ranges and overlap with this initialized manager's Context, Config, session
    // and allocation arrays, search workspace, or catalog arrays. Initialized
    // Config/workspace geometry remains immutable for the parent's lifetime.
    // This checks neither memory accessibility nor authority, concurrency or
    // physical retirement. Cleanup callbacks may use it during RM mutation.
    xRETURN_t
    xESK_Resource_Manager_Object_Check(const xESK_Resource_Manager_Context_t *context, const void *object, size_t size, size_t alignment);

    // Zero-initialize Context before first Init. Config and catalog arrays remain
    // immutable and alive until Deinit. All workspace storage is caller-provided,
    // module-managed; capacities are immutable while initialized. Allocation capacity
    // must cover session_capacity * max(service.requirement_count), including zero.
    // Ranges must be aligned, valid, disjoint, and independent of callback user state.
    // Init failure changes nothing; Deinit rejects active/unsafe sessions. Successful
    // Deinit clears Context; released workspace contents are unspecified.
    // One serialized executor supplies monotonic ticks. Callbacks may read snapshots
    // but must not mutate this resource manager. Independent authorities are independent.
    // Cleanup receives a terminal snapshot; only xRETURN_OK releases quarantined
    // resources. Release returns that terminal snapshot even when cleanup fails.
    // Input/output objects must be disjoint from retained storage and each other.
    // Failed calls leave output unchanged; a valid mutation may expire due sessions
    // before reporting capacity, resource conflict, terminal-state, or search failure.
    xRETURN_t xESK_Resource_Manager_Init(xESK_Resource_Manager_Context_t *context,
                                         const xESK_Resource_Manager_Config_t *config,
                                         const xESK_Resource_Manager_Workspace_t *workspace);
    xRETURN_t xESK_Resource_Manager_Deinit(xESK_Resource_Manager_Context_t *context);
    xRETURN_t
    xESK_Resource_Manager_Describe(const xESK_Resource_Manager_Context_t *context, uint8_t kind, size_t index, xESK_Record_t *record);
    // Serialized read-only projection. No tick advance, expiry, cleanup, claim or
    // allocation search. ACTIVE and unsafe terminal sessions block even with zero
    // allocations; safe historical tombstones are not current owners. Foreign
    // resource overlap is advisory, never an admission/feasibility promise.
    // No keys, owner IDs, tickets or control credentials are returned.
    xRETURN_t xESK_Resource_Manager_Observe(const xESK_Resource_Manager_Context_t *context,
                                            uint16_t service_id,
                                            uint16_t instance_id,
                                            xESK_Feature_Observation_t *observation);
    xRETURN_t xESK_Resource_Manager_Claim(xESK_Resource_Manager_Context_t *context,
                                          const xESK_Claim_Request_t *request,
                                          uint64_t now_ticks,
                                          xESK_Session_t *session);
    xRETURN_t xESK_Resource_Manager_Renew(xESK_Resource_Manager_Context_t *context,
                                          const xESK_Session_Key_t *key,
                                          const xESK_Binding_t *binding,
                                          uint64_t lease_ticks,
                                          uint64_t now_ticks,
                                          xESK_Session_t *session);
    xRETURN_t xESK_Resource_Manager_Release(xESK_Resource_Manager_Context_t *context,
                                            const xESK_Session_Key_t *key,
                                            const xESK_Binding_t *binding,
                                            uint64_t now_ticks,
                                            xESK_Session_t *session);
    xRETURN_t xESK_Resource_Manager_Get(const xESK_Resource_Manager_Context_t *context,
                                        const xESK_Session_Key_t *key,
                                        const xESK_Binding_t *binding,
                                        xESK_Session_t *session);
    xRETURN_t xESK_Resource_Manager_Expire(xESK_Resource_Manager_Context_t *context, uint64_t now_ticks);
    xRETURN_t xESK_Resource_Manager_Disconnect(xESK_Resource_Manager_Context_t *context, const xESK_Binding_t *binding, uint64_t now_ticks);
    xRETURN_t xESK_Resource_Manager_Reap(xESK_Resource_Manager_Context_t *context,
                                         const xESK_Session_Key_t *key,
                                         const xESK_Binding_t *binding,
                                         uint64_t now_ticks);

    // Snapshot the actual selected allocation for this exact owned session/key.
    // Retains unsafe allocations; never advances time or expires sessions.
    xRETURN_t xESK_Resource_Manager_Get_Allocation(const xESK_Resource_Manager_Context_t *context,
                                                   const xESK_Session_Key_t *key,
                                                   const xESK_Binding_t *binding,
                                                   uint32_t resource_id,
                                                   xESK_Allocation_t *allocation);
    // Caller closes binding admission and drains queued work before retirement.
    // Retry cleanup and reap only SAFE matching sessions; BUSY retains unsafe
    // ownership. Foreign bindings are unchanged except normal deadline expiry.
    xRETURN_t
    xESK_Resource_Manager_Retire_Binding(xESK_Resource_Manager_Context_t *context, const xESK_Binding_t *binding, uint64_t now_ticks);

    // One active SDK USB DEVICE/0 session may reserve a configuration operation.
    // Begin rejects other active or unsafe terminal sessions without cleanup.
    // Success blocks new claims and FWU maintenance. Expiry or disconnect revokes
    // the owner but retains its tombstone and exclusion. A trusted serialized
    // dispatcher calls End only after effects and borrowed references are terminal.
    xRETURN_t xESK_Resource_Manager_Device_Begin(xESK_Resource_Manager_Context_t *context,
                                                 const xESK_Session_Key_t *key,
                                                 const xESK_Binding_t *binding,
                                                 uint64_t now_ticks);
    xRETURN_t xESK_Resource_Manager_Device_End(xESK_Resource_Manager_Context_t *context,
                                               const xESK_Session_Key_t *key,
                                               const xESK_Binding_t *binding,
                                               uint64_t now_ticks);

    // One active SDK FWU session may hold device-wide maintenance exclusion.
    // Begin closes Claim admission before retiring every other session. OK means
    // ownership accepted, not update-ready. A held latch rejects another Begin,
    // including the same key; use Poll to retry existing cleanup. Poll
    // returns OK only while this owner is active and every other session is safe.
    // Expiry/disconnect/release revoke authorization but retain the latch and owner
    // tombstone. End requires that owner terminal and safe, and all other cleanup
    // complete. Only explicit End reopens admission; no takeover or automatic reap.
    xRETURN_t xESK_Resource_Manager_Maintenance_Begin(xESK_Resource_Manager_Context_t *context,
                                                      const xESK_Session_Key_t *key,
                                                      const xESK_Binding_t *binding,
                                                      uint64_t now_ticks);
    xRETURN_t xESK_Resource_Manager_Maintenance_Poll(xESK_Resource_Manager_Context_t *context,
                                                     const xESK_Session_Key_t *key,
                                                     const xESK_Binding_t *binding,
                                                     uint64_t now_ticks);
    xRETURN_t xESK_Resource_Manager_Maintenance_End(xESK_Resource_Manager_Context_t *context,
                                                    const xESK_Session_Key_t *key,
                                                    const xESK_Binding_t *binding,
                                                    uint64_t now_ticks);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // xESK_RESOURCE_MANAGER_H

// EOF /////////////////////////////////////////////////////////////////////////////
