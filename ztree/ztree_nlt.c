#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "ztree_nlt.h"

/* ───────────────────────────────────────────────────────────────────────────
 * Lock contention profiling (alloc_lock only — hot path is lock-free)
 * ─────────────────────────────────────────────────────────────────────────── */

static inline uint64_t nlt_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void nlt_update_max(_Atomic(uint64_t) *target, uint64_t sample)
{
    uint64_t cur = atomic_load_explicit(target, memory_order_relaxed);
    while (sample > cur)
    {
        if (atomic_compare_exchange_weak_explicit(target, &cur, sample,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return;
    }
}

static inline void nlt_record_lock(nlt_t *nlt,
                                   uint64_t wait_ns,
                                   uint64_t hold_ns)
{
    atomic_fetch_add_explicit(&nlt->prof_wait_ns_sum, wait_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&nlt->prof_hold_ns_sum, hold_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&nlt->prof_acquire_count, 1, memory_order_relaxed);
    nlt_update_max(&nlt->prof_max_wait_ns, wait_ns);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Hashing & helpers
 * ─────────────────────────────────────────────────────────────────────────── */

static inline uint64_t nlt_hash_node(ztree_node_id_t id)
{
    uint64_t x = id;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 33;
    return x;
}

static inline uint64_t nlt_hash_zone(uint32_t zone_id)
{
    return nlt_hash_node((ztree_node_id_t)zone_id ^ 0x9e3779b9U);
}

static size_t next_pow2(size_t n)
{
    if (n == 0)
        return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

static inline nlt_slot_t *nlt_zone_tracker(const nlt_zone_entry_t *zone)
{
    return (nlt_slot_t *)atomic_load_explicit(&zone->tracker,
                                              memory_order_acquire);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Zone-bucket probing
 * ─────────────────────────────────────────────────────────────────────────── */

/* Locate or claim the bucket for zone_id.  Reader-friendly:
 *   - returns the bucket if zone_id is already attached (no lock)
 *   - if create_alloc_lock is held by caller, may publish a new bucket */
static nlt_zone_entry_t *nlt_probe_zone_atomic(nlt_t *nlt, uint32_t zone_id)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
        return NULL;

    size_t cap  = nlt->capacity;
    size_t mask = cap - 1;
    size_t pos  = (size_t)(nlt_hash_zone(zone_id) & (uint64_t)mask);

    /* Bound the probe at capacity to be safe even under heavy collision. */
    for (size_t i = 0; i < cap; i++)
    {
        uint32_t cur = atomic_load_explicit(&nlt->zones[pos].zone_id,
                                            memory_order_acquire);
        if (cur == zone_id)
            return &nlt->zones[pos];
        if (cur == ZTREE_INVALID_ZONE_ID)
            return NULL;
        pos = (pos + 1) & mask;
    }
    return NULL;
}

/* Slow path: ensure a bucket exists for zone_id.  Allocates the tracker
 * and atomically publishes zone_id.  Single-threaded under alloc_lock. */
static nlt_zone_entry_t *nlt_attach_zone(nlt_t *nlt, uint32_t zone_id)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
        return NULL;

    nlt_zone_entry_t *existing = nlt_probe_zone_atomic(nlt, zone_id);
    if (existing)
        return existing;

    /* Allocate-and-publish under alloc_lock. */
    uint64_t lock_t0 = nlt_monotonic_ns();
    pthread_mutex_lock(&nlt->alloc_lock);
    uint64_t lock_t1 = nlt_monotonic_ns();

    /* Re-check under lock — another thread may have attached the bucket. */
    existing = nlt_probe_zone_atomic(nlt, zone_id);
    if (existing)
    {
        uint64_t lock_t2 = nlt_monotonic_ns();
        pthread_mutex_unlock(&nlt->alloc_lock);
        nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
        return existing;
    }

    /* Find an empty slot in zones[].  zones[] is fixed-size, so callers
     * must size it well above max attached zones. */
    size_t cap  = nlt->capacity;
    size_t mask = cap - 1;
    size_t pos  = (size_t)(nlt_hash_zone(zone_id) & (uint64_t)mask);
    size_t target = SIZE_MAX;
    for (size_t i = 0; i < cap; i++)
    {
        uint32_t cur = atomic_load_explicit(&nlt->zones[pos].zone_id,
                                            memory_order_relaxed);
        if (cur == ZTREE_INVALID_ZONE_ID)
        {
            target = pos;
            break;
        }
        pos = (pos + 1) & mask;
    }
    if (target == SIZE_MAX)
    {
        fprintf(stderr,
                "nlt_attach_zone: zones[] full (capacity=%zu) for zone_id=%u\n",
                cap, zone_id);
        uint64_t lock_t2 = nlt_monotonic_ns();
        pthread_mutex_unlock(&nlt->alloc_lock);
        nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
        return NULL;
    }

    nlt_zone_entry_t *zone = &nlt->zones[target];

    /* Allocate tracker (zero-initialised → all slots EMPTY). */
    if (atomic_load_explicit(&zone->tracker, memory_order_relaxed) == 0)
    {
        nlt_slot_t *tracker = calloc(nlt->tracker_cap, sizeof(*tracker));
        if (!tracker)
        {
            perror("nlt_attach_zone: calloc tracker");
            uint64_t lock_t2 = nlt_monotonic_ns();
            pthread_mutex_unlock(&nlt->alloc_lock);
            nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
            return NULL;
        }
        zone->tracker_cap = nlt->tracker_cap;
        atomic_store_explicit(&zone->used, 0, memory_order_relaxed);
        atomic_store_explicit(&zone->sealed, 0, memory_order_relaxed);
        /* Publish tracker pointer with release so readers that load
         * zone_id (acquire) see a fully-initialised tracker. */
        atomic_store_explicit(&zone->tracker, (uintptr_t)tracker,
                              memory_order_release);
    }

    /* Publish zone_id last; a successful acquire-load on zone_id implies
     * the tracker pointer / capacity are visible. */
    atomic_store_explicit(&zone->zone_id, zone_id, memory_order_release);
    atomic_fetch_add_explicit(&nlt->used, 1, memory_order_relaxed);

    uint64_t lock_t2 = nlt_monotonic_ns();
    pthread_mutex_unlock(&nlt->alloc_lock);
    nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);

    return zone;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Tracker probing
 * ─────────────────────────────────────────────────────────────────────────── */

/* Lock-free linear probe.  Returns the matching slot value (non-empty,
 * non-tombstone) and writes its position into *out_pos when found.
 * Returns NLT_SLOT_EMPTY if not present. */
static uint64_t nlt_tracker_lookup(nlt_slot_t *tracker, size_t cap,
                                   ztree_node_id_t node_id,
                                   size_t *out_pos)
{
    if (!tracker || cap == 0 || node_id == ZTREE_INVALID_NODE_ID)
        return NLT_SLOT_EMPTY;

    size_t mask = cap - 1;
    size_t pos  = (size_t)(nlt_hash_node(node_id) & (uint64_t)mask);

    for (size_t i = 0; i < cap; i++)
    {
        uint64_t v = atomic_load_explicit(&tracker[pos], memory_order_acquire);
        if (v == NLT_SLOT_EMPTY)
            return NLT_SLOT_EMPTY;                 /* probe ends at empty */
        if (v != NLT_SLOT_TOMBSTONE &&
            nlt_unpack_node(v) == node_id)
        {
            if (out_pos) *out_pos = pos;
            return v;
        }
        pos = (pos + 1) & mask;
    }
    return NLT_SLOT_EMPTY;
}

/* Lock-free insert/update.  Claims an empty / tombstone slot via CAS, or
 * overwrites an existing entry for the same node_id.  Returns 1 on success. */
static int nlt_tracker_publish(nlt_zone_entry_t *zone,
                               ztree_node_id_t node_id,
                               uint32_t slot_id)
{
    nlt_slot_t *tracker = nlt_zone_tracker(zone);
    if (!tracker || zone->tracker_cap == 0)
        return 0;

    size_t cap  = zone->tracker_cap;
    size_t mask = cap - 1;
    size_t pos  = (size_t)(nlt_hash_node(node_id) & (uint64_t)mask);
    uint64_t new_v = nlt_pack(node_id, slot_id);

    for (size_t i = 0; i < cap; i++)
    {
        uint64_t v = atomic_load_explicit(&tracker[pos], memory_order_acquire);

        if (v == NLT_SLOT_EMPTY || v == NLT_SLOT_TOMBSTONE)
        {
            uint64_t expected = v;
            if (atomic_compare_exchange_strong_explicit(
                    &tracker[pos], &expected, new_v,
                    memory_order_release, memory_order_acquire))
            {
                if (v == NLT_SLOT_EMPTY)
                    atomic_fetch_add_explicit(&zone->used, 1,
                                              memory_order_relaxed);
                return 1;
            }
            /* CAS lost — re-read this slot before deciding next step. */
            continue;
        }

        if (nlt_unpack_node(v) == node_id)
        {
            /* Existing entry for this node — atomic store updates slot.
             * Concurrent updaters can race, but flush_page_immediate holds
             * the per-node latch so the same node_id is single-writer here. */
            atomic_store_explicit(&tracker[pos], new_v, memory_order_release);
            return 1;
        }

        pos = (pos + 1) & mask;
    }

    fprintf(stderr,
            "nlt_tracker_publish: tracker full (cap=%zu) zone_id=%u node_id=%u\n",
            cap, atomic_load_explicit(&zone->zone_id, memory_order_relaxed),
            node_id);
    return 0;
}

/* Tombstone the entry for node_id (no-op if absent). */
static void nlt_tracker_tombstone(nlt_zone_entry_t *zone,
                                  ztree_node_id_t node_id)
{
    nlt_slot_t *tracker = nlt_zone_tracker(zone);
    if (!tracker || zone->tracker_cap == 0)
        return;

    size_t cap  = zone->tracker_cap;
    size_t mask = cap - 1;
    size_t pos  = (size_t)(nlt_hash_node(node_id) & (uint64_t)mask);

    for (size_t i = 0; i < cap; i++)
    {
        uint64_t v = atomic_load_explicit(&tracker[pos], memory_order_acquire);
        if (v == NLT_SLOT_EMPTY)
            return;                              /* not present */
        if (v == NLT_SLOT_TOMBSTONE)
        {
            pos = (pos + 1) & mask;
            continue;
        }
        if (nlt_unpack_node(v) == node_id)
        {
            uint64_t expected = v;
            if (atomic_compare_exchange_strong_explicit(
                    &tracker[pos], &expected, NLT_SLOT_TOMBSTONE,
                    memory_order_release, memory_order_acquire))
            {
                atomic_fetch_sub_explicit(&zone->used, 1,
                                          memory_order_relaxed);
                return;
            }
            /* CAS lost — re-read same slot (entry may have been updated). */
            continue;
        }
        pos = (pos + 1) & mask;
    }
}

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

void nlt_init(nlt_t *nlt, size_t zone_capacity_hint, size_t tracker_capacity)
{
    size_t zcap = next_pow2(zone_capacity_hint < 16 ? 16 : zone_capacity_hint);
    size_t tcap = next_pow2(tracker_capacity   < 64 ? 64 : tracker_capacity);

    nlt->zones = calloc(zcap, sizeof(*nlt->zones));
    if (!nlt->zones)
    {
        perror("nlt_init: calloc zones");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < zcap; i++)
    {
        atomic_store_explicit(&nlt->zones[i].zone_id, ZTREE_INVALID_ZONE_ID,
                              memory_order_relaxed);
        atomic_store_explicit(&nlt->zones[i].sealed, 0, memory_order_relaxed);
        atomic_store_explicit(&nlt->zones[i].tracker, (uintptr_t)0,
                              memory_order_relaxed);
        nlt->zones[i].tracker_cap = 0;
        atomic_store_explicit(&nlt->zones[i].used, 0, memory_order_relaxed);
    }

    nlt->capacity     = zcap;
    nlt->tracker_cap  = tcap;
    atomic_store_explicit(&nlt->used, 0, memory_order_relaxed);

    if (pthread_mutex_init(&nlt->alloc_lock, NULL) != 0)
    {
        perror("nlt_init: pthread_mutex_init");
        exit(EXIT_FAILURE);
    }

    atomic_store_explicit(&nlt->prof_wait_ns_sum,   0, memory_order_relaxed);
    atomic_store_explicit(&nlt->prof_hold_ns_sum,   0, memory_order_relaxed);
    atomic_store_explicit(&nlt->prof_acquire_count, 0, memory_order_relaxed);
    atomic_store_explicit(&nlt->prof_max_wait_ns,   0, memory_order_relaxed);
}

void nlt_destroy(nlt_t *nlt)
{
    pthread_mutex_destroy(&nlt->alloc_lock);
    if (nlt->zones)
    {
        for (size_t i = 0; i < nlt->capacity; i++)
        {
            nlt_slot_t *t = (nlt_slot_t *)atomic_load_explicit(
                &nlt->zones[i].tracker, memory_order_relaxed);
            free(t);
        }
        free(nlt->zones);
    }
    nlt->zones = NULL;
    nlt->capacity = 0;
    nlt->tracker_cap = 0;
}

int nlt_lookup(nlt_t *nlt, const nlt_location_t *query, nlt_location_t *out)
{
    if (!query || query->node_id == ZTREE_INVALID_NODE_ID)
        return 0;

    /* ── Fast path: probe the bucket pointed at by the parent's hint. */
    if (query->zone_id != ZTREE_INVALID_ZONE_ID)
    {
        nlt_zone_entry_t *zone = nlt_probe_zone_atomic(nlt, query->zone_id);
        if (zone)
        {
            nlt_slot_t *tracker = nlt_zone_tracker(zone);
            uint64_t v = nlt_tracker_lookup(tracker, zone->tracker_cap,
                                            query->node_id, NULL);
            if (v != NLT_SLOT_EMPTY)
            {
                if (out)
                {
                    out->zone_id = query->zone_id;
                    out->node_id = query->node_id;
                    out->slot_id = nlt_unpack_slot(v);
                }
                return 1;
            }
        }
    }

    /* ── Fallback: stale hint or unknown zone — scan every attached bucket.
     * Each iteration is a few atomic loads; with zones[] sized to ≥ 2 ×
     * max attached zones, the typical scan is well under a microsecond. */
    size_t cap = nlt->capacity;
    for (size_t i = 0; i < cap; i++)
    {
        uint32_t zid = atomic_load_explicit(&nlt->zones[i].zone_id,
                                            memory_order_acquire);
        if (zid == ZTREE_INVALID_ZONE_ID)
            continue;
        if (zid == query->zone_id)
            continue; /* already probed in fast path */
        nlt_slot_t *tracker = nlt_zone_tracker(&nlt->zones[i]);
        uint64_t v = nlt_tracker_lookup(tracker, nlt->zones[i].tracker_cap,
                                        query->node_id, NULL);
        if (v != NLT_SLOT_EMPTY)
        {
            if (out)
            {
                out->zone_id = zid;
                out->node_id = query->node_id;
                out->slot_id = nlt_unpack_slot(v);
            }
            return 1;
        }
    }
    return 0;
}

void nlt_update(nlt_t *nlt, const nlt_location_t *entry)
{
    if (!entry || entry->node_id == ZTREE_INVALID_NODE_ID ||
        entry->zone_id == ZTREE_INVALID_ZONE_ID)
        return;

    nlt_zone_entry_t *zone = nlt_probe_zone_atomic(nlt, entry->zone_id);
    if (!zone)
        zone = nlt_attach_zone(nlt, entry->zone_id);
    if (!zone)
        return;

    nlt_tracker_publish(zone, entry->node_id, entry->slot_id);
}

void nlt_update_migrate(nlt_t *nlt,
                        const nlt_location_t *new_entry,
                        uint32_t prev_zone)
{
    if (!new_entry || new_entry->node_id == ZTREE_INVALID_NODE_ID ||
        new_entry->zone_id == ZTREE_INVALID_ZONE_ID)
        return;

    /* 1) Publish the fresh entry first so readers that pick up the
     *    parent's NEW zone_id always find a valid slot. */
    nlt_zone_entry_t *new_zone = nlt_probe_zone_atomic(nlt, new_entry->zone_id);
    if (!new_zone)
        new_zone = nlt_attach_zone(nlt, new_entry->zone_id);
    if (new_zone)
        nlt_tracker_publish(new_zone, new_entry->node_id, new_entry->slot_id);

    /* 2) Tombstone the previous zone's entry (if it differs).  Readers
     *    using the OLD parent's zone_id may still observe the live entry
     *    here until this CAS lands — that is fine; the OLD slot has the
     *    snapshot they expect.  After the CAS, OLD-hint readers fall back
     *    to the scan, which finds the NEW bucket. */
    if (prev_zone != ZTREE_INVALID_ZONE_ID && prev_zone != new_entry->zone_id)
    {
        nlt_zone_entry_t *prev_zone_e = nlt_probe_zone_atomic(nlt, prev_zone);
        if (prev_zone_e)
            nlt_tracker_tombstone(prev_zone_e, new_entry->node_id);
    }

    atomic_thread_fence(memory_order_release);
}

void nlt_remove(nlt_t *nlt, uint32_t zone_id, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID || zone_id == ZTREE_INVALID_ZONE_ID)
        return;

    nlt_zone_entry_t *zone = nlt_probe_zone_atomic(nlt, zone_id);
    if (!zone)
        return;

    nlt_tracker_tombstone(zone, node_id);
}

void nlt_set_zone_sealed(nlt_t *nlt, uint32_t zone_id, bool sealed)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
        return;

    nlt_zone_entry_t *zone = nlt_probe_zone_atomic(nlt, zone_id);
    if (!zone)
        zone = nlt_attach_zone(nlt, zone_id);
    if (!zone)
        return;

    atomic_store_explicit(&zone->sealed, sealed ? 1 : 0,
                          memory_order_release);
}

int nlt_zone_is_sealed(const nlt_t *nlt, uint32_t zone_id)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
        return 0;

    nlt_zone_entry_t *zone = nlt_probe_zone_atomic((nlt_t *)nlt, zone_id);
    if (!zone)
        return 0;
    return (int)atomic_load_explicit(&zone->sealed, memory_order_acquire);
}

void nlt_sync_zone(nlt_t *nlt, uint32_t zone_id)
{
    (void)nlt;
    (void)zone_id;

    if (zone_id == ZTREE_INVALID_ZONE_ID)
        return;

    atomic_thread_fence(memory_order_release);
}

void nlt_zone_for_each(nlt_t *nlt, uint32_t zone_id,
                       void (*cb)(ztree_node_id_t, uint32_t, void *),
                       void *ctx)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID || !cb)
        return;
    nlt_zone_entry_t *zone = nlt_probe_zone_atomic(nlt, zone_id);
    if (!zone)
        return;
    nlt_slot_t *tracker = nlt_zone_tracker(zone);
    if (!tracker)
        return;
    size_t cap = zone->tracker_cap;
    for (size_t i = 0; i < cap; i++)
    {
        uint64_t v = atomic_load_explicit(&tracker[i], memory_order_acquire);
        if (v == NLT_SLOT_EMPTY || v == NLT_SLOT_TOMBSTONE)
            continue;
        cb(nlt_unpack_node(v), nlt_unpack_slot(v), ctx);
    }
}
