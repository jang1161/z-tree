/*
 * ztree_zone.c  –  Zone allocator implementation
 *
 * ILayer allocation: pure round-robin over a fixed set of zones.
 * LLayer allocation: heat-aware 80/20 hot/cold split.
 *
 * Zone-full detection: when the write pointer of the chosen zone has reached
 * the zone capacity, the allocator marks it full and advances to the next one.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libzbd/zbd.h>

#include "ztree_zone.h"

/* ───────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ─────────────────────────────────────────────────────────────────────────── */

/* Paper-aligned timestamp: compressed to 16-bit milliseconds (mod 65536) */
static inline uint16_t zone_monotonic_ts_16b(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    return (uint16_t)(ms & 0xFFFFU);
}

/*
 * Check whether a zone has room for at least one more page.
 * Returns 1 if the zone is usable, 0 if it is full.
 */
static int zone_has_space(zone_alloc_t *za, uint32_t zone_id)
{
    if (atomic_load_explicit(&za->zone_full[zone_id], memory_order_acquire))
        return 0;

    uint64_t wp    = atomic_load_explicit(&za->zone_wp_bytes[zone_id],
                                          memory_order_acquire);
    uint64_t cap   = za->zones[zone_id].capacity;
    uint64_t start = za->zones[zone_id].start;
    return (wp + ZTREE_PAGE_SIZE <= start + cap) ? 1 : 0;
}

/*
 * Dynamic_Allocation (paper §3.2): round-robin within the active group.
 *
 * Each call advances the counter by one, so T concurrent callers get T
 * different starting zones → inter-zone I/O parallelism.
 *
 * When every zone in the active group is sealed (zone_full=1), the group
 * is expanded by `init_count' idle zones from the pool.  At most one
 * expansion batch is open at a time, so the device's active-zone budget
 * is consumed gradually rather than all at once.
 */
static uint32_t rr_pick_zone(zone_alloc_t *za,
                              uint32_t pool_base,
                              uint32_t pool_size,
                              uint32_t init_count,
                              _Atomic(uint32_t) *group_count,
                              _Atomic(uint32_t) *rr_counter,
                              uint32_t avoid_zone,
                              const char *layer_name)
{
    for (;;) {
        uint32_t count = atomic_load_explicit(group_count, memory_order_acquire);
        uint32_t start = atomic_fetch_add_explicit(rr_counter, 1, memory_order_relaxed);

        for (uint32_t i = 0; i < count; i++) {
            uint32_t zone_id = pool_base + ((start + i) % count);
            if (zone_id == avoid_zone)
                continue;
            if (zone_has_space(za, zone_id))
                return zone_id;
        }

        /* All active group zones are sealed.  Attach the next batch. */
        uint32_t new_count = count + init_count;
        if (new_count > pool_size)
            new_count = pool_size;

        if (new_count == count) {
            fprintf(stderr, "[ztree_zone] all %s zones full – pool exhausted\n",
                    layer_name);
            exit(EXIT_FAILURE);
        }

        /* CAS: only one thread expands; others retry and see new count. */
        uint32_t expected = count;
        if (atomic_compare_exchange_strong_explicit(group_count, &expected,
                                                    new_count,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed))
        {
            /* We won the expansion. Finish every zone in the outgoing group
             * (except avoid_zone, which still has space) so the device's
             * max_active_zones budget frees up before the new batch is
             * implicitly opened by the next pwrite. CLOSED zones still count
             * toward active, so zbd_finish_zones is the only safe release. */
            if (za->fd >= 0) {
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t zid = pool_base + i;
                    if (zid == avoid_zone)
                        continue;
                    /* Serialize with pwriters on this zone. Holding the
                     * write lock ensures any in-flight pwrite completes
                     * before we issue zbd_finish_zones, and any allocator
                     * that picked this zone but has not yet locked will
                     * see zone_full=1 after we release and retry. */
                    if (za->zone_write_locks)
                        pthread_mutex_lock(&za->zone_write_locks[zid]);
                    uint8_t was_full = atomic_exchange_explicit(
                        &za->zone_full[zid], 1, memory_order_acq_rel);
                    if (!was_full) {
                        zbd_finish_zones(za->fd,
                                         (off_t)za->zones[zid].start,
                                         (off_t)za->zone_size);
                    }
                    if (za->zone_write_locks)
                        pthread_mutex_unlock(&za->zone_write_locks[zid]);
                }
            }
        }
    }
}

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

