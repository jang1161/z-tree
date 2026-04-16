/*
 * ztree_zone.h  –  Zone allocator interface for ILayer / LLayer separation
 *
 * ZTree partitions ZNS zones into three logical layers:
 *
 *   RLayer  zones 0–1      meta / superblock (managed directly by ztree_main)
 *   ILayer  zones 2–17     internal nodes, round-robin placement
 *   LLayer  zones 18+      leaf nodes, heat-aware 80/20 hot/cold split
 *
 * The zone allocator exposes two allocation functions:
 *   zone_alloc_ilayer()  – pick the next ILayer zone (round-robin)
 *   zone_alloc_llayer()  – pick a hot or cold LLayer zone based on node heat
 *
 * Heat tracking
 * ─────────────
 * Each leaf node has lightweight metadata:
 *   access_count  – total number of writes (across all TXGs)
 *   last_write_ns – nanosecond timestamp of the last write
 *
 * A node is considered "hot" if access_count > ZTREE_HEAT_HOT_THRESHOLD.
 * Hot nodes are placed in hot zones (round-robin over the hot zone set).
 * Cold nodes are placed in cold zones (round-robin over the cold zone set).
 *
 * The 80/20 split means: for N LLayer zones,
 *   hot zone count  = N * 80 / 100   (≥ 1)
 *   cold zone count = N - hot_count  (≥ 1 if N ≥ 2)
 *
 * This separation reduces GC pressure because frequently-updated (hot) data
 * is not mixed with infrequently-updated (cold) data within the same zone.
 */

#pragma once

#include <pthread.h>

#include "ztree_types.h"

/* ── Heat threshold ──────────────────────────────────────────────────────── */
#define ZTREE_HEAT_HOT_THRESHOLD  10U   /* legacy threshold (unused under percentile policy) */
#define ZTREE_HEAT_TABLE_SIZE    65536U /* must be a power of two         */

/* ── Percentile-based heat policy (paper §3.1.1) ───────────────────────────
 * Recompute the median of access_count and last_write_ts every N writes;
 * a node is hot iff its Counter OR Timestamp is in the top 50th percentile
 * (i.e., strictly greater than the median).  Until enough samples have
 * accumulated for the first recompute, the policy defaults to hot to avoid
 * funneling new leaves into the small cold pool. */
#define ZTREE_HEAT_RECOMPUTE_INTERVAL  16384U  /* writes between recomputes */
#define ZTREE_HEAT_MIN_SAMPLES         128U    /* below this → bootstrap (default-hot) */

