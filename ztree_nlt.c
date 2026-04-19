#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "ztree_nlt.h"

/* ───────────────────────────────────────────────────────────────────────────
 * Lock contention profiling helpers (wrlock wait/hold/count/max)
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
 * Internal helpers
 * ─────────────────────────────────────────────────────────────────────────── */

static inline uint64_t nlt_hash(ztree_node_id_t id)
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
    return nlt_hash((ztree_node_id_t)zone_id ^ 0x9e3779b9U);
}

static size_t next_pow2(size_t n)
{
    if (n == 0)
    {
        return 1;
    }
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

static void nlt_tracker_reset(nlt_tracker_t *tracker, size_t capacity)
{
    size_t cap = next_pow2(capacity < 16 ? 16 : capacity);
    tracker->entries = calloc(cap, sizeof(*tracker->entries));
    if (!tracker->entries)
    {
        perror("nlt_tracker_reset: calloc");
        exit(EXIT_FAILURE);
    }
    tracker->capacity = cap;
    atomic_store_explicit(&tracker->used, 0, memory_order_relaxed);
}

static void nlt_tracker_destroy(nlt_tracker_t *tracker)
{
    free(tracker->entries);
    tracker->entries = NULL;
    tracker->capacity = 0;
    atomic_store_explicit(&tracker->used, 0, memory_order_relaxed);
}

static size_t nlt_probe_zone(const nlt_zone_entry_t *zones, size_t cap,
                             uint32_t zone_id)
{
    size_t mask = cap - 1;
    size_t pos = (size_t)(nlt_hash_zone(zone_id) & (uint64_t)mask);

    for (;;)
    {
        uint32_t cur = zones[pos].zone_id;
        if (cur == ZTREE_INVALID_ZONE_ID || cur == zone_id)
        {
            return pos;
        }
        pos = (pos + 1) & mask;
    }
}

static size_t nlt_probe_tracker(const nlt_tracker_t *tracker, size_t cap,
                                ztree_node_id_t node_id)
{
    size_t mask = cap - 1;
    size_t pos = (size_t)(nlt_hash(node_id) & (uint64_t)mask);

    for (;;)
    {
        ztree_node_id_t cur = tracker->entries[pos].node_id;
        if (cur == ZTREE_INVALID_NODE_ID || cur == node_id)
        {
            return pos;
        }
        pos = (pos + 1) & mask;
    }
}

static void nlt_zone_init_empty(nlt_zone_entry_t *zone)
{
    zone->zone_id = ZTREE_INVALID_ZONE_ID;
    zone->sealed = 0;
    zone->_pad[0] = 0;
    zone->_pad[1] = 0;
    zone->_pad[2] = 0;
    zone->tracker.entries = NULL;
    zone->tracker.capacity = 0;
    atomic_store_explicit(&zone->tracker.used, 0, memory_order_relaxed);
}

static void nlt_grow_zones(nlt_t *nlt, size_t new_cap)
{
    size_t cap = next_pow2(new_cap < 16 ? 16 : new_cap);
    nlt_zone_entry_t *new_zones = calloc(cap, sizeof(*new_zones));
    if (!new_zones)
    {
        perror("nlt_grow_zones: calloc");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < cap; i++)
    {
        nlt_zone_init_empty(&new_zones[i]);
    }

    for (size_t i = 0; i < nlt->capacity; i++)
    {
        if (nlt->zones[i].zone_id == ZTREE_INVALID_ZONE_ID)
        {
            continue;
        }

        size_t pos = nlt_probe_zone(new_zones, cap, nlt->zones[i].zone_id);
        new_zones[pos].zone_id = nlt->zones[i].zone_id;
        new_zones[pos].sealed = nlt->zones[i].sealed;
        nlt_tracker_reset(&new_zones[pos].tracker,
                          nlt->zones[i].tracker.capacity);

        for (size_t j = 0; j < nlt->zones[i].tracker.capacity; j++)
        {
            ztree_node_id_t node_id = nlt->zones[i].tracker.entries[j].node_id;
            if (node_id == ZTREE_INVALID_NODE_ID)
            {
                continue;
            }
            size_t tpos = nlt_probe_tracker(&new_zones[pos].tracker,
                                            new_zones[pos].tracker.capacity,
                                            node_id);
            new_zones[pos].tracker.entries[tpos] = nlt->zones[i].tracker.entries[j];
        }
        atomic_store_explicit(&new_zones[pos].tracker.used,
                              atomic_load_explicit(&nlt->zones[i].tracker.used,
                                                   memory_order_relaxed),
                              memory_order_relaxed);
    }

    for (size_t i = 0; i < nlt->capacity; i++)
    {
        if (nlt->zones[i].zone_id == ZTREE_INVALID_ZONE_ID)
        {
            continue;
        }
        nlt_tracker_destroy(&nlt->zones[i].tracker);
    }

    free(nlt->zones);
    nlt->zones = new_zones;
    nlt->capacity = cap;
    atomic_fetch_add_explicit(&nlt->generation, 1, memory_order_release);
}

static nlt_zone_entry_t *nlt_find_zone_entry(nlt_t *nlt, uint32_t zone_id,
                                             int create)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
    {
        return NULL;
    }

    size_t pos = nlt_probe_zone(nlt->zones, nlt->capacity, zone_id);
    if (nlt->zones[pos].zone_id == zone_id)
    {
        return &nlt->zones[pos];
    }

    if (!create)
    {
        return NULL;
    }

    size_t used = atomic_load_explicit(&nlt->used, memory_order_relaxed);
    if ((used + 1) * 10 >= nlt->capacity * 7)
    {
        nlt_grow_zones(nlt, nlt->capacity * 2);
        pos = nlt_probe_zone(nlt->zones, nlt->capacity, zone_id);
    }

    if (nlt->zones[pos].zone_id == ZTREE_INVALID_ZONE_ID)
    {
        nlt->zones[pos].zone_id = zone_id;
        nlt->zones[pos].sealed = 0;
        nlt_tracker_reset(&nlt->zones[pos].tracker, 16);
        atomic_fetch_add_explicit(&nlt->used, 1, memory_order_relaxed);
    }

    return &nlt->zones[pos];
}

