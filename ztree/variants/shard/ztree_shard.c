/*
 * ztree_shard.c  –  Tree-level sharding wrapper
 *
 * 13 independent tree structures (root, node latches, NLT, node-ID space)
 * sharing the same ZNS zone pool (device, zone_write_locks, zone_alloc,
 * page cache).  Reduces CPU-side contention (latch + NLT + cache thrashing)
 * while zone-level write contention remains unchanged.
 *
 * Key routing: key % ZTREE_NUM_SHARDS.
 *
 gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
      ztree_nlt.c ztree_zone.c ztree_main.c \
      variants/shard/ztree_shard.c variants/shard/bench_shard.c \
      -I. -Ivariants/shard \
      -o build/ztree_shard -lzbd -lnvme -lpthread
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ztree_shard.h"

/* Clone shard-specific state from the primary.
 * Shares: fd, direct_fd, info, zones, zone_wp_bytes, zone_full,
 *         zone_write_locks, za, global_cache, cache_lru_clock.
 * Creates own: nlt, node_latches, root (volatile_sb), next_node_id,
 *              sb_lock, stats. */
static ztree_t *shard_clone(ztree_t *base, uint32_t shard_id)
{
    ztree_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    /* ── Shared (copy pointers / values from base) ──────────────── */
    t->fd        = base->fd;
    t->direct_fd = base->direct_fd;
    t->nsid      = base->nsid;
    t->info      = base->info;
    t->zones     = base->zones;          /* shared array */
    t->zone_wp_bytes  = base->zone_wp_bytes;   /* shared atomic array */
    t->zone_full      = base->zone_full;       /* shared atomic array */
    t->zone_write_locks = base->zone_write_locks; /* shared mutex array */
    t->za         = base->za;            /* shared pointer (NOT copy!) */
    t->global_cache   = base->global_cache;    /* shared cache */
    atomic_store_explicit(&t->cache_lru_clock,
        atomic_load_explicit(&base->cache_lru_clock, memory_order_relaxed),
        memory_order_relaxed);

    /* ── Own NLT ────────────────────────────────────────────────── */
    nlt_init(&t->nlt, 4096);

    /* ── Own node latches ───────────────────────────────────────── */
    t->node_latches = calloc(ZTREE_NODE_LATCH_BUCKETS, sizeof(*t->node_latches));
    if (!t->node_latches) { free(t); return NULL; }
    for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
        pthread_rwlock_init(&t->node_latches[i], NULL);

    /* ── Own root (empty tree) ──────────────────────────────────── */
    memset(&t->durable_sb, 0, sizeof(t->durable_sb));
    t->durable_sb.root_node_id = ZTREE_INVALID_NODE_ID;
    t->durable_sb.root_zone_id = ZTREE_INVALID_ZONE_ID;
    t->durable_sb.root_slot_id = ZTREE_INVALID_SLOT_ID;
    t->durable_sb.leaf_order     = ZTREE_LEAF_ORDER;
    t->durable_sb.internal_order = ZTREE_INTERNAL_ORDER;

    atomic_store_explicit(&t->volatile_sb.root_node_id, ZTREE_INVALID_NODE_ID, memory_order_relaxed);
    atomic_store_explicit(&t->volatile_sb.root_zone_id, ZTREE_INVALID_ZONE_ID, memory_order_relaxed);
    atomic_store_explicit(&t->volatile_sb.root_slot_id, ZTREE_INVALID_SLOT_ID, memory_order_relaxed);
    atomic_store_explicit(&t->volatile_sb.seq_no, 0, memory_order_relaxed);

    /* ── Own node-ID space (non-overlapping with other shards) ─── */
    uint32_t id_base = 1 + shard_id * 40000000U;  /* 40M IDs per shard */
    t->durable_sb.next_node_id = id_base;
    atomic_store_explicit(&t->next_node_id, id_base, memory_order_relaxed);

    /* ── Own sb_lock (needed by try_publish_root_if_unchanged) ─── */
    pthread_mutex_init(&t->sb_lock, NULL);

    /* ── No flusher thread (meta zone belongs to primary only) ─── */
    t->active_meta_zone = 0;
    t->meta_wp = 0;
    t->meta_version = 0;
    atomic_store_explicit(&t->stop_flusher, true, memory_order_relaxed);
    atomic_store_explicit(&t->dirty_sb, false, memory_order_relaxed);

    /* ── Own stats (zero-init from calloc) ─────────────────────── */
    atomic_store_explicit(&t->txg_next, 0, memory_order_relaxed);
    atomic_store_explicit(&t->txg_synced, 0, memory_order_relaxed);

    return t;
}

static void shard_close_clone(ztree_t *t)
{
    if (!t) return;

    /* Only free shard-specific resources.
     * Do NOT free shared resources (fd, zones, zone_write_locks,
     * global_cache, zone_wp_bytes, zone_full — owned by primary). */
    nlt_destroy(&t->nlt);

    if (t->node_latches) {
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
    }

    pthread_mutex_destroy(&t->sb_lock);
    free(t);
}

shard_tree_t *shard_open(const char *dev_path)
{
    shard_tree_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;

    /* Shard 0: full open (owns all shared resources). */
    st->primary = cow_open(dev_path);
    if (!st->primary) { free(st); return NULL; }
    st->shards[0] = st->primary;

    /* Shards 1..N-1: lightweight clones. */
    for (int i = 1; i < ZTREE_NUM_SHARDS; i++) {
        st->shards[i] = shard_clone(st->primary, (uint32_t)i);
        if (!st->shards[i]) {
            /* Cleanup on failure. */
            for (int j = 1; j < i; j++)
                shard_close_clone(st->shards[j]);
            cow_close(st->primary);
            free(st);
            return NULL;
        }
    }

    return st;
}

void shard_insert(shard_tree_t *st, int64_t key, const char *value)
{
    uint32_t shard = (uint32_t)((uint64_t)key % ZTREE_NUM_SHARDS);
    cow_insert(st->shards[shard], key, value);
}

void shard_close(shard_tree_t *st)
{
    if (!st) return;

    /* Print aggregate stats from primary (shared cache stats there). */
    /* Close clones first (they reference primary's shared state). */
    for (int i = ZTREE_NUM_SHARDS - 1; i >= 1; i--)
        shard_close_clone(st->shards[i]);

    /* Close primary last (frees shared resources). */
    cow_close(st->primary);
    free(st);
}
