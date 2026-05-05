/*
 * ctree_nlt.h  –  Node Location Table (NLT): NodeID → (zone_id, slot_id)
 *
 * Lock-free reads AND lock-free updates on the hot path.
 *
 * Each zone bucket owns a fixed-size open-addressed tracker whose entries
 * are atomic uint64_t packed as (NodeID:32, SlotID:32).  Tombstones are
 * used for migration removal so chains stay intact for concurrent readers.
 * The only remaining wrlock guards the (rare) first-time bucket allocation.
 */

#pragma once

#include "ctree_types.h"

/* ───────────────────────────────────────────────────────────────────────────
 * NLT location tuple (caller-facing)
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    uint32_t zone_id;        /* current zone for the node              */
    ztree_node_id_t node_id; /* stable node identifier                 */
    uint32_t slot_id;        /* slot within zone (paper: 3 bytes)      */
} nlt_location_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Atomic tracker slot encoding
 *
 *   value == 0                → empty (probe stops)
 *   value == ~0ULL            → tombstone (probe continues)
 *   else: bits [63:32]=NodeID, [31:0]=SlotID
 * ─────────────────────────────────────────────────────────────────────────── */
typedef _Atomic(uint64_t) nlt_slot_t;

#define NLT_SLOT_EMPTY     ((uint64_t)0)
#define NLT_SLOT_TOMBSTONE (~(uint64_t)0)

static inline uint64_t nlt_pack(ztree_node_id_t node_id, uint32_t slot_id)
{
    return ((uint64_t)node_id << 32) | (uint64_t)slot_id;
}
static inline ztree_node_id_t nlt_unpack_node(uint64_t v)
{
    return (ztree_node_id_t)(v >> 32);
}
static inline uint32_t nlt_unpack_slot(uint64_t v)
{
    return (uint32_t)(v & 0xFFFFFFFFu);
}

/* ───────────────────────────────────────────────────────────────────────────
 * NLT zone entry
 *
 * One bucket per attached zone.  The tracker is allocated once on first
 * use and never resized — its capacity is fixed at NLT init time so
 * lookups can read tracker[]/tracker_cap without any synchronisation.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    _Atomic(uint32_t)  zone_id;       /* INVALID_ZONE_ID = empty bucket */
    _Atomic(uint8_t)   sealed;        /* paper sealed bit               */
    uint8_t            _pad0[3];
    _Atomic(uintptr_t) tracker;       /* nlt_slot_t * (atomic for publish) */
    size_t             tracker_cap;   /* fixed once tracker is published */
    _Atomic(size_t)    used;          /* approx live entry count        */
} nlt_zone_entry_t;

/* Aliases retained for ABI parity with the prior version. */
typedef nlt_zone_entry_t ztree_zone_entry_t;

/* ───────────────────────────────────────────────────────────────────────────
 * NLT handle
 *
 * zones[] is sized at init and never grown — making every probe a pure
 * sequence of atomic loads.  Callers must size capacity ≥ max attached
 * zones; fixed at NLT init time so lookups can read zones[]/capacity without
 * any synchronisation.  grow_lock now only serialises the (rare) first-time
 * bucket allocation (memory ordering is on the atomic publish of zone_id).
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    nlt_zone_entry_t *zones;          /* fixed-size open-addressed array */
    size_t            capacity;       /* power-of-two, fixed at init     */
    _Atomic(size_t)   used;           /* live zone bucket count          */
    size_t            tracker_cap;    /* per-zone tracker capacity       */

    pthread_mutex_t   alloc_lock;     /* serialises new-bucket allocation */

    /* ── Lock contention profile (alloc_lock only) ─────────────────── */
    _Atomic(uint64_t) prof_wait_ns_sum;
    _Atomic(uint64_t) prof_hold_ns_sum;
    _Atomic(uint64_t) prof_acquire_count;
    _Atomic(uint64_t) prof_max_wait_ns;
} nlt_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

/* nlt_init  –  initialise the NLT.
 *   zone_capacity_hint: at least max_attached_zones (rounded to power of 2).
 *   tracker_capacity:   per-zone tracker size (rounded to power of 2).
 *                       Must be ≥ max nodes-ever-touched per zone × 2 for
 *                       healthy probe behaviour with tombstones.
 */
void nlt_init(nlt_t *nlt, size_t zone_capacity_hint, size_t tracker_capacity);

/* nlt_destroy  –  release all NLT resources. */
void nlt_destroy(nlt_t *nlt);

/* nlt_lookup  –  lock-free lookup. Pass zone_id hint when known.
 * Returns 1 + fills *out on success, 0 if not found. */
int nlt_lookup(nlt_t *nlt, const nlt_location_t *query, nlt_location_t *out);

/* nlt_update  –  insert or update location for a node.
 * Lock-free unless the bucket for entry->zone_id has not yet been allocated. */
void nlt_update(nlt_t *nlt, const nlt_location_t *entry);

/* nlt_update_migrate  –  insert into new zone + tombstone in previous.
 * Both ops are lock-free CAS; new is published before the old is invalidated
 * so concurrent readers always see at least one valid entry. */
void nlt_update_migrate(nlt_t *nlt,
                        const nlt_location_t *new_entry,
                        uint32_t prev_zone);

/* nlt_remove  –  tombstone the entry for (zone_id, node_id).  No-op if missing. */
void nlt_remove(nlt_t *nlt, uint32_t zone_id, ztree_node_id_t node_id);

/* Mark a zone entry sealed / unsealed. */
void nlt_set_zone_sealed(nlt_t *nlt, uint32_t zone_id, bool sealed);

/* Query whether a zone has been sealed. */
int nlt_zone_is_sealed(const nlt_t *nlt, uint32_t zone_id);

/* Lightweight synchronization hook for paper-aligned SyncNLT points. */
void nlt_sync_zone(nlt_t *nlt, uint32_t zone_id);
