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

#include "ctree_zone.h"

/* ───────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ─────────────────────────────────────────────────────────────────────────── */

/* ms timestamp truncated to 32 bits — wraps at ~50 days (vs 65 s for 16-bit).
 * Eliminates the cyclic-sort artifact that flipped sequential workloads
 * between hot/cold pools every ts wrap. */
static inline uint32_t zone_monotonic_ts_16b(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    return (uint32_t)ms;
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

/* Dynamic_Allocation (paper §3.2): round-robin within the active group.
 * Expands group by 1 when all zones are sealed. */
static uint32_t rr_pick_zone(zone_alloc_t *za,
                              uint32_t pool_base,
                              uint32_t pool_size,
                              uint32_t init_count __attribute__((unused)),
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

        /* All active zones sealed.  Attach 1 replacement (fallback;
         * primary growth is via zone_seal_and_replace). */
        uint32_t new_count = count + 1;
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
            /* Finish outgoing zones under lifecycle_lock. */
            pthread_mutex_lock(&za->lifecycle_lock);
            if (za->fd >= 0) {
                int finished_count = 0;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t zid = pool_base + i;
                    if (zid == avoid_zone)
                        continue;
                    if (za->zone_write_locks)
                        pthread_mutex_lock(&za->zone_write_locks[zid]);
                    atomic_store_explicit(
                        &za->zone_full[zid], 1, memory_order_release);
                    zbd_finish_zones(za->fd,
                                     (off_t)za->zones[zid].start,
                                     (off_t)za->zone_size);
                    if (za->zone_write_locks)
                        pthread_mutex_unlock(&za->zone_write_locks[zid]);
                }
                for (int attempt = 0; attempt < 200; attempt++) {
                    finished_count = 0;
                    for (uint32_t i = 0; i < count; i++) {
                        uint32_t zid = pool_base + i;
                        if (zid == avoid_zone)
                            continue;
                        struct zbd_zone zinfo;
                        unsigned int nz = 1;
                        if (zbd_report_zones(za->fd,
                                             (off_t)za->zones[zid].start,
                                             (off_t)za->zone_size,
                                             ZBD_RO_ALL, &zinfo, &nz) != 0
                            || nz == 0)
                        {
                            finished_count++;
                            continue;
                        }
                        if (zinfo.cond == ZBD_ZONE_COND_FULL) {
                            finished_count++;
                        } else {
                            zbd_finish_zones(za->fd,
                                             (off_t)za->zones[zid].start,
                                             (off_t)za->zone_size);
                        }
                    }
                    if (finished_count >= (int)count)
                        break;
                    usleep(200);
                }
            }
            pthread_mutex_unlock(&za->lifecycle_lock);
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

    /* Percentile-based heat policy state */
    atomic_store_explicit(&za->heat_median_count, 0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_median_ts,    0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_writes_since_recompute, 0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_recompute_in_progress,  0, memory_order_relaxed);

    if (pthread_mutex_init(&za->lifecycle_lock, NULL) != 0)
    {
        perror("zone_alloc_init: lifecycle_lock");
        exit(EXIT_FAILURE);
    }
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
    pthread_mutex_destroy(&za->lifecycle_lock);
    /* zone arrays are owned by ztree_t; we must not free them here */
}

/* ───────────────────────────────────────────────────────────────────────────
 * zone_seal_and_replace  –  finish a sealed zone and grow its group by 1.
 * Serialised by lifecycle_lock; handles async OPEN→FULL transitions.
 * ─────────────────────────────────────────────────────────────────────────── */