void zone_alloc_init(zone_alloc_t *za,
                     uint32_t ilayer_pool_base, uint32_t ilayer_pool_size,
                     uint32_t ilayer_init_count,
                     uint32_t hot_pool_base, uint32_t hot_pool_size,
                     uint32_t hot_init_count,
                     uint32_t cold_pool_base, uint32_t cold_pool_size,
                     uint32_t cold_init_count,
                     _Atomic(uint8_t)  *zone_full,
                     _Atomic(uint64_t) *zone_wp_bytes,
                     struct zbd_zone   *zones,
                     uint32_t           nr_zones,
                     int                fd,
                     uint64_t           zone_size,
                     pthread_mutex_t   *zone_write_locks)
{
    memset(za, 0, sizeof(*za));

    /* ILayer (IZGroup) */
    za->ilayer_pool_base  = ilayer_pool_base;
    za->ilayer_pool_size  = (ilayer_pool_size  > 0) ? ilayer_pool_size  : 1;
    za->ilayer_init_count = (ilayer_init_count > 0) ? ilayer_init_count : 1;
    atomic_store_explicit(&za->ilayer_group_count, za->ilayer_init_count, memory_order_relaxed);
    atomic_store_explicit(&za->ilayer_rr, 0, memory_order_relaxed);

    /* LLayer hot */
    za->hot_pool_base  = hot_pool_base;
    za->hot_pool_size  = (hot_pool_size  > 0) ? hot_pool_size  : 1;
    za->hot_init_count = (hot_init_count > 0) ? hot_init_count : 1;
    atomic_store_explicit(&za->hot_group_count, za->hot_init_count, memory_order_relaxed);
    atomic_store_explicit(&za->hot_rr,  0, memory_order_relaxed);

    /* LLayer cold */
    za->cold_pool_base  = cold_pool_base;
    za->cold_pool_size  = (cold_pool_size  > 0) ? cold_pool_size  : 1;
    za->cold_init_count = (cold_init_count > 0) ? cold_init_count : 1;
    atomic_store_explicit(&za->cold_group_count, za->cold_init_count, memory_order_relaxed);
    atomic_store_explicit(&za->cold_rr, 0, memory_order_relaxed);

    za->zone_full      = zone_full;
    za->zone_wp_bytes  = zone_wp_bytes;
    za->zones          = zones;
    za->nr_zones       = nr_zones;
    za->fd             = fd;
    za->zone_size      = zone_size;
    za->zone_write_locks = zone_write_locks;
}

void zone_alloc_destroy(zone_alloc_t *za)
{
    (void)za;
    /* zone arrays are owned by ztree_t; we must not free them here */
}

uint32_t zone_alloc_ilayer(zone_alloc_t *za, uint32_t avoid_zone)
{
    return rr_pick_zone(za,
                        za->ilayer_pool_base, za->ilayer_pool_size,
                        za->ilayer_init_count,
                        &za->ilayer_group_count, &za->ilayer_rr,
                        avoid_zone,
                        "ILayer");
}

uint32_t zone_alloc_llayer(zone_alloc_t *za, ztree_node_id_t node_id,
                           uint32_t avoid_zone)
{
    /*
     * Heat-aware placement: hot nodes go to hot zones (denser writes, worse
     * GC amplification) and cold nodes go to cold zones (sparse writes, lower
     * GC amplification).  Within each group, round-robin for parallelism.
     */
    if (zone_is_hot(za, node_id)) {
        return rr_pick_zone(za,
                            za->hot_pool_base, za->hot_pool_size,
                            za->hot_init_count,
                            &za->hot_group_count, &za->hot_rr,
                            avoid_zone,
                            "LLayer-hot");
    } else {
        return rr_pick_zone(za,
                            za->cold_pool_base, za->cold_pool_size,
                            za->cold_init_count,
                            &za->cold_group_count, &za->cold_rr,
                            avoid_zone,
                            "LLayer-cold");
    }
}

void zone_heat_record_write(zone_alloc_t *za, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID)
        return;

    /* Hash into the fixed-size heat table.  Collisions are harmless –
     * the heat estimate for a node may be influenced by another node
     * that maps to the same bucket, but the outcome is just a slightly
     * imprecise hot/cold classification.                                */
    size_t idx = (size_t)(node_id % ZTREE_HEAT_TABLE_SIZE);

    /* Saturate access_count at uint16_t max without taking a global lock. */
    for (;;)
    {
        uint16_t old_cnt = atomic_load_explicit(&za->heat_table[idx].access_count,
                                                memory_order_relaxed);
        if (old_cnt == UINT16_MAX)
            break;
        uint16_t new_cnt = (uint16_t)(old_cnt + 1U);
        if (atomic_compare_exchange_weak_explicit(&za->heat_table[idx].access_count,
                                                  &old_cnt,
                                                  new_cnt,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }

    atomic_store_explicit(&za->heat_table[idx].last_write_ts,
                          zone_monotonic_ts_16b(),
                          memory_order_relaxed);
}

int zone_is_hot(zone_alloc_t *za, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID)
        return 0;

    size_t idx = (size_t)(node_id % ZTREE_HEAT_TABLE_SIZE);

    uint16_t cnt = atomic_load_explicit(&za->heat_table[idx].access_count,
                                        memory_order_relaxed);

    /* Paper-aligned: treat as hot if access_count > ZTREE_HEAT_HOT_THRESHOLD (10) */
    return (cnt > ZTREE_HEAT_HOT_THRESHOLD) ? 1 : 0;
}
