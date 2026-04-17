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

        /* All active group zones are sealed.  Attach ONE replacement.
         *
         * The primary growth mechanism is zone_grow_group_by_one() in
         * flush_page_immediate, which increments group_count by 1 every
         * time a zone is sealed.  This fallback expansion fires only
         * when all zones in the current scan range are simultaneously
         * out of space (rare: usually the sliding growth has already
         * added a replacement).  Adding just 1 zone here avoids
         * opening too many new zones at once and exceeding the device's
         * max_active_zones budget. */
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
            /* We won the expansion.  Finish EVERY zone in the outgoing
             * group so the device's max_active_zones budget frees up
             * before the new batch is implicitly opened by the next
             * pwrite.
             *
             * Critical: we call zbd_finish_zones UNCONDITIONALLY for
             * each outgoing zone, regardless of whether zone_full was
             * already set.  A zone can be in a gap state where:
             *   • zone_has_space() returns 0 (WP + PAGE > capacity)
             *   • zone_full is still 0 (95% seal threshold not reached)
             *   • device still counts the zone as OPEN (active slot used)
             * The previous code only called finish when was_full==0, but
             * that skipped zones that had been sealed (zone_full=1) by
             * the 95% threshold in flush_page_immediate IF the earlier
             * zone_finish_if_full call failed silently.  Calling finish
             * unconditionally is safe (finishing an already-FULL zone
             * is a harmless no-op at the device level) and guarantees
             * the active slot is released. */
            if (za->fd >= 0) {
                /* Finish all outgoing zones, then verify each one has
                 * actually transitioned to FULL at the device level.
                 *
                 * Some ZNS devices acknowledge zbd_finish_zones (rc=0)
                 * but update the zone-state table asynchronously.  If we
                 * proceed before the transition commits, pwriting to a
                 * new zone (which implicitly opens it) can exceed the
                 * device's max_active_zones limit.  The verify loop
                 * re-reads each zone's state via zbd_report_zones and
                 * retries the finish until the zone is genuinely FULL. */
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

                /* Verify-and-retry loop: re-read zone states and retry
                 * finish for any zone that hasn't reached FULL yet. */
                for (int attempt = 0; attempt < 100; attempt++) {
                    int all_full = 1;
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
                            continue;
                        if (zinfo.cond != ZBD_ZONE_COND_FULL) {
                            zbd_finish_zones(za->fd,
                                             (off_t)za->zones[zid].start,
                                             (off_t)za->zone_size);
                            all_full = 0;
                        }
                    }
                    if (all_full)
                        break;
                    usleep(100);
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

    /* Percentile-based heat policy state */
    atomic_store_explicit(&za->heat_median_count, 0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_median_ts,    0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_writes_since_recompute, 0, memory_order_relaxed);
    atomic_store_explicit(&za->heat_recompute_in_progress,  0, memory_order_relaxed);

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

/* ───────────────────────────────────────────────────────────────────────────
 * Percentile-based heat policy (paper §3.1.1)
 *
 * recompute_heat_medians() scans the entire heat_table, collects all
 * non-zero (Counter, Timestamp) pairs, sorts each dimension, and stores
 * the 50th-percentile values.  zone_is_hot() then classifies a node as
 * hot iff its Counter OR Timestamp exceeds the corresponding median.
 *
 * Cost: with ZTREE_HEAT_TABLE_SIZE = 65536 entries, the scan + qsort runs
 * in roughly 1–3 ms on modern x86.  Recomputation is gated by
 * ZTREE_HEAT_RECOMPUTE_INTERVAL (16 K writes by default), so the
 * amortised overhead is well under 1% even for tight insert workloads.
 * Only one thread runs the recompute at a time; others see
 * heat_recompute_in_progress = 1 and skip until the next interval.
 *
 * Bootstrap: until at least ZTREE_HEAT_MIN_SAMPLES non-zero entries exist,
 * the medians stay at 0 and zone_is_hot() falls back to default-hot.
 * This avoids the pathological default-cold pinning observed when median
 * is undefined and every leaf would otherwise be classified cold.
 * ─────────────────────────────────────────────────────────────────────── */

static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a;
    uint16_t y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

static void recompute_heat_medians(zone_alloc_t *za)
{
    /* Heap allocation: ZTREE_HEAT_TABLE_SIZE * 2 bytes = 128 KB per array,
     * too large for a stack frame.  malloc/free per recompute is fine — one
     * call every ~16 K writes is negligible. */
    uint16_t *cnts = malloc(ZTREE_HEAT_TABLE_SIZE * sizeof(uint16_t));
    uint16_t *tss  = malloc(ZTREE_HEAT_TABLE_SIZE * sizeof(uint16_t));
    if (!cnts || !tss)
    {
        free(cnts);
        free(tss);
        return; /* skip this round; medians stay at their previous values */
    }

    size_t n = 0;
    for (size_t i = 0; i < ZTREE_HEAT_TABLE_SIZE; i++)
    {
        uint16_t c = atomic_load_explicit(&za->heat_table[i].access_count,
                                          memory_order_relaxed);
        if (c == 0)
            continue; /* empty bucket — exclude from population */
        uint16_t t = atomic_load_explicit(&za->heat_table[i].last_write_ts,
                                          memory_order_relaxed);
        cnts[n] = c;
        tss[n]  = t;
        n++;
    }

    if (n < ZTREE_HEAT_MIN_SAMPLES)
    {
        /* Too few samples for a meaningful median.  Force defaults to 0
         * so zone_is_hot() takes the bootstrap (default-hot) branch. */
        atomic_store_explicit(&za->heat_median_count, 0, memory_order_relaxed);
        atomic_store_explicit(&za->heat_median_ts,    0, memory_order_relaxed);
        free(cnts);
        free(tss);
        return;
    }

    qsort(cnts, n, sizeof(uint16_t), cmp_u16);
    qsort(tss,  n, sizeof(uint16_t), cmp_u16);

    /* 50th percentile = element at index n/2 (lower median for even n). */
    atomic_store_explicit(&za->heat_median_count, cnts[n / 2], memory_order_relaxed);
    atomic_store_explicit(&za->heat_median_ts,    tss[n / 2],  memory_order_relaxed);

    free(cnts);
    free(tss);
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

    /* Periodic median recompute.  Every Nth write triggers a one-shot
     * recompute pass; only the thread that successfully claims the
     * in_progress flag runs it, the rest skip and continue. */
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
            /* Reset the rolling counter inside the critical section so
             * concurrent writers will only re-trigger once we've finished
             * (and the next batch of writes accumulates). */
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

    /* Counter goes to 0 — paper §3.1.1 reset rule.  Timestamp is set to
     * "now" rather than 0 so the freshly-relocated node still flags as
     * recently-active under the OR-combined hot test; otherwise zeroing
     * both fields would force the node into the cold pool for one full
     * recompute interval, recreating the very pathology percentile-based
     * heat is meant to avoid. */
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

    /* Same bucket via hash collision: dst and src physically share the
     * heat slot.  Resetting dst's counter would also wipe src's counter
     * (we'd be zeroing the parent's heat right at the moment we're
     * trying to use it).  Leave the bucket alone — both nodes will share
     * the parent's full heat under hash-collision semantics, which the
     * heat table already accepts as approximate. */
    if (src_idx == dst_idx)
        return;

    /* Inherit semantics:
     *   • Timestamp is COPIED from src.  This is the channel through
     *     which the parent's hot/cold character propagates to the
     *     freshly split sibling: zone_is_hot(dst) will test ts against
     *     the median, getting the same recently-active signal the parent
     *     would have.
     *   • Counter is RESET to 0.  The new node hasn't actually been
     *     written to yet, so its access frequency starts fresh.  This
     *     also avoids inflating the heat-table population with phantom
     *     duplicates of the parent's count, which would bias the median
     *     upward over time. */
    uint16_t src_ts = atomic_load_explicit(&za->heat_table[src_idx].last_write_ts,
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
    uint16_t ts      = atomic_load_explicit(&za->heat_table[idx].last_write_ts,
                                            memory_order_relaxed);
    uint16_t med_cnt = atomic_load_explicit(&za->heat_median_count,
                                            memory_order_relaxed);
    uint16_t med_ts  = atomic_load_explicit(&za->heat_median_ts,
                                            memory_order_relaxed);

    /* Bootstrap: medians not yet computed (or workload too sparse).
     * Default to hot — putting new leaves in the larger hot pool avoids
     * the default-cold pinning that funnels everything into 2 zones. */
    if (med_cnt == 0 && med_ts == 0)
        return 1;

    /* Paper §3.1.1: hot iff Counter OR Timestamp is in the top 50th
     * percentile.  We use strict > so exactly-at-median nodes count as
     * cold (lower-half tiebreak). */
    return (cnt > med_cnt) || (ts > med_ts) ? 1 : 0;
}
