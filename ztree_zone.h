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
#define ZTREE_HEAT_HOT_THRESHOLD  10U   /* writes before a node is "hot" */
#define ZTREE_HEAT_TABLE_SIZE    65536U /* must be a power of two         */

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
 */
void zone_heat_record_write(zone_alloc_t *za, ztree_node_id_t node_id);

/*
 * zone_is_hot  –  returns 1 if node_id is considered hot, 0 otherwise.
 */
int zone_is_hot(zone_alloc_t *za, ztree_node_id_t node_id);
