/*
 * ctree_main.h  –  CTree handle definition and public API
 *
 * CTree = ZTree + CNS fallback on zone write-lock contention.
 * Instead of blocking on a contended zone mutex, spill the page to the
 * conventional namespace device (nvme3n1) and record the location in NLT.
 */

#pragma once

#include "ctree_types.h"
#include "ctree_nlt.h"
#include "ctree_zone.h"

#define ZTREE_FLUSH_INTERVAL_MS      10
#define ZTREE_MAX_BATCH_PAGES      2048
#define ZTREE_MAX_NVME_PAGES         64
#define ZTREE_NODE_LATCH_BUCKETS  65536
#define CTREE_CNS_SHARDS             64   /* dynamic: CNS file shards (inode-lock split) */

struct ztree_s {
    /* ZNS device */
    int              fd;
    int              direct_fd;
    __u32            nsid;
    struct zbd_info  info;
    struct zbd_zone *zones;

    _Atomic(uint64_t) *zone_wp_bytes;
    _Atomic(uint8_t)  *zone_full;

    /* Per-zone live-leaf count (LLayer ZNS zones only) — drives ZNS GC
     * victim selection without a full NLT scan.  Maintained incrementally
     * by flush_page_immediate / cow_evict_cns_leaves / delete / cow_gc_zns. */
    _Atomic(uint32_t) *zone_valid_leaves;

    /* ── CNS fallback ──────────────────────────────────────────────────── */
    int              cns_fd;          /* fd for /dev/nvme3n1 (O_RDWR|O_DIRECT) */
    int              cns_fd_shard[CTREE_CNS_SHARDS]; /* dynamic: sharded CNS fds */
    _Atomic(uint8_t) *cns_bitmap;    /* 1 bit per node_id: 1=on CNS, 0=on ZNS */
    uint32_t          cns_bitmap_bytes;
    _Atomic(uint32_t) cns_zones_busy;        /* current # of zone locks held */
    _Atomic(uint64_t) stat_zones_busy_sum;   /* sum of busy count at each flush */
    _Atomic(uint64_t) stat_zones_busy_samples;
    FILE             *trace_fp;              /* write trace for ZNS/CNS graph */
    uint64_t          trace_start_ns;       /* monotonic start time for trace */
    _Atomic(uint32_t) tree_height;          /* current tree height (root split increments) */
    _Atomic(int64_t)  stat_cns_current;      /* current # valid nodes on CNS */
    _Atomic(uint64_t) stat_cns_writes;       /* cumulative pages written to CNS */
    _Atomic(uint64_t) stat_cns_return_home;  /* CNS → home zone success */
    _Atomic(uint64_t) stat_cns_return_new;   /* CNS → new zone (home full) */
    _Atomic(uint64_t) stat_cns_stay;         /* CNS → CNS again (trylock fail) */
    _Atomic(uint64_t) stat_cns_home_contend; /* home zone trylock failed */
    _Atomic(uint64_t) stat_cns_home_full;    /* home zone was full */

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
    _Atomic(uint64_t) stat_deletes;
    _Atomic(uint64_t) stat_delete_merges;
    _Atomic(uint64_t) stat_delete_cascades;
    _Atomic(uint64_t) stat_delete_root_collapses;
    _Atomic(uint64_t) stat_cache_hit;
    _Atomic(uint64_t) stat_cache_miss;
    _Atomic(uint64_t) stat_page_appends;

    _Atomic(uint64_t) stat_nlt_only_updates;
    _Atomic(uint64_t) stat_zone_changes;
    _Atomic(uint64_t) stat_parent_rewrites;

    _Atomic(uint64_t) stat_leaf_splits;
    _Atomic(uint64_t) stat_internal_splits;
    _Atomic(uint64_t) stat_zone_seals;
    _Atomic(uint64_t) stat_leaf_updates;   /* same-key overwrite */
    _Atomic(uint64_t) stat_leaf_appends;   /* new key, no split */

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

/* cow_delete – remove the entry for key. Returns 1 if a key was deleted,
 * 0 if not found. Mirrors ZTree semantics. */
int       cow_delete(cow_tree *t, int64_t key);

ztree_record *ztree_find(ztree_t *t, int64_t key);

/* Dynamic CNS variant only — NO-OP in other variants.
 * cow_evict_cns_leaves:  migrate every CNS-resident leaf back to a ZNS zone.
 * cow_gc_cns:            punch holes in CNS file for orphan slot ranges.
 * cow_gc_zns:            migrate live leaves out of stale sealed ZNS zones
 *                        and ZONE_RESET them.  Gated by CTREE_DYNAMIC_ZNS_GC=1.
 * All must be called from a quiescent point (no concurrent ops). */
size_t    cow_evict_cns_leaves(cow_tree *t);
size_t    cow_gc_cns(cow_tree *t);
size_t    cow_gc_zns(cow_tree *t);

/* Emit a "begin:<name>" / "end:<name>" line into the phase log
 * (<trace_path>.phases) for the plotter to draw a shaded band. */
void      cow_phase_mark(cow_tree *t, const char *name);

/* Diagnostic: walk cns_bitmap, load each marked node, classify is_leaf. */
void      cow_count_cns_residents(cow_tree *t,
                                  size_t *out_internals,
                                  size_t *out_leaves);
