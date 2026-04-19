/*
 * ztree_nlt.h  –  Node Location Table (NLT): NodeID → (zone_id, slot_id)
 *
 * Lock-free reads (nlt_lookup), write-lock protected updates/removes.
 */

#pragma once

#include "ztree_types.h"

/* ───────────────────────────────────────────────────────────────────────────
 * NLT location tuple
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

    /* ── Lock contention profile (global wrlock) ───────────────────── */
    _Atomic(uint64_t) prof_wait_ns_sum;     /* Σ time blocked waiting for wrlock */
    _Atomic(uint64_t) prof_hold_ns_sum;     /* Σ time wrlock was held            */
    _Atomic(uint64_t) prof_acquire_count;   /* # of wrlock acquisitions          */
    _Atomic(uint64_t) prof_max_wait_ns;     /* tail (worst single wait)          */
} nlt_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

/* nlt_init  –  initialise the NLT (capacity rounded to next power of two). */
void nlt_init(nlt_t *nlt, size_t initial_cap);

/*
 * nlt_destroy  –  release all NLT resources.
 */
void nlt_destroy(nlt_t *nlt);

/* nlt_lookup  –  lock-free lookup. Pass zone_id hint when known.
 * Returns 1 + fills *out on success, 0 if not found. */
int nlt_lookup(nlt_t *nlt, const nlt_location_t *query, nlt_location_t *out);

/* nlt_update  –  insert or update location; auto-grows at 70% load. */
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