void zone_seal_and_replace(zone_alloc_t *za, uint32_t zone_id)
{
    if (za->fd < 0)
        return;

    pthread_mutex_lock(&za->lifecycle_lock);

    /* 0. Dedup: skip if another thread already finished this zone. */
    {
        struct zbd_zone pre_check;
        unsigned int pre_nz = 1;
        if (zbd_report_zones(za->fd,
                             (off_t)za->zones[zone_id].start,
                             (off_t)za->zone_size,
                             ZBD_RO_ALL, &pre_check, &pre_nz) == 0
            && pre_nz > 0
            && pre_check.cond == ZBD_ZONE_COND_FULL)
        {
            /* Already FULL — skip to avoid double-growing. */
            pthread_mutex_unlock(&za->lifecycle_lock);
            return;
        }
    }

    /* 1. Finish the sealed zone (no verify loop; EOVERFLOW retry
     *    in flush_page_immediate handles async transitions). */
    zbd_finish_zones(za->fd,
                     (off_t)za->zones[zone_id].start,
                     (off_t)za->zone_size);

    /* Grow by 1 only if writable zones < init_count (caps OPEN zones).
     * New zone is implicitly opened by its first pwrite. */
    uint32_t pool_base, pool_size, init_cnt;
    _Atomic(uint32_t) *grp_cnt;

    if (zone_id >= za->cold_pool_base)
    {
        pool_base = za->cold_pool_base;
        pool_size = za->cold_pool_size;
        init_cnt  = za->cold_init_count;
        grp_cnt   = &za->cold_group_count;
    }
    else if (zone_id >= za->hot_pool_base)
    {
        pool_base = za->hot_pool_base;
        pool_size = za->hot_pool_size;
        init_cnt  = za->hot_init_count;
        grp_cnt   = &za->hot_group_count;
    }
    else
    {
        pool_base = za->ilayer_pool_base;
        pool_size = za->ilayer_pool_size;
        init_cnt  = za->ilayer_init_count;
        grp_cnt   = &za->ilayer_group_count;
    }

    /* Count non-full zones in this group. */
    uint32_t cur_count = atomic_load_explicit(grp_cnt, memory_order_relaxed);
    uint32_t active = 0;
    for (uint32_t i = 0; i < cur_count; i++)
    {
        uint32_t zid = pool_base + i;
        if (!atomic_load_explicit(&za->zone_full[zid], memory_order_relaxed))
            active++;
    }

    /* Only grow if active < init_count (room for a replacement). */
    if (active < init_cnt && cur_count < pool_size)
    {
        atomic_compare_exchange_strong_explicit(grp_cnt,
            &cur_count, cur_count + 1,
            memory_order_acq_rel, memory_order_relaxed);
    }

    pthread_mutex_unlock(&za->lifecycle_lock);
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
    /* Heat-aware placement: hot → hot zones, cold → cold zones. */
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

/* ───────────────────────────────────────────────────────────────────────────
 * Percentile-based heat policy (paper §3.1.1)
 *
 * Scans heat_table, sorts non-zero (Counter, Timestamp) pairs, stores
 * 50th-percentile medians.  Gated by RECOMPUTE_INTERVAL; one thread at
 * a time.  Bootstrap: defaults to hot until MIN_SAMPLES are observed.
 * ─────────────────────────────────────────────────────────────────────── */

static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a;
    uint16_t y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void recompute_heat_medians(zone_alloc_t *za)
{
    uint16_t *cnts = malloc(ZTREE_HEAT_TABLE_SIZE * sizeof(uint16_t));
    uint32_t *tss  = malloc(ZTREE_HEAT_TABLE_SIZE * sizeof(uint32_t));
    if (!cnts || !tss)
    {
        free(cnts);
        free(tss);
        return;
    }

    size_t n = 0;
    for (size_t i = 0; i < ZTREE_HEAT_TABLE_SIZE; i++)
    {
        uint16_t c = atomic_load_explicit(&za->heat_table[i].access_count,
                                          memory_order_relaxed);
        if (c == 0)
            continue;
        uint32_t t = atomic_load_explicit(&za->heat_table[i].last_write_ts,
                                          memory_order_relaxed);
        cnts[n] = c;
        tss[n]  = t;
        n++;
    }

    if (n < ZTREE_HEAT_MIN_SAMPLES)
    {
        atomic_store_explicit(&za->heat_median_count, 0, memory_order_relaxed);
        atomic_store_explicit(&za->heat_median_ts,    0, memory_order_relaxed);
        free(cnts);
        free(tss);
        return;
    }

    qsort(cnts, n, sizeof(uint16_t), cmp_u16);
    qsort(tss,  n, sizeof(uint32_t), cmp_u32);

    atomic_store_explicit(&za->heat_median_count, cnts[n / 2], memory_order_relaxed);
    atomic_store_explicit(&za->heat_median_ts,    tss[n / 2],  memory_order_relaxed);

    free(cnts);
    free(tss);
}

void zone_heat_record_write(zone_alloc_t *za, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID)
        return;

    /* Hash into heat table; collisions only cause imprecise classification. */
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

    /* Periodic median recompute; one thread at a time via CAS. */
    uint64_t writes_now = atomic_fetch_add_explicit(
        &za->heat_writes_since_recompute, 1, memory_order_relaxed) + 1;
    if (writes_now >= ZTREE_HEAT_RECOMPUTE_INTERVAL)
    {
        uint8_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&za->heat_recompute_in_progress,
                                                    &expected, 1,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed))
        {
            /* Reset counter before recompute so re-trigger waits for next batch. */
            atomic_store_explicit(&za->heat_writes_since_recompute, 0,
                                  memory_order_relaxed);
            recompute_heat_medians(za);
            atomic_store_explicit(&za->heat_recompute_in_progress, 0,
                                  memory_order_release);
        }
        /* If CAS lost, another thread is already recomputing; do nothing. */
    }
}

