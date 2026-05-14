/*
 * ztree_zone.h  –  Zone allocator for ILayer / LLayer separation
 *
 * Three layers: RLayer (zones 0-1, meta), ILayer (2-17, round-robin),
 * LLayer (18+, heat-aware 80/20 hot/cold split).
 * Heat tracking uses per-node (access_count, last_write_ts) to route
 * hot nodes to hot zones, reducing GC amplification.
 */

#pragma once

#include <pthread.h>

#include "ztree_types.h"

/* ── Heat threshold ──────────────────────────────────────────────────────── */
#define ZTREE_HEAT_HOT_THRESHOLD  10U   /* legacy threshold (unused under percentile policy) */
#define ZTREE_HEAT_TABLE_SIZE    65536U /* must be a power of two         */

/* ── Percentile-based heat policy (paper §3.1.1) ───────────────────────────
 * Hot iff Counter OR Timestamp > median (top 50th percentile).
 * Defaults to hot until MIN_SAMPLES are observed. */
#define ZTREE_HEAT_RECOMPUTE_INTERVAL  16384U  /* writes between recomputes */
#define ZTREE_HEAT_MIN_SAMPLES         128U    /* below this → bootstrap (default-hot) */

/* ───────────────────────────────────────────────────────────────────────────
 * Per-node heat metadata (paper-aligned: 2-byte Counter + 2-byte Timestamp)
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    _Atomic(uint16_t) access_count;  /* 2-byte: access frequency counter */
    _Atomic(uint32_t) last_write_ts; /* 4-byte ms ts (no wrap under ~50 days) */
} ztree_node_heat_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Zone allocator handle
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
     * Recomputed every RECOMPUTE_INTERVAL writes; one thread at a time. */
    _Atomic(uint16_t) heat_median_count;
    _Atomic(uint32_t) heat_median_ts;
    _Atomic(uint64_t) heat_writes_since_recompute;
    _Atomic(uint8_t)  heat_recompute_in_progress;

    /* Shared zone state pointers (owned by ztree_t, not by this struct) */
    _Atomic(uint8_t)  *zone_full;      /* per-zone full flag          */
    _Atomic(uint64_t) *zone_wp_bytes;  /* per-zone write pointer      */
    struct zbd_zone   *zones;          /* libzbd zone descriptors     */
    uint32_t           nr_zones;       /* total device zones          */

    /* Device fd/locks for finishing old zones (CLOSED still counts as active). */
    int               fd;              /* device fd for zbd_finish_zones  */
    uint64_t          zone_size;       /* zone stride in bytes            */
    pthread_mutex_t  *zone_write_locks;/* per-zone; guards write vs finish */

    /* Serialises zone seal → finish → grow transitions. */
    pthread_mutex_t    lifecycle_lock;
} zone_alloc_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Public API
 * ─────────────────────────────────────────────────────────────────────────── */

/* zone_alloc_init  –  initialise the zone allocator with pool ranges. */
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

/* zone_alloc_ilayer  –  round-robin IZGroup allocation; expands when exhausted. */
uint32_t zone_alloc_ilayer(zone_alloc_t *za, uint32_t avoid_zone);

/* zone_alloc_llayer  –  heat-aware LZGroup allocation (hot or cold sub-group). */
uint32_t zone_alloc_llayer(zone_alloc_t *za, ztree_node_id_t node_id,
                           uint32_t avoid_zone);

/* zone_seal_and_replace  –  finish a sealed zone and grow its group by 1.
 * Call AFTER zone_full is set and zone_write_lock is released.
 * Takes lifecycle_lock; at most one transition in flight at a time. */
void zone_seal_and_replace(zone_alloc_t *za, uint32_t zone_id);

/* zone_heat_record_write  –  increment counter + update timestamp after page append. */
void zone_heat_record_write(zone_alloc_t *za, ztree_node_id_t node_id);

/* zone_heat_reset  –  reset counter on zone relocation (paper §3.1.1).
 * Timestamp refreshed to "now" so the node isn't misclassified cold. */
void zone_heat_reset(zone_alloc_t *za, ztree_node_id_t node_id);

/* zone_heat_inherit  –  copy src's timestamp to dst, reset dst's counter.
 * Ensures freshly-split siblings inherit parent's hot/cold character.
 * No-op if dst and src hash to the same bucket (avoids wiping src). */
void zone_heat_inherit(zone_alloc_t *za, ztree_node_id_t dst, ztree_node_id_t src);

/* zone_is_hot  –  1 if hot (above median in counter OR timestamp), 0 otherwise.
 * Defaults to hot during bootstrap (< MIN_SAMPLES). */
int zone_is_hot(zone_alloc_t *za, ztree_node_id_t node_id);
