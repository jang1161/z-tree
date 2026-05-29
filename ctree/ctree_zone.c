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

/* Round-robin picker with active-open cap = init_count.
 * Round-robin walk: pick first non-full zone with space.  If the zone is
 * EMPTY (wp == start), opening it would create a new write-active slot,
 * so only allow it when current write-active count < init_count.
 * Returns ZTREE_INVALID_ZONE_ID at cap (all empties skipped, no write-
 * active had space) — caller spills to CNS. */
/* prefer_existing: pack into a write-active zone only (no new-empty open,
 * no grow); returns INVALID if none has space.  Used by ZNS GC to skip
 * burning an admission slot when open zones already have room. */
static uint32_t rr_pick_zone(zone_alloc_t *za,
                              uint32_t pool_base,
                              uint32_t pool_size,
                              uint32_t init_count,
                              _Atomic(uint32_t) *group_count,
                              _Atomic(uint32_t) *rr_counter,
                              uint32_t avoid_zone,
                              const char *layer_name,
                              int prefer_existing)
{
    for (;;) {
        uint32_t count = atomic_load_explicit(group_count, memory_order_acquire);
        uint32_t start = atomic_fetch_add_explicit(rr_counter, 1, memory_order_relaxed);

        /* Count write-active zones to gate opening new empties. */
        uint32_t wa = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t zid = pool_base + i;
            if (atomic_load_explicit(&za->zone_full[zid],
                                     memory_order_acquire))
                continue;
            if (atomic_load_explicit(&za->zone_wp_bytes[zid],
                                     memory_order_acquire)
                > za->zones[zid].start)
                wa++;
        }
        bool allow_empty = prefer_existing ? false : (wa < init_count);

        /* Pick pool = write-active-with-space, plus empties while under the
         * init_count cap.  RR over the pool — first-fit funnels all threads
         * onto one zone after a full run. */
        uint32_t npick = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t zone_id = pool_base + i;
            if (zone_id == avoid_zone)
                continue;
            if (atomic_load_explicit(&za->zone_full[zone_id],
                                     memory_order_acquire))
                continue;
            uint64_t wp = atomic_load_explicit(&za->zone_wp_bytes[zone_id],
                                               memory_order_acquire);
            uint64_t zstart = za->zones[zone_id].start;
            if (wp + ZTREE_PAGE_SIZE > zstart + za->zones[zone_id].capacity)
                continue;  /* no space */
            if (wp > zstart || allow_empty)
                npick++;
        }
        /* Grow to expose more empties — but only while we may actually open
         * one (allow_empty).  Once write-active == init_count, npick naturally
         * falls below init_count as zones fill; growing then can't open any
         * empty and just runs group_count up to pool_size (scatters the RR
         * window).  admission control caps real opens. */
        if (allow_empty && npick > 0 && npick < init_count && count < pool_size) {
            uint32_t new_count = count + (init_count - npick);
            if (new_count > pool_size)
                new_count = pool_size;
            uint32_t expected = count;
            atomic_compare_exchange_strong_explicit(group_count, &expected,
                new_count, memory_order_acq_rel, memory_order_relaxed);
            continue;
        }
        if (npick > 0) {
            uint32_t target = start % npick, k = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t zone_id = pool_base + i;
                if (zone_id == avoid_zone)
                    continue;
                if (atomic_load_explicit(&za->zone_full[zone_id],
                                         memory_order_acquire))
                    continue;
                uint64_t wp = atomic_load_explicit(&za->zone_wp_bytes[zone_id],
                                                   memory_order_acquire);
                uint64_t zstart = za->zones[zone_id].start;
                if (wp + ZTREE_PAGE_SIZE > zstart + za->zones[zone_id].capacity)
                    continue;
                if ((wp > zstart || allow_empty) && k++ == target)
                    return zone_id;
            }
            continue;  /* lost a race; retry */
        }

        /* prefer_existing: no write-active zone had space — give up. */
        if (prefer_existing)
            return ZTREE_INVALID_ZONE_ID;

        /* Nothing pickable.  A non-full zone with space exists only when an
         * empty was throttled by the cap → caller spills to CNS.  Else all
         * zones full → grow. */
        uint32_t first_empty = ZTREE_INVALID_ZONE_ID;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t zone_id = pool_base + i;
            if (zone_id == avoid_zone)
                continue;
            if (atomic_load_explicit(&za->zone_full[zone_id],
                                     memory_order_acquire))
                continue;
            uint64_t wp = atomic_load_explicit(&za->zone_wp_bytes[zone_id],
                                               memory_order_acquire);
            uint64_t zstart = za->zones[zone_id].start;
            if (wp + ZTREE_PAGE_SIZE > zstart + za->zones[zone_id].capacity)
                continue;
            first_empty = zone_id;
            break;
        }
        if (first_empty != ZTREE_INVALID_ZONE_ID)
            return ZTREE_INVALID_ZONE_ID;

        /* All zones full — grow. */
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
                    zone_mark_full(za, zid);
                    zbd_finish_zones(za->fd,
                                     (off_t)za->zones[zid].start,
                                     (off_t)za->zone_size);
                    zone_admission_release_zone(za, zid);
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
 * Active-zone admission control
 * ─────────────────────────────────────────────────────────────────────────── */