/* ───────────────────────────────────────────────────────────────────────────
 * Per-node heat metadata (paper-aligned: 2-byte Counter + 2-byte Timestamp)
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    _Atomic(uint16_t) access_count;  /* 2-byte: access frequency counter */
    _Atomic(uint16_t) last_write_ts; /* 2-byte: timestamp of last write  */
} ztree_node_heat_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Zone allocator handle
 *
 * Each group (ILayer / LLayer-hot / LLayer-cold) has:
 *   pool_base  – first zone ID in the full pool
 *   pool_size  – total zones available in the pool
 *   init_count – initial group size (also the expansion step)
 *   group_count (atomic) – current active group size; expands when exhausted
 *   rr (atomic) – round-robin counter within the active group
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    /* ILayer (IZGroup) */
    uint32_t          ilayer_pool_base;    /* = ZTREE_ILAYER_ZONE_START (2)     */
    uint32_t          ilayer_pool_size;    /* = ZTREE_ILAYER_POOL_SIZE  (16)    */
    uint32_t          ilayer_init_count;   /* = ZTREE_ILAYER_INIT_COUNT (4)     */
    _Atomic(uint32_t) ilayer_group_count;  /* current IZGroup size              */
    _Atomic(uint32_t) ilayer_rr;           /* round-robin counter               */

    /* LLayer hot zones */
    uint32_t          hot_pool_base;       /* first hot-pool zone               */
    uint32_t          hot_pool_size;       /* total hot-pool zones (80% of LLayer pool) */
    uint32_t          hot_init_count;      /* = ZTREE_LZGROUP_HOT_INIT (6)      */
    _Atomic(uint32_t) hot_group_count;     /* current hot group size            */
    _Atomic(uint32_t) hot_rr;

    /* LLayer cold zones */
    uint32_t          cold_pool_base;      /* first cold-pool zone              */
    uint32_t          cold_pool_size;      /* total cold-pool zones (20% of LLayer pool) */
    uint32_t          cold_init_count;     /* = ZTREE_LZGROUP_COLD_INIT (2)     */
    _Atomic(uint32_t) cold_group_count;    /* current cold group size           */
    _Atomic(uint32_t) cold_rr;

    /* Heat tracking: fixed-size table indexed by (node_id % TABLE_SIZE).
     * Multiple node_ids may collide, but the approximation is acceptable
     * since the heat decision is a soft heuristic, not a correctness constraint. */
    ztree_node_heat_t heat_table[ZTREE_HEAT_TABLE_SIZE];

    /* Percentile-based heat thresholds (paper §3.1.1).
     * Updated by recompute_heat_medians() every ZTREE_HEAT_RECOMPUTE_INTERVAL
     * writes.  Read by zone_is_hot() to classify each new allocation.
     *
     *   heat_median_count: median of non-zero access_count values across
     *                      the heat table.  cnt > this → counter-hot.
     *   heat_median_ts:    median of non-zero last_write_ts values.
     *                      ts > this → recently-active, timestamp-hot.
     *   heat_writes_since_recompute: rolling counter to schedule the
     *                                next recompute pass.
     *   heat_recompute_in_progress: CAS-guarded flag; only one thread
     *                               runs the (multi-ms) recompute at a
     *                               time, the others skip and continue. */
    _Atomic(uint16_t) heat_median_count;
    _Atomic(uint16_t) heat_median_ts;
    _Atomic(uint64_t) heat_writes_since_recompute;
    _Atomic(uint8_t)  heat_recompute_in_progress;

    /* Shared zone state pointers (owned by ztree_t, not by this struct) */
    _Atomic(uint8_t)  *zone_full;      /* per-zone full flag          */
    _Atomic(uint64_t) *zone_wp_bytes;  /* per-zone write pointer      */
    struct zbd_zone   *zones;          /* libzbd zone descriptors     */
    uint32_t           nr_zones;       /* total device zones          */

    /* Needed so rr_pick_zone can finish old zones before opening new ones.
     * Required because the device's max_active_zones budget counts CLOSED
     * zones as still active — only zbd_finish_zones releases the slot. */
    int               fd;              /* device fd for zbd_finish_zones  */
    uint64_t          zone_size;       /* zone stride in bytes            */
    pthread_mutex_t  *zone_write_locks;/* per-zone; guards write vs finish */
} zone_alloc_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

/*
 * zone_alloc_init  –  initialise the zone allocator.
 *
 *   ilayer_pool_base  first ILayer pool zone (ZTREE_ILAYER_ZONE_START = 2)
 *   ilayer_pool_size  ILayer pool size       (ZTREE_ILAYER_POOL_SIZE  = 16)
 *   ilayer_init_count initial IZGroup size   (ZTREE_ILAYER_INIT_COUNT  = 4)
 *
 *   hot_pool_base     first hot LLayer pool zone
 *   hot_pool_size     number of hot LLayer pool zones (80% of LLayer pool)
 *   hot_init_count    initial hot group size (ZTREE_LZGROUP_HOT_INIT  = 6)
 *
 *   cold_pool_base    first cold LLayer pool zone (hot_pool_base + hot_pool_size)
 *   cold_pool_size    number of cold LLayer pool zones (20% of LLayer pool)
 *   cold_init_count   initial cold group size (ZTREE_LZGROUP_COLD_INIT = 2)
 *
 *   zone_full / zone_wp_bytes / zones / nr_zones  shared device-state arrays
 */
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
                     pthread_mutex_t   *zone_write_locks);