void zone_heat_reset(zone_alloc_t *za, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID)
        return;

    size_t idx = (size_t)(node_id % ZTREE_HEAT_TABLE_SIZE);

    /* Counter → 0 (paper §3.1.1 reset).  Timestamp → "now" so relocated
     * node still flags as recently-active (avoids cold-pool misclassification). */
    atomic_store_explicit(&za->heat_table[idx].access_count, 0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_table[idx].last_write_ts,
                          zone_monotonic_ts_16b(),
                          memory_order_relaxed);
}

void zone_heat_inherit(zone_alloc_t *za,
                       ztree_node_id_t dst,
                       ztree_node_id_t src)
{
    if (dst == ZTREE_INVALID_NODE_ID || src == ZTREE_INVALID_NODE_ID)
        return;

    size_t src_idx = (size_t)(src % ZTREE_HEAT_TABLE_SIZE);
    size_t dst_idx = (size_t)(dst % ZTREE_HEAT_TABLE_SIZE);

    /* Hash collision: same bucket, so resetting dst would wipe src. No-op. */
    if (src_idx == dst_idx)
        return;

    uint32_t src_ts = atomic_load_explicit(&za->heat_table[src_idx].last_write_ts,
                                           memory_order_relaxed);

    atomic_store_explicit(&za->heat_table[dst_idx].access_count, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&za->heat_table[dst_idx].last_write_ts, src_ts,
                          memory_order_relaxed);
}

int zone_is_hot(zone_alloc_t *za, ztree_node_id_t node_id)
{
    if (node_id == ZTREE_INVALID_NODE_ID)
        return 0;

    size_t idx = (size_t)(node_id % ZTREE_HEAT_TABLE_SIZE);

    uint16_t cnt     = atomic_load_explicit(&za->heat_table[idx].access_count,
                                            memory_order_relaxed);
    uint32_t ts      = atomic_load_explicit(&za->heat_table[idx].last_write_ts,
                                            memory_order_relaxed);
    uint16_t med_cnt = atomic_load_explicit(&za->heat_median_count,
                                            memory_order_relaxed);
    uint32_t med_ts  = atomic_load_explicit(&za->heat_median_ts,
                                            memory_order_relaxed);

    /* Bootstrap: default to hot (avoids cold-pool funneling). */
    if (med_cnt == 0 && med_ts == 0)
        return 1;

    /* Hot iff Counter OR Timestamp > median (strict >; at-median → cold). */
    return (cnt > med_cnt) || (ts > med_ts) ? 1 : 0;
}