static void nlt_tracker_grow_if_needed(nlt_zone_entry_t *zone)
{
    size_t used = atomic_load_explicit(&zone->tracker.used, memory_order_relaxed);
    if ((used + 1) * 10 < zone->tracker.capacity * 7)
    {
        return;
    }

    nlt_tracker_t new_tracker;
    nlt_tracker_reset(&new_tracker, zone->tracker.capacity * 2);

    for (size_t i = 0; i < zone->tracker.capacity; i++)
    {
        ztree_node_id_t node_id = zone->tracker.entries[i].node_id;
        if (node_id == ZTREE_INVALID_NODE_ID)
        {
            continue;
        }
        size_t pos = nlt_probe_tracker(&new_tracker, new_tracker.capacity, node_id);
        new_tracker.entries[pos] = zone->tracker.entries[i];
    }
    atomic_store_explicit(&new_tracker.used,
                          atomic_load_explicit(&zone->tracker.used, memory_order_relaxed),
                          memory_order_relaxed);

    free(zone->tracker.entries);
    zone->tracker = new_tracker;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

void nlt_init(nlt_t *nlt, size_t initial_cap)
{
    size_t cap = next_pow2(initial_cap < 16 ? 16 : initial_cap);

    nlt->zones = calloc(cap, sizeof(*nlt->zones));
    if (!nlt->zones)
    {
        perror("nlt_init: calloc");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < cap; i++)
    {
        nlt_zone_init_empty(&nlt->zones[i]);
    }

    nlt->capacity = cap;
    atomic_store_explicit(&nlt->used, 0, memory_order_relaxed);
    atomic_store_explicit(&nlt->generation, 0, memory_order_relaxed);

    /* Lock profile counters */
    atomic_store_explicit(&nlt->prof_wait_ns_sum,   0, memory_order_relaxed);
    atomic_store_explicit(&nlt->prof_hold_ns_sum,   0, memory_order_relaxed);
    atomic_store_explicit(&nlt->prof_acquire_count, 0, memory_order_relaxed);
    atomic_store_explicit(&nlt->prof_max_wait_ns,   0, memory_order_relaxed);

    if (pthread_rwlock_init(&nlt->grow_lock, NULL) != 0)
    {
        perror("nlt_init: pthread_rwlock_init");
        exit(EXIT_FAILURE);
    }
}

void nlt_destroy(nlt_t *nlt)
{
    pthread_rwlock_destroy(&nlt->grow_lock);
    if (nlt->zones)
    {
        for (size_t i = 0; i < nlt->capacity; i++)
        {
            if (nlt->zones[i].zone_id == ZTREE_INVALID_ZONE_ID)
            {
                continue;
            }
            nlt_tracker_destroy(&nlt->zones[i].tracker);
        }
    }
    free(nlt->zones);
    nlt->zones = NULL;
    nlt->capacity = 0;
}

int nlt_lookup(nlt_t *nlt, const nlt_location_t *query, nlt_location_t *out)
{
    if (!query || query->node_id == ZTREE_INVALID_NODE_ID)
    {
        return 0;
    }

    for (int attempt = 0; attempt < 10; attempt++)
    {
        uint64_t gen_before = atomic_load_explicit(&nlt->generation,
                                                   memory_order_acquire);
        nlt_zone_entry_t *zones = nlt->zones;
        size_t capacity = nlt->capacity;

        nlt_location_t result;
        memset(&result, 0, sizeof(result));
        int found = 0;

        if (query->zone_id != ZTREE_INVALID_ZONE_ID)
        {
            size_t zpos = nlt_probe_zone(zones, capacity, query->zone_id);
            if (zones[zpos].zone_id == query->zone_id &&
                zones[zpos].tracker.capacity > 0)
            {
                size_t tpos = nlt_probe_tracker(&zones[zpos].tracker,
                                                zones[zpos].tracker.capacity,
                                                query->node_id);
                if (zones[zpos].tracker.entries[tpos].node_id == query->node_id)
                {
                    result.zone_id = zones[zpos].zone_id;
                    result.node_id = query->node_id;
                    result.slot_id = zones[zpos].tracker.entries[tpos].slot_id;
                    found = 1;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < capacity; i++)
            {
                if (zones[i].zone_id == ZTREE_INVALID_ZONE_ID ||
                    zones[i].tracker.capacity == 0)
                {
                    continue;
                }
                size_t tpos = nlt_probe_tracker(&zones[i].tracker,
                                                zones[i].tracker.capacity,
                                                query->node_id);
                if (zones[i].tracker.entries[tpos].node_id == query->node_id)
                {
                    result.zone_id = zones[i].zone_id;
                    result.node_id = query->node_id;
                    result.slot_id = zones[i].tracker.entries[tpos].slot_id;
                    found = 1;
                    break;
                }
            }
        }

        uint64_t gen_after = atomic_load_explicit(&nlt->generation,
                                                  memory_order_acquire);
        if (gen_before != gen_after)
        {
            continue;
        }

        if (found)
        {
            if (out)
            {
                *out = result;
            }
            return 1;
        }
        return 0;
    }

    fprintf(stderr, "nlt_lookup: excessive resize contention for node_id=%u\n",
            query ? query->node_id : 0U);
    return 0;
}

void nlt_update(nlt_t *nlt, const nlt_location_t *entry)
{
    if (!entry || entry->node_id == ZTREE_INVALID_NODE_ID ||
        entry->zone_id == ZTREE_INVALID_ZONE_ID)
    {
        return;
    }

    uint64_t lock_t0 = nlt_monotonic_ns();
    pthread_rwlock_wrlock(&nlt->grow_lock);
    uint64_t lock_t1 = nlt_monotonic_ns();

    nlt_zone_entry_t *zone = nlt_find_zone_entry(nlt, entry->zone_id, 1);
    if (!zone)
    {
        uint64_t lock_t2 = nlt_monotonic_ns();
        pthread_rwlock_unlock(&nlt->grow_lock);
        nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
        return;
    }

    nlt_tracker_grow_if_needed(zone);

    size_t pos = nlt_probe_tracker(&zone->tracker, zone->tracker.capacity,
                                   entry->node_id);
    if (zone->tracker.entries[pos].node_id == ZTREE_INVALID_NODE_ID)
    {
        atomic_fetch_add_explicit(&zone->tracker.used, 1, memory_order_relaxed);
    }

    zone->tracker.entries[pos].node_id = entry->node_id;
    zone->tracker.entries[pos].slot_id = entry->slot_id;

    atomic_fetch_add_explicit(&nlt->generation, 1, memory_order_release);
    uint64_t lock_t2 = nlt_monotonic_ns();
    pthread_rwlock_unlock(&nlt->grow_lock);
    nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
}

void nlt_remove(nlt_t *nlt, uint32_t zone_id, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID || zone_id == ZTREE_INVALID_ZONE_ID)
    {
        return;
    }

    uint64_t lock_t0 = nlt_monotonic_ns();
    pthread_rwlock_wrlock(&nlt->grow_lock);
    uint64_t lock_t1 = nlt_monotonic_ns();

    nlt_zone_entry_t *zone = nlt_find_zone_entry(nlt, zone_id, 0);
    if (!zone || zone->tracker.capacity == 0)
    {
        uint64_t lock_t2 = nlt_monotonic_ns();
        pthread_rwlock_unlock(&nlt->grow_lock);
        nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
        return;
    }

    size_t cap = zone->tracker.capacity;
    size_t mask = cap - 1;
    size_t pos = nlt_probe_tracker(&zone->tracker, cap, node_id);

    if (zone->tracker.entries[pos].node_id != node_id)
    {
        uint64_t lock_t2 = nlt_monotonic_ns();
        pthread_rwlock_unlock(&nlt->grow_lock);
        nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
        return;
    }

    zone->tracker.entries[pos].node_id = ZTREE_INVALID_NODE_ID;
    zone->tracker.entries[pos].slot_id = 0;
    atomic_fetch_sub_explicit(&zone->tracker.used, 1, memory_order_relaxed);

    size_t next = (pos + 1) & mask;
    while (zone->tracker.entries[next].node_id != ZTREE_INVALID_NODE_ID)
    {
        nlt_tracker_entry_t displaced = zone->tracker.entries[next];
        zone->tracker.entries[next].node_id = ZTREE_INVALID_NODE_ID;
        zone->tracker.entries[next].slot_id = 0;
        atomic_fetch_sub_explicit(&zone->tracker.used, 1, memory_order_relaxed);

        size_t reinsert_pos = nlt_probe_tracker(&zone->tracker, cap,
                                                displaced.node_id);
        zone->tracker.entries[reinsert_pos] = displaced;
        atomic_fetch_add_explicit(&zone->tracker.used, 1, memory_order_relaxed);

        next = (next + 1) & mask;
    }

    atomic_fetch_add_explicit(&nlt->generation, 1, memory_order_release);
    uint64_t lock_t2 = nlt_monotonic_ns();
    pthread_rwlock_unlock(&nlt->grow_lock);
    nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
}

void nlt_set_zone_sealed(nlt_t *nlt, uint32_t zone_id, bool sealed)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
    {
        return;
    }

    uint64_t lock_t0 = nlt_monotonic_ns();
    pthread_rwlock_wrlock(&nlt->grow_lock);
    uint64_t lock_t1 = nlt_monotonic_ns();
    nlt_zone_entry_t *zone = nlt_find_zone_entry(nlt, zone_id, 1);
    if (zone)
    {
        zone->sealed = sealed ? 1 : 0;
        atomic_fetch_add_explicit(&nlt->generation, 1, memory_order_release);
    }
    uint64_t lock_t2 = nlt_monotonic_ns();
    pthread_rwlock_unlock(&nlt->grow_lock);
    nlt_record_lock(nlt, lock_t1 - lock_t0, lock_t2 - lock_t1);
}

int nlt_zone_is_sealed(const nlt_t *nlt, uint32_t zone_id)
{
    if (zone_id == ZTREE_INVALID_ZONE_ID)
    {
        return 0;
    }

    uint64_t gen_before = atomic_load_explicit(&nlt->generation,
                                               memory_order_acquire);
    size_t pos = nlt_probe_zone(nlt->zones, nlt->capacity, zone_id);
    int sealed = (nlt->zones[pos].zone_id == zone_id) ? nlt->zones[pos].sealed : 0;
    uint64_t gen_after = atomic_load_explicit(&nlt->generation,
                                              memory_order_acquire);
    if (gen_before != gen_after)
    {
        return 0;
    }
    return sealed;
}

void nlt_sync_zone(nlt_t *nlt, uint32_t zone_id)
{
    (void)nlt;
    (void)zone_id;

    if (zone_id == ZTREE_INVALID_ZONE_ID)
    {
        return;
    }

    atomic_thread_fence(memory_order_release);
}
