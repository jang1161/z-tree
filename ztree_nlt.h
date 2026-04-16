/*
 * ztree_nlt.h  –  Node Location Table (NLT) interface (Lock-Free Reads)
 *
 * Paper-aligned lock-free NLT for efficient node address resolution.
 * The NLT maintains a mapping:
 *
 *   NodeID  →  (zone_id, slot_id)
 *
 * where:
 *   NodeID   is the stable, globally-unique identifier of a tree node.
 *   zone_id  is the zone that currently holds the node's latest copy.
 *   slot_id  is the slot index within that zone.
 *
 * Physical byte offset  =  zone_start[zone_id] + slot_id × PAGE_SIZE
 *
 * Thread safety:
 *   - nlt_lookup: **lock-free** (atomic reads only)
 *   - nlt_update: write-lock protected (grow/rehash synchronization)
 *   - nlt_remove: write-lock protected
 */

#pragma once

#include "ztree_types.h"

/* ───────────────────────────────────────────────────────────────────────────
 * Paper-aligned NLTable location tuple
 *
 * ZoneID + NodeID identify a tracker bucket, SlotID identifies the latest
 * physical position of that node within the zone.
 * SlotID is stored as uint32_t for implementation simplicity and to avoid
 * the 16-bit range limit in large zones.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    uint32_t zone_id;        /* current zone for the node              */
    ztree_node_id_t node_id; /* stable node identifier                 */
    uint32_t slot_id;        /* slot within zone (paper: 3 bytes)      */
} nlt_location_t;

typedef struct
{
    ztree_node_id_t node_id; /* tracker key                             */
    uint32_t slot_id;        /* tracker value                           */
} nlt_tracker_entry_t;

typedef struct
{
    nlt_tracker_entry_t *entries; /* open-addressed NodeID -> SlotID map */
    size_t capacity;              /* power-of-two size                   */
    _Atomic(size_t) used;         /* live entries                        */
} nlt_tracker_t;

typedef struct
{
    uint32_t zone_id;      /* 2-byte paper field, widened to 32-bit  */
    uint8_t sealed;        /* paper sealed bit                        */
    uint8_t _pad[3];       /* keep alignment predictable              */
    nlt_tracker_t tracker; /* NodeID -> SlotID tracker                */
} nlt_zone_entry_t;

typedef nlt_tracker_t ztree_node_tracker_t;
typedef nlt_zone_entry_t ztree_zone_entry_t;

/* ───────────────────────────────────────────────────────────────────────────
 * NLT handle (lock-free reads, write-lock for grow/rehash)
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    nlt_zone_entry_t *zones;      /* open-addressed ZoneID buckets      */
    size_t capacity;              /* power-of-two zone bucket count     */
    _Atomic(size_t) used;         /* live zone entries                  */
    _Atomic(uint64_t) generation; /* detects concurrent resize/update   */
    pthread_rwlock_t grow_lock;   /* protects grow/rehash + updates     */

    /* ── Lock contention profile (global wrlock) ─────────────────────
     * Recorded around every wrlock acquisition in the public NLT API.
     * Use these to gauge whether the global NLT lock is the bottleneck. */
    _Atomic(uint64_t) prof_wait_ns_sum;     /* Σ time blocked waiting for wrlock */
    _Atomic(uint64_t) prof_hold_ns_sum;     /* Σ time wrlock was held            */
    _Atomic(uint64_t) prof_acquire_count;   /* # of wrlock acquisitions          */
    _Atomic(uint64_t) prof_max_wait_ns;     /* tail (worst single wait)          */
} nlt_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

/*
 * nlt_init  –  initialise the NLT with the given initial capacity.
 * capacity must be > 0; it is rounded up to the next power of two.
 * Exits the process on allocation failure.
 */
void nlt_init(nlt_t *nlt, size_t initial_cap);

/*
 * nlt_destroy  –  release all NLT resources.
 */
void nlt_destroy(nlt_t *nlt);

/*
 * nlt_lookup  –  look up a node's current physical location.
 * Query must supply the zone_id bucket and node_id key.
 * If query->zone_id is ZTREE_INVALID_ZONE_ID, implementations may fall back
 * to a slow path scan, but callers should pass the zone hint whenever known.
 * Returns 1 and fills *out on success, 0 if the node is not in the table.
 */
int nlt_lookup(nlt_t *nlt, const nlt_location_t *query, nlt_location_t *out);

/*
 * nlt_update  –  insert or update the location entry for node_id.
 * If the node is already present, its slot_id is overwritten in place.
 * The zone entry is created on demand and the table grows automatically
 * when the zone-bucket load factor exceeds 70%.
 */
void nlt_update(nlt_t *nlt, const nlt_location_t *entry);

/*
 * nlt_remove  –  remove the entry for node_id (e.g. after a merge / delete).
 * No-op if the zone or node is not present.
 */
void nlt_remove(nlt_t *nlt, uint32_t zone_id, ztree_node_id_t node_id);

/* Mark a zone entry sealed / unsealed.  Sealed zones are treated as write-
 * closed and should be redirected through dynamic allocation. */
void nlt_set_zone_sealed(nlt_t *nlt, uint32_t zone_id, bool sealed);

/* Query whether a zone has been sealed. */
int nlt_zone_is_sealed(const nlt_t *nlt, uint32_t zone_id);

/* Lightweight synchronization hook for paper-aligned SyncNLT points. */
void nlt_sync_zone(nlt_t *nlt, uint32_t zone_id);
