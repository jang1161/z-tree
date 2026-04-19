/*
 * ztree_main.h  –  ZTree handle definition and public API
 */

#pragma once

#include "ztree_types.h"
#include "ztree_nlt.h"
#include "ztree_zone.h"

#define ZTREE_FLUSH_INTERVAL_MS      10
#define ZTREE_MAX_BATCH_PAGES      2048
#define ZTREE_MAX_NVME_PAGES         64
#define ZTREE_NODE_LATCH_BUCKETS  65536

struct ztree_s {
    /* ZNS device */
    int              fd;
    int              direct_fd;
    __u32            nsid;
    struct zbd_info  info;
    struct zbd_zone *zones;

    _Atomic(uint64_t) *zone_wp_bytes;
    _Atomic(uint8_t)  *zone_full;

    /* Superblock / root state */
    ztree_superblock_entry  durable_sb;
    uint32_t                active_meta_zone;
    uint64_t                meta_wp;
    uint64_t                meta_version;
    pthread_mutex_t         sb_lock;
    ztree_atomic_superblock volatile_sb;

    /* Node identity / lookup */
    _Atomic(uint32_t) next_node_id;
    nlt_t            nlt;

    /* Allocation and cache */
    zone_alloc_t      za;
    ztree_cache_set  *global_cache;
    _Atomic(uint64_t) cache_lru_clock;

    /* Concurrency */
    pthread_rwlock_t *node_latches;  /* hashed by stable node_id */
    pthread_mutex_t  *zone_write_locks; /* one per zone; serialises ZNS sequential writes */

    /* Background checkpoint thread */
    pthread_t     flusher_tid;
    _Atomic(bool) stop_flusher;
    _Atomic(bool) dirty_sb;

    _Atomic(uint64_t) txg_next;
    _Atomic(uint64_t) txg_synced;

    /* Stats */
    _Atomic(uint64_t) stat_inserts;
    _Atomic(uint64_t) stat_cache_hit;
    _Atomic(uint64_t) stat_cache_miss;
    _Atomic(uint64_t) stat_page_appends;

    _Atomic(uint64_t) stat_nlt_only_updates;
    _Atomic(uint64_t) stat_zone_changes;
    _Atomic(uint64_t) stat_parent_rewrites;

    _Atomic(uint64_t) stat_apply_ns_sum;
    _Atomic(uint64_t) stat_apply_ns_samples;
    _Atomic(uint64_t) stat_flush_ns_sum;
    _Atomic(uint64_t) stat_flush_ns_samples;

    /* ── Lock contention profile ──────────────────────────────────────
     * prof_zwl_{iz,hot,cold}_*: per-zone write mutex by zone group.
     * prof_nl_rd_*: node latch shared (read-crab descent).
     * prof_nl_wr_*: node latch exclusive (leaf upgrade + ascent).
     * NLT wrlock stats on nlt_t (see ztree_nlt.h). */
    _Atomic(uint64_t) prof_zwl_iz_wait_ns_sum;
    _Atomic(uint64_t) prof_zwl_iz_hold_ns_sum;
    _Atomic(uint64_t) prof_zwl_iz_acquire_count;
    _Atomic(uint64_t) prof_zwl_iz_max_wait_ns;

    _Atomic(uint64_t) prof_zwl_hot_wait_ns_sum;
    _Atomic(uint64_t) prof_zwl_hot_hold_ns_sum;
    _Atomic(uint64_t) prof_zwl_hot_acquire_count;
    _Atomic(uint64_t) prof_zwl_hot_max_wait_ns;

    _Atomic(uint64_t) prof_zwl_cold_wait_ns_sum;
    _Atomic(uint64_t) prof_zwl_cold_hold_ns_sum;
    _Atomic(uint64_t) prof_zwl_cold_acquire_count;
    _Atomic(uint64_t) prof_zwl_cold_max_wait_ns;

    _Atomic(uint64_t) prof_nl_rd_wait_ns_sum;
    _Atomic(uint64_t) prof_nl_rd_acquire_count;
    _Atomic(uint64_t) prof_nl_rd_max_wait_ns;

    _Atomic(uint64_t) prof_nl_wr_wait_ns_sum;
    _Atomic(uint64_t) prof_nl_wr_acquire_count;
    _Atomic(uint64_t) prof_nl_wr_max_wait_ns;
};

typedef struct ztree_s ztree_t;
typedef ztree_t cow_tree;

cow_tree *cow_open (const char *dev_path);
void      cow_insert(cow_tree *t, int64_t key, const char *value);
void      cow_close (cow_tree *t);

ztree_record *ztree_find(ztree_t *t, int64_t key);