int zone_admission_try(zone_alloc_t *za)
{
    if (!za->admission_enabled)
        return 1;
    uint32_t cur = atomic_load_explicit(&za->active_zones, memory_order_acquire);
    for (;;) {
        if (cur >= za->active_cap)
            return 0;
        if (atomic_compare_exchange_weak_explicit(&za->active_zones, &cur, cur + 1,
                memory_order_acq_rel, memory_order_acquire))
            return 1;
    }
}

void zone_admission_release(zone_alloc_t *za)
{
    if (!za->admission_enabled)
        return;
    uint32_t cur = atomic_load_explicit(&za->active_zones, memory_order_acquire);
    while (cur > 0 &&
           !atomic_compare_exchange_weak_explicit(&za->active_zones, &cur, cur - 1,
               memory_order_acq_rel, memory_order_acquire))
        ;  /* guard against underflow if a release ever lacks a matching acquire */
}

/* Reserve a slot for zone_id and mark it held.  Caller holds the zone's write
 * lock and is about to do its first write (acquire precedes device-open).
 * Idempotent per zone: if already held (e.g. an EOVERFLOW retry re-picks the
 * same still-empty zone) it returns 1 without double-counting. */
int zone_admission_acquire(zone_alloc_t *za, uint32_t zone_id)
{
    if (!za->admission_enabled)
        return 1;
    if (atomic_load_explicit(&za->admission_held[zone_id], memory_order_acquire))
        return 1;
    if (!zone_admission_try(za))
        return 0;
    atomic_store_explicit(&za->admission_held[zone_id], 1, memory_order_release);
    return 1;
}

/* Release zone_id's slot iff currently held (exactly-once via CAS).  Call only
 * after the zone is finished/full on the device, so active_zones never drops
 * below the device's true active count. */
void zone_admission_release_zone(zone_alloc_t *za, uint32_t zone_id)
{
    if (!za->admission_enabled)
        return;
    uint8_t held = 1;
    if (atomic_compare_exchange_strong_explicit(&za->admission_held[zone_id],
            &held, 0, memory_order_acq_rel, memory_order_relaxed))
        zone_admission_release(za);
}

void zone_mark_full(zone_alloc_t *za, uint32_t zone_id)
{
    uint8_t expected = 0;
    atomic_compare_exchange_strong_explicit(&za->zone_full[zone_id], &expected, 1,
            memory_order_acq_rel, memory_order_relaxed);
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

    /* Per-zone admission-held bits (owned here; only used if admission_enabled). */
    za->admission_held = calloc(nr_zones, sizeof(_Atomic(uint8_t)));
    if (!za->admission_held)
    {
        perror("zone_alloc_init: admission_held");
        exit(EXIT_FAILURE);
    }
}

void zone_alloc_destroy(zone_alloc_t *za)
{
    pthread_mutex_destroy(&za->lifecycle_lock);
    free(za->admission_held);
    za->admission_held = NULL;
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
            /* Already FULL on the device (another thread finished it, or a
             * natural fill).  Release our held slot, then skip the regrow. */
            zone_admission_release_zone(za, zone_id);
            pthread_mutex_unlock(&za->lifecycle_lock);
            return;
        }
    }

    /* 1. Finish the sealed zone (no verify loop; EOVERFLOW retry
     *    in flush_page_immediate handles async transitions). */
    zbd_finish_zones(za->fd,
                     (off_t)za->zones[zone_id].start,
                     (off_t)za->zone_size);
    /* Finish-then-release: the device freed the slot, so drop ours now. */
    zone_admission_release_zone(za, zone_id);

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
                        "ILayer", 0);
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
                            "LLayer-hot", 0);
    } else {
        return rr_pick_zone(za,
                            za->cold_pool_base, za->cold_pool_size,
                            za->cold_init_count,
                            &za->cold_group_count, &za->cold_rr,
                            avoid_zone,
                            "LLayer-cold", 0);
    }
}

/* GC variant: pack into an open zone only (INVALID if none has space). */
uint32_t zone_alloc_llayer_existing(zone_alloc_t *za, ztree_node_id_t node_id,
                                    uint32_t avoid_zone)
{
    if (zone_is_hot(za, node_id)) {
        return rr_pick_zone(za,
                            za->hot_pool_base, za->hot_pool_size,
                            za->hot_init_count,
                            &za->hot_group_count, &za->hot_rr,
                            avoid_zone,
                            "LLayer-hot", 1);
    } else {
        return rr_pick_zone(za,
                            za->cold_pool_base, za->cold_pool_size,
                            za->cold_init_count,
                            &za->cold_group_count, &za->cold_rr,
                            avoid_zone,
                            "LLayer-cold", 1);
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