/*
 * zone_alloc_destroy  –  release allocator resources (does NOT free zone arrays).
 */
void zone_alloc_destroy(zone_alloc_t *za);

/*
 * zone_alloc_ilayer  –  Dynamic_Allocation for an internal node (split or relocation).
 * Round-robins within the active IZGroup; if all zones are exhausted, expands
 * the group by attaching the next batch of idle zones from the ILayer pool.
 */
uint32_t zone_alloc_ilayer(zone_alloc_t *za, uint32_t avoid_zone);

/*
 * zone_alloc_llayer  –  Dynamic_Allocation for a leaf node (split or relocation).
 * Routes to the hot or cold sub-group based on node heat, then round-robins
 * within that group's active zone set.  Expands the group when exhausted.
 */
uint32_t zone_alloc_llayer(zone_alloc_t *za, ztree_node_id_t node_id,
                           uint32_t avoid_zone);

/*
 * zone_heat_record_write  –  increment write counter and update timestamp for
 * node_id.  Call this after every successful page append to track heat.
 * Also drives periodic median recomputation (every
 * ZTREE_HEAT_RECOMPUTE_INTERVAL writes, one thread at a time).
 */
void zone_heat_record_write(zone_alloc_t *za, ztree_node_id_t node_id);

/*
 * zone_heat_reset  –  reset access_count to 0 for node_id.  Call this when
 * a node is reallocated to a new zone (paper §3.1.1: "metadata is reset
 * when a node is reallocated to a new zone").  The timestamp is refreshed
 * to "now" so the freshly-relocated node still registers as recently-
 * active under the percentile policy — without this, a relocated node
 * would be misclassified as cold for one round just because its counter
 * was zeroed.
 */
void zone_heat_reset(zone_alloc_t *za, ztree_node_id_t node_id);

/*
 * zone_heat_inherit  –  seed dst's heat from src so a freshly split
 * sibling inherits the parent's hot/cold character without falsely
 * inflating its counter.
 *
 * Semantics:
 *   • Timestamp is COPIED from src → dst.  zone_is_hot(dst) will then
 *     evaluate dst's ts against the median and reach the same
 *     hot/cold verdict the parent would, propagating the parent's
 *     recently-active signal to the sibling.
 *   • Counter is RESET to 0 on dst.  The new node has no actual write
 *     history yet, so it starts fresh; this also prevents heat-table
 *     inflation from each split duplicating the parent's count.
 *
 * Without this, every freshly-split leaf would start at (cnt=0, ts=0),
 * fail both percentile tests, and get stickied into the small cold pool
 * — recreating the default-cold pathology the percentile policy is
 * meant to fix.
 *
 * Hash-collision case (dst and src map to the same heat bucket): the
 * function is a no-op.  Resetting cnt would wipe src's counter at the
 * very moment we're trying to use its character; leaving it alone means
 * dst inherits src's full heat, which is acceptable under the heat
 * table's existing approximate semantics.
 */
void zone_heat_inherit(zone_alloc_t *za, ztree_node_id_t dst, ztree_node_id_t src);

/*
 * zone_is_hot  –  returns 1 if node_id is considered hot, 0 otherwise.
 *
 * Policy (paper §3.1.1, percentile-based):
 *   • If fewer than ZTREE_HEAT_MIN_SAMPLES non-zero entries have been
 *     observed (bootstrap / sparse workloads): default-hot.
 *   • Otherwise: hot iff (Counter > median_Counter) OR (Timestamp >
 *     median_Timestamp), i.e., in the top 50th percentile of either
 *     dimension.
 */
int zone_is_hot(zone_alloc_t *za, ztree_node_id_t node_id);
