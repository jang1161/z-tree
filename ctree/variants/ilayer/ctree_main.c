/*
 * ctree_main.c  –  CTree variant: ILayer on CNS, LLayer on ZNS, no fallback
 *
 * Honest separation of node classes:
 *   - Internal nodes  → CNS (/dev/nvme3n1) at slot_id = node_id, never to ZNS
 *   - Leaf nodes      → ZNS (sticky append + dynamic alloc), never to CNS
 *   - RLayer (meta)   → ZNS zones 0-1 (ping-pong, unchanged from ztree)
 *
 * Because location is fully determined by pg->is_leaf there is no CNS bitmap;
 * the NLT entry's zone_id (== CTREE_CNS_ZONE_ID for internals) routes reads.
 *
 * Internal-node CoW slot is permanently slot_id == node_id, so a parent's
 * (child_zone_id, child_node_id) link never needs rewriting on internal CoW;
 * the only structural parent rewrites are split-induced.
 *
 gcc -O2 -g -Wall -Wextra -std=c11 -pthread -I ctree \
      ctree/ctree_nlt.c ctree/ctree_zone.c \
      ctree/variants/ilayer/ctree_main.c \
      ctree/bench_main_ctree.c \
      -o build/ctree_ilayer -lzbd -lnvme -lpthread
 *
 * ────────────────────────────────────────────────────────────────────────── */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ctree_main.h"

/* ── Zone budget overrides for this variant ────────────────────────────────
 * ILayer is dormant (internals on CNS).  3 freed active slots → +2 hot / +1 cold.
 * Active budget: 0 IZ + 9 Hot + 3 Cold + 1 Meta = 13 (within device limit). */
#undef  ZTREE_LZGROUP_HOT_INIT
#define ZTREE_LZGROUP_HOT_INIT     10U
#undef  ZTREE_LZGROUP_COLD_INIT
#define ZTREE_LZGROUP_COLD_INIT    3U

/* CNS lives on an F2FS sparse file: slot=node_id offsets are sparse, so
 * physical use = live internal pages only (not bounded by raw device size). */
#define CTREE_CNS_DIR       "/mnt/cns"
#define CTREE_CNS_FILE_FMT  "/mnt/cns/nodes.%d.dat"

/* CNS I/O mode toggle (env var CNS_ODIRECT=1 to enable). */
static int g_cns_odirect = 0;

/* Trace every TRACE_SAMPLE_INTERVAL page_appends to /tmp/ctree_ilayer_trace.csv */
#define TRACE_SAMPLE_INTERVAL 10000U

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_HEIGHT 32
#define RIGHTMOST_IDX UINT32_MAX

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW insert path state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    ztree_node_id_t node_id;
    uint32_t zone_id;
    uint32_t slot_id;
    ztree_page page;
    uint32_t cidx_from_parent; /* RIGHTMOST_IDX when linked via ptr */
} insert_path_frame;

typedef struct
{
    ztree_node_id_t left_id;
    uint32_t left_zone;
    uint32_t left_slot;
    int left_zone_changed;

    int split;
    int64_t promote_key;
    ztree_node_id_t right_id;
    uint32_t right_zone;
    uint32_t right_slot;
} propagate_state;

#define MAX_BATCH_PAGES ZTREE_MAX_BATCH_PAGES
#define MAX_NVME_PAGES ZTREE_MAX_NVME_PAGES

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Sum st_blocks across all CNS shards (sparse holes excluded). */
static inline size_t cns_physical_bytes(ztree_t *t)
{
    if (t->cns_fd < 0) return 0;
    size_t bytes = 0;
    for (int k = 0; k < CTREE_CNS_SHARDS; k++) {
        struct stat st;
        if (t->cns_fd_shard[k] >= 0 && fstat(t->cns_fd_shard[k], &st) == 0)
            bytes += (size_t)st.st_blocks * 512;
    }
    return bytes;
}

/* Cumulative bytes written across LLayer zones (zone 2+). */
static inline size_t zns_physical_bytes(ztree_t *t)
{
    size_t total = 0;
    for (uint32_t z = 2; z < t->info.nr_zones; z++) {
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[z], memory_order_relaxed);
        uint64_t start = t->zones[z].start;
        if (wp > start) total += (size_t)(wp - start);
    }
    return total;
}

static inline void emit_trace_row(ztree_t *t)
{
    if (!t->trace_fp) return;
    uint64_t total = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    double elapsed = (double)(monotonic_ns() - t->trace_start_ns) / 1e9;
    int64_t internals = atomic_load_explicit(&t->stat_cns_current, memory_order_relaxed);
    uint32_t total_nodes = atomic_load_explicit(&t->next_node_id, memory_order_relaxed) - 1;
    int64_t leaves = (int64_t)total_nodes - internals;
    uint64_t cns_w = atomic_load_explicit(&t->stat_cns_writes, memory_order_relaxed);
    uint32_t height = atomic_load_explicit(&t->tree_height, memory_order_relaxed);
    size_t cns_phys = cns_physical_bytes(t);
    size_t zns_phys = zns_physical_bytes(t);
    fprintf(t->trace_fp, "%.3f,%lld,%lld,%llu,%llu,%u,%zu,%zu\n",
            elapsed, (long long)leaves, (long long)internals,
            (unsigned long long)total, (unsigned long long)cns_w,
            height, cns_phys, zns_phys);
}

static inline void maybe_trace_sample(ztree_t *t)
{
    if (!t->trace_fp) return;
    uint64_t total = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    if (total % TRACE_SAMPLE_INTERVAL != 0) return;
    emit_trace_row(t);
}

static inline void force_trace_sample(ztree_t *t)
{
    emit_trace_row(t);
    if (t->trace_fp) fflush(t->trace_fp);
}

/* Override the weak stub in YCSB-cpp's ctree_db_stubs.c. */
void cow_phase_mark(cow_tree *t, const char *name)
{
    static FILE *phase_fp = NULL;
    static int tried_open = 0;
    if (!t) return;
    if (!phase_fp && !tried_open) {
        tried_open = 1;
        const char *path = getenv("CTREE_DYNAMIC_PHASE_PATH");
        char fallback[1024];
        if (!path || !*path) {
            const char *tp = getenv("CTREE_DYNAMIC_TRACE_PATH");
            if (tp) { snprintf(fallback, sizeof fallback, "%s.phases", tp); path = fallback; }
        }
        if (path) {
            phase_fp = fopen(path, "w");
            if (phase_fp) fprintf(phase_fp, "time_sec,phase\n");
        }
    }
    if (phase_fp) {
        double sec = (double)(monotonic_ns() - t->trace_start_ns) / 1e9;
        fprintf(phase_fp, "%.3f,%s\n", sec, name);
        fflush(phase_fp);
    }
}

static inline uint64_t ztree_hash64(ztree_node_id_t id)
{
    uint64_t x = id;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 33;
    return x;
}

/* Per-node latch mapping (hashed lock table). */
static inline pthread_rwlock_t *node_latch_for_id(ztree_t *t, ztree_node_id_t id)
{
    size_t idx = (size_t)(ztree_hash64(id) & (ZTREE_NODE_LATCH_BUCKETS - 1));
    return &t->node_latches[idx];
}

/* Atomic monotonic-max — used by all three lock-profile buckets. */
static inline void prof_update_max(_Atomic(uint64_t) *target, uint64_t sample)
{
    uint64_t cur = atomic_load_explicit(target, memory_order_relaxed);
    while (sample > cur)
    {
        if (atomic_compare_exchange_weak_explicit(target, &cur, sample,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return;
    }
}

/* CNS slot mapping for internal nodes: slot_id == node_id. */
static inline uint32_t cns_slot_for_node(ztree_node_id_t node_id)
{
    return (uint32_t)node_id;
}

/* Sharded CNS: slot_id (== node_id) → shard (slot & N-1), dense offset within. */
static inline off_t cns_slot_offset(uint32_t slot_id)
{
    return (off_t)(slot_id / CTREE_CNS_SHARDS) * (off_t)ZTREE_PAGE_SIZE;
}

static inline int cns_shard_fd(ztree_t *t, uint32_t slot_id)
{
    return t->cns_fd_shard[slot_id & (CTREE_CNS_SHARDS - 1)];
}

/* CNS cache tag: bit 63 set + slot_id keeps CNS pages disjoint from ZNS pgnum. */
static inline ztree_pagenum_t cns_cache_tag(uint32_t slot_id)
{
    return (ztree_pagenum_t)slot_id | (1ULL << 63);
}

/* Per-zone write mutex (ZWL) profile recorders, broken out by zone group.
 * Routing: ilayer_pool_base..hot_pool_base → IZ, hot..cold → Hot, cold+ → Cold.
 * In this variant ilayer_pool_base == hot_pool_base, so IZ never matches.
 * Meta zones (0,1) bypass this path. Aggregated at print time. */

typedef enum
{
    ZWL_GROUP_IZ   = 0,
    ZWL_GROUP_HOT  = 1,
    ZWL_GROUP_COLD = 2,
    ZWL_GROUP_NONE = 3,   /* meta or otherwise unclassified */
} zwl_group_t;

static inline zwl_group_t zone_group_of(ztree_t *t, uint32_t zone_id)
{
    if (zone_id >= t->za.cold_pool_base)
        return ZWL_GROUP_COLD;
    if (zone_id >= t->za.hot_pool_base)
        return ZWL_GROUP_HOT;
    if (zone_id >= t->za.ilayer_pool_base)
        return ZWL_GROUP_IZ;
    return ZWL_GROUP_NONE;
}

static inline void record_zwl_wait(ztree_t *t, uint32_t zone_id, uint64_t wait_ns)
{
    _Atomic(uint64_t) *wait_p, *cnt_p, *max_p;
    switch (zone_group_of(t, zone_id))
    {
    case ZWL_GROUP_IZ:
        wait_p = &t->prof_zwl_iz_wait_ns_sum;
        cnt_p  = &t->prof_zwl_iz_acquire_count;
        max_p  = &t->prof_zwl_iz_max_wait_ns;
        break;
    case ZWL_GROUP_HOT:
        wait_p = &t->prof_zwl_hot_wait_ns_sum;
        cnt_p  = &t->prof_zwl_hot_acquire_count;
        max_p  = &t->prof_zwl_hot_max_wait_ns;
        break;
    case ZWL_GROUP_COLD:
        wait_p = &t->prof_zwl_cold_wait_ns_sum;
        cnt_p  = &t->prof_zwl_cold_acquire_count;
        max_p  = &t->prof_zwl_cold_max_wait_ns;
        break;
    default:
        return;
    }
    atomic_fetch_add_explicit(wait_p, wait_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(cnt_p,  1,       memory_order_relaxed);
    prof_update_max(max_p, wait_ns);
}

static inline void record_zwl_hold(ztree_t *t, uint32_t zone_id, uint64_t hold_ns)
{
    _Atomic(uint64_t) *hold_p;
    switch (zone_group_of(t, zone_id))
    {
    case ZWL_GROUP_IZ:   hold_p = &t->prof_zwl_iz_hold_ns_sum;   break;
    case ZWL_GROUP_HOT:  hold_p = &t->prof_zwl_hot_hold_ns_sum;  break;
    case ZWL_GROUP_COLD: hold_p = &t->prof_zwl_cold_hold_ns_sum; break;
    default: return;
    }
    atomic_fetch_add_explicit(hold_p, hold_ns, memory_order_relaxed);
}

/* Instrumented node locking: records wait time for contention profiling. */
static inline void node_wrlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID)
        return;
    uint64_t t0 = monotonic_ns();
    pthread_rwlock_wrlock(node_latch_for_id(t, id));
    uint64_t t1 = monotonic_ns();
    uint64_t wait = t1 - t0;
    atomic_fetch_add_explicit(&t->prof_nl_wr_wait_ns_sum, wait, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->prof_nl_wr_acquire_count, 1, memory_order_relaxed);
    prof_update_max(&t->prof_nl_wr_max_wait_ns, wait);
}

/* Non-blocking variant — used by ZNS GC to avoid foreground deadlock. */
static inline int node_trywrlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID) return 1;
    if (pthread_rwlock_trywrlock(node_latch_for_id(t, id)) != 0)
        return 0;
    atomic_fetch_add_explicit(&t->prof_nl_wr_acquire_count, 1, memory_order_relaxed);
    return 1;
}

static inline void node_rdlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID)
        return;
    uint64_t t0 = monotonic_ns();
    pthread_rwlock_rdlock(node_latch_for_id(t, id));
    uint64_t t1 = monotonic_ns();
    uint64_t wait = t1 - t0;
    atomic_fetch_add_explicit(&t->prof_nl_rd_wait_ns_sum, wait, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->prof_nl_rd_acquire_count, 1, memory_order_relaxed);
    prof_update_max(&t->prof_nl_rd_max_wait_ns, wait);
}

static inline void node_unlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID)
        return;
    pthread_rwlock_unlock(node_latch_for_id(t, id));
}

/* Helper: compute physical page number from zone + slot */
static inline ztree_pagenum_t zone_slot_to_pn(ztree_t *t,
                                              uint32_t zone_id,
                                              uint32_t slot_id)
{
    uint64_t off = t->zones[zone_id].start + (uint64_t)slot_id * ZTREE_PAGE_SIZE;
    return (ztree_pagenum_t)(off / ZTREE_PAGE_SIZE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4-Way Set-Associative Global Page Cache
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cache_init(ztree_t *t)
{
    t->global_cache = calloc(ZTREE_CACHE_NUM_SETS, sizeof(ztree_cache_set));
    if (!t->global_cache)
    {
        perror("cache_init calloc");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < ZTREE_CACHE_NUM_SETS; i++)
    {
        if (pthread_mutex_init(&t->global_cache[i].lock, NULL) != 0)
        {
            perror("cache_init mutex");
            exit(EXIT_FAILURE);
        }
        for (int j = 0; j < ZTREE_CACHE_WAYS; j++)
        {
            t->global_cache[i].ways[j].valid = 0;
            t->global_cache[i].ways[j].tag = ZTREE_INVALID_PGN;
        }
    }
    atomic_store_explicit(&t->cache_lru_clock, 0, memory_order_relaxed);
}

static void cache_destroy(ztree_t *t)
{
    if (!t->global_cache)
        return;
    for (size_t i = 0; i < ZTREE_CACHE_NUM_SETS; i++)
        pthread_mutex_destroy(&t->global_cache[i].lock);
    free(t->global_cache);
    t->global_cache = NULL;
}

static int cache_lookup(ztree_t *t, ztree_pagenum_t pn, ztree_page *dst)
{
    size_t set_idx = (size_t)(ztree_hash64((uint64_t)pn) % ZTREE_CACHE_NUM_SETS);
    ztree_cache_set *set = &t->global_cache[set_idx];

    pthread_mutex_lock(&set->lock);
    for (int i = 0; i < ZTREE_CACHE_WAYS; i++)
    {
        if (set->ways[i].valid && set->ways[i].tag == pn)
        {
            set->ways[i].lru_counter = atomic_fetch_add_explicit(
                &t->cache_lru_clock, 1, memory_order_relaxed);
            *dst = set->ways[i].data;
            pthread_mutex_unlock(&set->lock);
            atomic_fetch_add_explicit(&t->stat_cache_hit, 1, memory_order_relaxed);
            return 1;
        }
    }
    pthread_mutex_unlock(&set->lock);
    return 0;
}

static void cache_insert(ztree_t *t, ztree_pagenum_t pn, const ztree_page *src)
{
    size_t set_idx = (size_t)(ztree_hash64((uint64_t)pn) % ZTREE_CACHE_NUM_SETS);
    ztree_cache_set *set = &t->global_cache[set_idx];
    uint64_t clock = atomic_fetch_add_explicit(&t->cache_lru_clock, 1,
                                               memory_order_relaxed);
    pthread_mutex_lock(&set->lock);

    /* Dedupe by tag: same pn must occupy at most one way (otherwise
     * cache_lookup may return a stale duplicate). */
    int victim = -1;
    for (int i = 0; i < ZTREE_CACHE_WAYS; i++)
    {
        if (set->ways[i].valid && set->ways[i].tag == pn)
        {
            victim = i;
            break;
        }
    }

    if (victim < 0)
    {
        victim = 0;
        uint64_t min_lru = set->ways[0].lru_counter;
        for (int i = 0; i < ZTREE_CACHE_WAYS; i++)
        {
            if (!set->ways[i].valid)
            {
                victim = i;
                break;
            }
            if (set->ways[i].lru_counter < min_lru)
            {
                min_lru = set->ways[i].lru_counter;
                victim = i;
            }
        }
    }

    set->ways[victim].valid = 1;
    set->ways[victim].tag = pn;
    set->ways[victim].lru_counter = clock;
    set->ways[victim].data = *src;

    pthread_mutex_unlock(&set->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page I/O (NLT-aware load)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void load_page_by_pn(ztree_t *t, ztree_pagenum_t pn, ztree_page *dst)
{
    if (cache_lookup(t, pn, dst))
        return;

    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    off_t off = (off_t)pn * ZTREE_PAGE_SIZE;
    int rfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;

    if (t->direct_fd >= 0)
    {
        void *raw;
        if (posix_memalign(&raw, ZTREE_PAGE_SIZE, ZTREE_PAGE_SIZE) != 0)
        {
            perror("load_page_by_pn: posix_memalign");
            exit(EXIT_FAILURE);
        }
        ssize_t n = pread(rfd, raw, ZTREE_PAGE_SIZE, off);
        if (n != (ssize_t)ZTREE_PAGE_SIZE)
        {
            fprintf(stderr, "load_page_by_pn: pread ret=%ld err=%d off=%lu\n",
                    (long)n, errno, (unsigned long)off);
            free(raw);
            exit(EXIT_FAILURE);
        }
        memcpy(dst, raw, ZTREE_PAGE_SIZE);
        free(raw);
    }
    else
    {
        if (pread(t->fd, dst, ZTREE_PAGE_SIZE, off) != (ssize_t)ZTREE_PAGE_SIZE)
        {
            perror("load_page_by_pn: pread");
            exit(EXIT_FAILURE);
        }
    }

    cache_insert(t, pn, dst);

    /* NLT is owned by flush_page_immediate; read-side republish would
     * revert the live entry to a stale slot (see e8dc9d8). */
}

/* Read a page from CNS.  O_DIRECT path uses an aligned bounce buffer. */
static void load_page_from_cns(ztree_t *t, uint32_t slot_id, ztree_page *dst)
{
    ztree_pagenum_t pn = cns_cache_tag(slot_id);

    if (cache_lookup(t, pn, dst))
        return;

    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    ssize_t n;
    if (g_cns_odirect)
    {
        _Alignas(ZTREE_PAGE_SIZE) char raw[ZTREE_PAGE_SIZE];
        n = pread(cns_shard_fd(t, slot_id), raw, ZTREE_PAGE_SIZE, cns_slot_offset(slot_id));
        if (n == (ssize_t)ZTREE_PAGE_SIZE)
            memcpy(dst, raw, ZTREE_PAGE_SIZE);
    }
    else
    {
        n = pread(cns_shard_fd(t, slot_id), dst, ZTREE_PAGE_SIZE, cns_slot_offset(slot_id));
    }
    if (n != (ssize_t)ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "load_page_from_cns: pread ret=%ld err=%d slot=%u\n",
                (long)n, errno, slot_id);
        exit(EXIT_FAILURE);
    }

    cache_insert(t, pn, dst);
}

static int load_page_by_nlt(ztree_t *t, const nlt_location_t *loc,
                            ztree_page *dst)
{
    if (!loc || loc->zone_id == ZTREE_INVALID_ZONE_ID ||
        loc->node_id == ZTREE_INVALID_NODE_ID)
    {
        return 0;
    }

    if (loc->zone_id == CTREE_CNS_ZONE_ID)
    {
        load_page_from_cns(t, loc->slot_id, dst);
        return 1;
    }

    ztree_pagenum_t pn = zone_slot_to_pn(t, loc->zone_id, loc->slot_id);
    load_page_by_pn(t, pn, dst);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Zone write helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void zone_finish_if_full(ztree_t *t, uint32_t zone_id)
{
    if (zone_id >= t->info.nr_zones)
        return;
    if (!atomic_load_explicit(&t->zone_full[zone_id], memory_order_acquire))
        return;
    off_t zstart = (off_t)t->zones[zone_id].start;
    int rc = zbd_finish_zones(t->fd, zstart, (off_t)t->info.zone_size);
    if (rc != 0)
        fprintf(stderr,
                "[zone_finish_if_full] zone=%u start=0x%llx size=0x%llx "
                "rc=%d errno=%d (%s)\n",
                zone_id,
                (unsigned long long)t->zones[zone_id].start,
                (unsigned long long)t->info.zone_size,
                rc, errno, strerror(errno));
}

static int zone_append_page(ztree_t *t, uint32_t zone_id,
                            const void *buf,
                            uint32_t *out_slot_id,
                            ztree_pagenum_t *out_pn)
{
    uint64_t cur_wp = atomic_fetch_add_explicit(
        &t->zone_wp_bytes[zone_id], ZTREE_PAGE_SIZE, memory_order_acq_rel);

    int wfd;
    const void *wbuf;
    _Alignas(ZTREE_PAGE_SIZE) char local_bounce[ZTREE_PAGE_SIZE];
    if (t->direct_fd >= 0)
    {
        memcpy(local_bounce, buf, ZTREE_PAGE_SIZE);
        wfd = t->direct_fd;
        wbuf = local_bounce;
    }
    else
    {
        wfd = t->fd;
        wbuf = buf;
    }
    if (pwrite(wfd, wbuf, ZTREE_PAGE_SIZE, (off_t)cur_wp) != (ssize_t)ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "pwrite failed: cur_wp=%llu\n", (unsigned long long)cur_wp);
        return -1;
    }

    uint32_t slot = (uint32_t)((cur_wp - t->zones[zone_id].start) / ZTREE_PAGE_SIZE);
    if (out_slot_id)
        *out_slot_id = slot;
    if (out_pn)
        *out_pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);

    uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
    uint64_t zone_end = t->zones[zone_id].start + t->zones[zone_id].capacity;
    if (new_wp >= zone_end)
    {
        atomic_store_explicit(&t->zone_full[zone_id], 1, memory_order_release);
        zone_finish_if_full(t, zone_id);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Meta zone management (RLayer – superblock ping-pong on ZNS zones 0/1)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t other_meta_zone(uint32_t z)
{
    return (z == ZTREE_META_ZONE_0) ? ZTREE_META_ZONE_1 : ZTREE_META_ZONE_0;
}

static void activate_meta_zone(ztree_t *t, uint32_t zone_id, uint64_t version)
{
    off_t zstart = (off_t)t->zones[zone_id].start;
    fdatasync(t->fd);
    zbd_finish_zones(t->fd, zstart, (off_t)t->info.zone_size);
    if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
    {
        perror("activate_meta_zone: zbd_reset_zones");
        exit(EXIT_FAILURE);
    }

    atomic_store_explicit(&t->zone_wp_bytes[zone_id],
                          t->zones[zone_id].start, memory_order_release);
    atomic_store_explicit(&t->zone_full[zone_id], 0, memory_order_release);

    ztree_zone_header zh;
    memset(&zh, 0, sizeof zh);
    zh.magic = ZTREE_ZH_MAGIC;
    zh.state = ZTREE_ZH_ACTIVE;
    zh.version = version;

    uint32_t ignored_slot;
    if (zone_append_page(t, zone_id, &zh, &ignored_slot, NULL) != 0)
    {
        perror("activate_meta_zone: pwrite header");
        exit(EXIT_FAILURE);
    }

    t->active_meta_zone = zone_id;
    t->meta_wp = 1;
    t->meta_version = version;
}

static void rotate_meta_zone(ztree_t *t)
{
    activate_meta_zone(t, other_meta_zone(t->active_meta_zone),
                       t->meta_version + 1);
}

static uint64_t scan_meta_zone(int fd, uint32_t zone_id __attribute__((unused)),
                               uint64_t zone_start, uint64_t zone_size,
                               ztree_superblock_entry *out)
{
    uint64_t n_slots = zone_size / ZTREE_PAGE_SIZE;
    uint64_t last_wp = 0;

    for (uint64_t i = 1; i < n_slots; i++)
    {
        ztree_superblock_entry tmp;
        off_t off = (off_t)(zone_start + i * ZTREE_PAGE_SIZE);
        if (pread(fd, &tmp, ZTREE_PAGE_SIZE, off) != (ssize_t)ZTREE_PAGE_SIZE)
            break;
        if (tmp.magic != ZTREE_SB_MAGIC)
            break;
        *out = tmp;
        last_wp = i;
    }
    return last_wp;
}

static void load_superblock(ztree_t *t)
{
    ztree_zone_header zh0, zh1;
    uint64_t wp0 = atomic_load_explicit(&t->zone_wp_bytes[ZTREE_META_ZONE_0],
                                        memory_order_acquire);
    uint64_t wp1 = atomic_load_explicit(&t->zone_wp_bytes[ZTREE_META_ZONE_1],
                                        memory_order_acquire);

    int v0 = (wp0 > t->zones[ZTREE_META_ZONE_0].start) &&
             (pread(t->fd, &zh0, ZTREE_PAGE_SIZE, 0) == (ssize_t)ZTREE_PAGE_SIZE) &&
             (zh0.magic == ZTREE_ZH_MAGIC);
    int v1 = (wp1 > t->zones[ZTREE_META_ZONE_1].start) &&
             (pread(t->fd, &zh1, ZTREE_PAGE_SIZE,
                    (off_t)t->zones[ZTREE_META_ZONE_1].start) == (ssize_t)ZTREE_PAGE_SIZE) &&
             (zh1.magic == ZTREE_ZH_MAGIC);

    ztree_superblock_entry sb0, sb1;
    uint64_t sbwp0 = 0, sbwp1 = 0;

    if (v0)
        sbwp0 = scan_meta_zone(t->fd, ZTREE_META_ZONE_0,
                               t->zones[ZTREE_META_ZONE_0].start,
                               t->info.zone_size, &sb0);
    if (v1)
        sbwp1 = scan_meta_zone(t->fd, ZTREE_META_ZONE_1,
                               t->zones[ZTREE_META_ZONE_1].start,
                               t->info.zone_size, &sb1);

    if (sbwp0 == 0 && sbwp1 == 0)
    {
        memset(&t->durable_sb, 0, sizeof t->durable_sb);
        t->durable_sb.root_node_id = ZTREE_INVALID_NODE_ID;
        t->durable_sb.root_zone_id = ZTREE_INVALID_ZONE_ID;
        t->durable_sb.root_slot_id = ZTREE_INVALID_SLOT_ID;
        t->durable_sb.next_node_id = 1;
        t->durable_sb.leaf_order = ZTREE_LEAF_ORDER;
        t->durable_sb.internal_order = ZTREE_INTERNAL_ORDER;
        activate_meta_zone(t, ZTREE_META_ZONE_0, 0);
    }
    else
    {
        ztree_superblock_entry *best;
        uint32_t best_zone;
        uint64_t best_wp;
        uint64_t best_meta_version;

        if (sbwp0 > 0 && sbwp1 > 0)
        {
            if (sb0.seq_no >= sb1.seq_no)
            {
                best = &sb0;
                best_zone = ZTREE_META_ZONE_0;
                best_wp = sbwp0;
                best_meta_version = zh0.version;
            }
            else
            {
                best = &sb1;
                best_zone = ZTREE_META_ZONE_1;
                best_wp = sbwp1;
                best_meta_version = zh1.version;
            }
        }
        else if (sbwp0 > 0)
        {
            best = &sb0;
            best_zone = ZTREE_META_ZONE_0;
            best_wp = sbwp0;
            best_meta_version = zh0.version;
        }
        else
        {
            best = &sb1;
            best_zone = ZTREE_META_ZONE_1;
            best_wp = sbwp1;
            best_meta_version = zh1.version;
        }

        t->durable_sb = *best;
        t->active_meta_zone = best_zone;
        t->meta_wp = best_wp + 1;
        t->meta_version = best_meta_version;
    }

    atomic_store_explicit(&t->volatile_sb.root_node_id,
                          t->durable_sb.root_node_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id,
                          t->durable_sb.root_zone_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id,
                          t->durable_sb.root_slot_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no,
                          t->durable_sb.seq_no * 2, memory_order_release);

    atomic_store_explicit(&t->next_node_id,
                          t->durable_sb.next_node_id, memory_order_relaxed);

    if (t->durable_sb.root_node_id != ZTREE_INVALID_NODE_ID)
    {
        nlt_location_t loc = {
            .zone_id = t->durable_sb.root_zone_id,
            .node_id = t->durable_sb.root_node_id,
            .slot_id = t->durable_sb.root_slot_id,
        };
        nlt_update(&t->nlt, &loc);
    }
}

static void write_superblock_sync(ztree_t *t)
{
    pthread_mutex_lock(&t->sb_lock);

    t->durable_sb.magic = ZTREE_SB_MAGIC;

    if (t->meta_wp >= t->zones[t->active_meta_zone].capacity / ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "[ctree_ilayer] meta zone full, rotating\n");
        rotate_meta_zone(t);
    }

    if (zone_append_page(t, t->active_meta_zone, &t->durable_sb, NULL, NULL) != 0)
    {
        perror("write_superblock_sync: pwrite");
        exit(EXIT_FAILURE);
    }
    t->meta_wp++;

    pthread_mutex_unlock(&t->sb_lock);
}

static void *sb_flusher_thread(void *arg)
{
    ztree_t *t = (ztree_t *)arg;

    while (!atomic_load_explicit(&t->stop_flusher, memory_order_acquire))
    {
        usleep(ZTREE_FLUSH_INTERVAL_MS * 1000);

        if (!atomic_exchange_explicit(&t->dirty_sb, false, memory_order_acq_rel))
            continue;

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot;
        uint64_t seq;
        for (;;)
        {
            uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 & 1ULL)
                continue;
            root_nid = atomic_load_explicit(&t->volatile_sb.root_node_id,
                                            memory_order_acquire);
            root_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id,
                                             memory_order_acquire);
            root_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id,
                                             memory_order_acquire);
            uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 == s2 && (s2 & 1ULL) == 0)
            {
                seq = s2;
                break;
            }
        }

        pthread_mutex_lock(&t->sb_lock);
        t->durable_sb.root_node_id = root_nid;
        t->durable_sb.root_zone_id = root_zone;
        t->durable_sb.root_slot_id = root_slot;
        t->durable_sb.seq_no = seq / 2;
        t->durable_sb.next_node_id =
            atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
        pthread_mutex_unlock(&t->sb_lock);

        write_superblock_sync(t);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW insert helpers (no overlay/temp_id)
 * ═══════════════════════════════════════════════════════════════════════════ */

static ztree_node_id_t assign_stable_node_id(ztree_t *t)
{
    return atomic_fetch_add_explicit(&t->next_node_id, 1, memory_order_acq_rel);
}

static int load_latest_node(ztree_t *t,
                            uint32_t zone_id,
                            ztree_node_id_t node_id,
                            uint32_t *out_zone,
                            uint32_t *out_slot,
                            ztree_page *out)
{
    nlt_location_t query = {
        .zone_id = zone_id,
        .node_id = node_id,
        .slot_id = ZTREE_INVALID_SLOT_ID,
    };
    nlt_location_t result;
    if (!nlt_lookup(&t->nlt, &query, &result))
        return 0;
    if (!load_page_by_nlt(t, &result, out))
        return 0;
    if (out_zone)
        *out_zone = result.zone_id;
    if (out_slot)
        *out_slot = result.slot_id;
    return 1;
}

/*
 * flush_page_immediate
 *   Internal node → CNS at slot_id == node_id (always). out_zone_changed=0:
 *     parent's (child_zone_id, child_node_id) link is permanent, so structural
 *     parent rewrite happens only via split propagation, never via CoW.
 *   Leaf node → ZNS, sticky-on-prev_zone or zone_alloc_llayer + blocking lock.
 */
/* ZNS GC forward decls + small helpers (full implementation at end of file). */
#define ILAYER_LLAYER_BASE      2U
#define ZNS_GC_STALE_THRESHOLD  0.5

static pthread_t   g_zns_gc_tid;
static _Atomic bool g_zns_gc_running = false;
static _Atomic bool g_zns_gc_stop    = false;
static unsigned     g_zns_gc_interval_ms = 0;

static inline void zone_valid_leaves_move(ztree_t *t,
                                          uint32_t prev_zone,
                                          uint32_t target_zone) {
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && prev_zone != CTREE_CNS_ZONE_ID
        && prev_zone >= ILAYER_LLAYER_BASE)
        atomic_fetch_sub_explicit(&t->zone_valid_leaves[prev_zone], 1,
                                  memory_order_relaxed);
    if (target_zone != CTREE_CNS_ZONE_ID
        && target_zone >= ILAYER_LLAYER_BASE)
        atomic_fetch_add_explicit(&t->zone_valid_leaves[target_zone], 1,
                                  memory_order_relaxed);
}

static void *ilayer_zns_gc_thread(void *arg);
size_t cow_gc_zns(cow_tree *t);

static void flush_page_immediate(ztree_t *t,
                                 ztree_page *pg,
                                 uint32_t prev_zone,
                                 uint32_t avoid_zone,
                                 int *out_zone_changed,
                                 uint32_t *out_zone,
                                 uint32_t *out_slot)
{
    if (!pg->is_leaf)
    {
        /* ── Internal node → CNS write (slot = node_id) ─────────────────── */
        uint32_t slot_id = cns_slot_for_node(pg->node_id);
        ztree_pagenum_t pn = cns_cache_tag(slot_id);

        pg->zone_id = CTREE_CNS_ZONE_ID;
        pg->slot_id = slot_id;

        /* O_DIRECT requires page-aligned buffer; buffered mode can use pg directly. */
        ssize_t pwr;
        if (g_cns_odirect)
        {
            _Alignas(ZTREE_PAGE_SIZE) char bounce[ZTREE_PAGE_SIZE];
            memcpy(bounce, pg, ZTREE_PAGE_SIZE);
            pwr = pwrite(cns_shard_fd(t, slot_id), bounce, ZTREE_PAGE_SIZE, cns_slot_offset(slot_id));
        }
        else
        {
            pwr = pwrite(cns_shard_fd(t, slot_id), pg, ZTREE_PAGE_SIZE, cns_slot_offset(slot_id));
        }
        if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
        {
            fprintf(stderr,
                    "flush_page_immediate: pwrite CNS slot=%u node_id=%u errno=%d (%s)\n",
                    slot_id, pg->node_id, errno, strerror(errno));
            exit(EXIT_FAILURE);
        }

        cache_insert(t, pn, pg);

        nlt_location_t loc = {
            .zone_id = CTREE_CNS_ZONE_ID,
            .node_id = pg->node_id,
            .slot_id = slot_id,
        };
        nlt_update_migrate(&t->nlt, &loc, prev_zone);

        atomic_fetch_add_explicit(&t->stat_cns_writes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->stat_page_appends, 1, memory_order_relaxed);
        maybe_trace_sample(t);

        if (out_zone_changed)
            *out_zone_changed = 0;
        if (out_zone)
            *out_zone = CTREE_CNS_ZONE_ID;
        if (out_slot)
            *out_slot = slot_id;
        return;
    }

    /* ── Leaf node → ZNS (ztree leaf path: sticky + dynamic alloc) ───────── */
    uint32_t target_zone;
    uint64_t cur_wp;
    bool     sticky_ok = false;
    uint64_t zwl_hold_start = 0;
    int      eoverflow_retries = 0;

retry_flush:
    sticky_ok = false;

    /* Phase 1a: try stickiness on prev_zone */
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && !atomic_load_explicit(&t->zone_full[prev_zone], memory_order_acquire))
    {
        uint64_t lock_t0 = monotonic_ns();
        pthread_mutex_lock(&t->zone_write_locks[prev_zone]);
        uint64_t lock_t1 = monotonic_ns();
        record_zwl_wait(t, prev_zone, lock_t1 - lock_t0);
        zwl_hold_start = lock_t1;

        uint64_t zone_start = t->zones[prev_zone].start;
        uint64_t zone_end   = zone_start + t->zones[prev_zone].capacity;
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[prev_zone],
                                           memory_order_relaxed);

        bool sealed = atomic_load_explicit(&t->zone_full[prev_zone],
                                           memory_order_relaxed)
                      || (wp + ZTREE_PAGE_SIZE > zone_end);

        if (!sealed)
        {
            cur_wp      = wp;
            target_zone = prev_zone;
            atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                   wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
            sticky_ok = true;
        }
        else
        {
            record_zwl_hold(t, prev_zone, monotonic_ns() - zwl_hold_start);
            pthread_mutex_unlock(&t->zone_write_locks[prev_zone]);
        }
    }

    /* Phase 1b: dynamic alloc */
    if (!sticky_ok)
    {
        for (;;)
        {
            target_zone = zone_alloc_llayer(&t->za, pg->node_id, avoid_zone);
            if (target_zone == ZTREE_INVALID_ZONE_ID)  /* allocator throttled */
            {
                usleep(20);
                continue;
            }

            uint64_t lock_t0 = monotonic_ns();
            pthread_mutex_lock(&t->zone_write_locks[target_zone]);
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, target_zone, lock_t1 - lock_t0);
            zwl_hold_start = lock_t1;

            if (atomic_load_explicit(&t->zone_full[target_zone],
                                     memory_order_acquire))
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                continue;
            }

            uint64_t zone_start = t->zones[target_zone].start;
            uint64_t zone_end   = zone_start + t->zones[target_zone].capacity;
            uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                               memory_order_relaxed);

            if (wp + ZTREE_PAGE_SIZE > zone_end)
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
                nlt_set_zone_sealed(&t->nlt, target_zone, true);
                zone_seal_and_replace(&t->za, target_zone);
                continue;
            }

            /* First write opens a device-active zone; wait if at cap. */
            if (wp == zone_start
                && !zone_admission_acquire(&t->za, target_zone))
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                usleep(20);
                continue;
            }

            cur_wp = wp;
            atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                   wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
            break;
        }
    }

    uint32_t slot_id = (uint32_t)((cur_wp - t->zones[target_zone].start) / ZTREE_PAGE_SIZE);
    ztree_pagenum_t pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);

    pg->zone_id = target_zone;
    pg->slot_id = slot_id;

    int wfd;
    const void *wbuf;
    _Alignas(ZTREE_PAGE_SIZE) char local_bounce[ZTREE_PAGE_SIZE];
    if (t->direct_fd >= 0)
    {
        memcpy(local_bounce, pg, ZTREE_PAGE_SIZE);
        wfd = t->direct_fd;
        wbuf = local_bounce;
    }
    else
    {
        wfd = t->fd;
        wbuf = pg;
    }
    {
        uint64_t zs = t->zones[target_zone].start;
        uint64_t ze = zs + t->zones[target_zone].capacity;
        if (cur_wp < zs || cur_wp + ZTREE_PAGE_SIZE > ze)
        {
            fprintf(stderr,
                    "flush_page_immediate(leaf): cur_wp out of range "
                    "target_zone=%u prev_zone=%u avoid=%u "
                    "node_id=%llu cur_wp=0x%llx "
                    "zone_start=0x%llx zone_end=0x%llx sticky=%d\n",
                    target_zone, prev_zone, avoid_zone,
                    (unsigned long long)pg->node_id,
                    (unsigned long long)cur_wp,
                    (unsigned long long)zs,
                    (unsigned long long)ze,
                    sticky_ok ? 1 : 0);
            exit(EXIT_FAILURE);
        }
    }
    ssize_t pwr = pwrite(wfd, wbuf, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
    {
        int e = errno;
        if (e == EOVERFLOW)
        {
            /* Re-sync WP from device; if FULL, seal + release the slot. */
            struct zbd_zone zinfo;
            unsigned int nz = 1;
            if (zbd_report_zones(t->fd,
                                 (off_t)t->zones[target_zone].start,
                                 (off_t)t->info.zone_size,
                                 ZBD_RO_ALL, &zinfo, &nz) == 0 && nz > 0)
            {
                atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                      (uint64_t)zinfo.wp, memory_order_release);
                if (zinfo.cond == ZBD_ZONE_COND_FULL)
                {
                    atomic_store_explicit(&t->zone_full[target_zone], 1,
                                          memory_order_release);
                    zone_admission_release_zone(&t->za, target_zone);
                }
            }
            else
            {
                atomic_fetch_sub_explicit(&t->zone_wp_bytes[target_zone],
                                          ZTREE_PAGE_SIZE, memory_order_relaxed);
            }
            record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
            pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            if (++eoverflow_retries > 1000)
            {
                fprintf(stderr,
                        "flush_page_immediate(leaf): EOVERFLOW unrecoverable "
                        "target_zone=%u node_id=%llu\n",
                        target_zone, (unsigned long long)pg->node_id);
                exit(EXIT_FAILURE);
            }
            usleep(500);
            goto retry_flush;
        }
        fprintf(stderr,
                "flush_page_immediate(leaf): pwrite at 0x%llx ret=%zd errno=%d (%s) "
                "target_zone=%u prev_zone=%u avoid=%u sticky=%d\n",
                (unsigned long long)cur_wp, pwr, e, strerror(e),
                target_zone, prev_zone, avoid_zone,
                sticky_ok ? 1 : 0);
        exit(EXIT_FAILURE);
    }

    record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
    pthread_mutex_unlock(&t->zone_write_locks[target_zone]);

    uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
    uint64_t zone_end = t->zones[target_zone].start + t->zones[target_zone].capacity;
    if (new_wp >= zone_end)
    {
        atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
        zone_seal_and_replace(&t->za, target_zone);
    }

    cache_insert(t, pn, pg);
    nlt_location_t loc = {
        .zone_id = target_zone,
        .node_id = pg->node_id,
        .slot_id = slot_id,
    };
    /* Atomic insert-new + remove-stale-from-prev so each node has exactly
     * one bucket entry (paper §3.1.2 "latest valid" invariant). */
    nlt_update_migrate(&t->nlt, &loc, prev_zone);
    zone_valid_leaves_move(t, prev_zone, target_zone);

    uint64_t zone_bytes_used = new_wp - t->zones[target_zone].start;
    uint64_t seal_threshold = (t->zones[target_zone].capacity * 95ULL) / 100ULL;
    if (zone_bytes_used >= seal_threshold)
    {
        atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
        nlt_set_zone_sealed(&t->nlt, target_zone, true);
        zone_seal_and_replace(&t->za, target_zone);
    }

    /* Heat tracking (leaf-only). */
    if (prev_zone != ZTREE_INVALID_ZONE_ID && prev_zone != target_zone)
        zone_heat_reset(&t->za, pg->node_id);
    zone_heat_record_write(&t->za, pg->node_id);

    atomic_fetch_add_explicit(&t->stat_page_appends, 1, memory_order_relaxed);
    maybe_trace_sample(t);

    if (prev_zone != ZTREE_INVALID_ZONE_ID && prev_zone == target_zone)
    {
        if (out_zone_changed)
            *out_zone_changed = 0;
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
    }
    else
    {
        if (out_zone_changed)
            *out_zone_changed = 1;
        atomic_fetch_add_explicit(&t->stat_zone_changes, 1, memory_order_relaxed);
    }

    if (out_zone)
        *out_zone = target_zone;
    if (out_slot)
        *out_slot = slot_id;
}

static uint32_t child_pos_for_key(const ztree_page *p, int64_t key)
{
    for (uint32_t i = 0; i < p->num_keys; i++)
    {
        if (key < (int64_t)p->internal[i].key)
            return i;
    }
    return RIGHTMOST_IDX;
}

static uint32_t child_pos_for_id(const ztree_page *p, ztree_node_id_t child_id)
{
    for (uint32_t i = 0; i < p->num_keys; i++)
    {
        if (p->internal[i].child_node_id == child_id)
            return i;
    }
    if (p->ptr_node_id == child_id)
        return RIGHTMOST_IDX;
    return UINT32_MAX;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Direct Concurrent Insert Path
 * ═══════════════════════════════════════════════════════════════════════════ */

static int try_publish_root_if_unchanged(ztree_t *t,
                                         uint64_t expected_seq_even,
                                         ztree_node_id_t root_nid,
                                         uint32_t root_zone,
                                         uint32_t root_slot)
{
    uint64_t expected = expected_seq_even;
    int cas_success = atomic_compare_exchange_strong_explicit(&t->volatile_sb.seq_no,
                                                              &expected,
                                                              expected_seq_even + 1,
                                                              memory_order_acq_rel,
                                                              memory_order_acquire);
    if (!cas_success)
        return 0;

    atomic_store_explicit(&t->volatile_sb.root_node_id, root_nid, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id, root_zone, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id, root_slot, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no, expected_seq_even + 2, memory_order_release);
    atomic_store_explicit(&t->dirty_sb, true, memory_order_release);
    (void)atomic_fetch_add_explicit(&t->txg_next, 1, memory_order_acq_rel);
    return 1;
}

static void do_single_insert(ztree_t *t, int64_t key, const char *value)
{
    for (uint32_t retry = 0;; retry++)
    {
        if (retry > 0)
        {
            if ((retry & 0xFU) == 0)
                sched_yield();
            if (retry >= 10000U)
            {
                fprintf(stderr, "cow_insert: excessive publish retries (key=%ld)\n", (long)key);
                exit(EXIT_FAILURE);
            }
        }

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot;
        uint64_t seq_snapshot;

        for (;;)
        {
            uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
            if (s1 & 1ULL)
                continue;
            root_nid = atomic_load_explicit(&t->volatile_sb.root_node_id, memory_order_acquire);
            root_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id, memory_order_acquire);
            root_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id, memory_order_acquire);
            uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
            if (s1 == s2 && (s2 & 1ULL) == 0)
            {
                seq_snapshot = s2;
                break;
            }
        }
        uint64_t apply_t0 = monotonic_ns();

        if (root_nid == ZTREE_INVALID_NODE_ID)
        {
            ztree_page root;
            memset(&root, 0, sizeof root);
            root.is_leaf = 1;
            root.node_id = assign_stable_node_id(t);
            root.num_keys = 1;
            root.leaf[0].key = (uint64_t)key;
            memcpy(root.leaf[0].record.value, value, 120);

            int zchg = 1;
            uint32_t rz, rs;
            flush_page_immediate(t, &root, ZTREE_INVALID_ZONE_ID,
                                 ZTREE_INVALID_ZONE_ID, &zchg, &rz, &rs);

            uint64_t apply_dt = monotonic_ns() - apply_t0;
            atomic_fetch_add_explicit(&t->stat_apply_ns_sum, apply_dt, memory_order_relaxed);
            atomic_fetch_add_explicit(&t->stat_apply_ns_samples, 1, memory_order_relaxed);

            if (try_publish_root_if_unchanged(t, seq_snapshot, root.node_id, rz, rs))
            {
                atomic_fetch_add_explicit(&t->stat_inserts, 1, memory_order_relaxed);
                return;
            }
            continue;
        }

        insert_path_frame path[MAX_HEIGHT];
        int depth = 0;

        ztree_node_id_t cur_id = root_nid;
        uint32_t cur_zone = root_zone;
        node_rdlock(t, cur_id);

        while (1)
        {
            if (depth >= MAX_HEIGHT)
            {
                node_unlock(t, cur_id);
                fprintf(stderr, "do_single_insert: depth overflow\n");
                exit(EXIT_FAILURE);
            }

            insert_path_frame *f = &path[depth];
            f->node_id = cur_id;
            if (!load_latest_node(t, cur_zone, cur_id, &f->zone_id, &f->slot_id, &f->page))
            {
                node_unlock(t, cur_id);
                goto retry_insert;
            }

            if (f->page.is_leaf)
            {
                node_unlock(t, cur_id);
                node_wrlock(t, cur_id);

                if (!load_latest_node(t, cur_zone, cur_id,
                                      &f->zone_id, &f->slot_id, &f->page))
                {
                    node_unlock(t, cur_id);
                    goto retry_insert;
                }

                if (!f->page.is_leaf)
                {
                    node_unlock(t, cur_id);
                    goto retry_insert;
                }

                if (depth >= 1)
                {
                    insert_path_frame *pf = &path[depth - 1];
                    ztree_page latest_parent;
                    uint32_t pzone, pslot;
                    if (!load_latest_node(t, pf->zone_id, pf->node_id,
                                          &pzone, &pslot, &latest_parent)
                        || latest_parent.is_leaf)
                    {
                        node_unlock(t, cur_id);
                        goto retry_insert;
                    }

                    uint32_t new_cidx = child_pos_for_key(&latest_parent, key);
                    ztree_node_id_t routed = (new_cidx == RIGHTMOST_IDX)
                                                 ? latest_parent.ptr_node_id
                                                 : latest_parent.internal[new_cidx].child_node_id;
                    if (routed != cur_id)
                    {
                        node_unlock(t, cur_id);
                        goto retry_insert;
                    }
                }

                depth++;
                break;
            }

            uint32_t cidx = child_pos_for_key(&f->page, key);
            f->cidx_from_parent = cidx;

            ztree_node_id_t next_id = (cidx == RIGHTMOST_IDX)
                                          ? f->page.ptr_node_id
                                          : f->page.internal[cidx].child_node_id;

            if (node_latch_for_id(t, next_id) != node_latch_for_id(t, cur_id))
            {
                node_rdlock(t, next_id);
                node_unlock(t, cur_id);
            }

            uint32_t next_zone = (cidx == RIGHTMOST_IDX)
                                     ? f->page.ptr_zone_id
                                     : f->page.internal[cidx].child_zone_id;
            cur_zone = next_zone;
            cur_id = next_id;
            depth++;
        }

        propagate_state prop;
        memset(&prop, 0, sizeof prop);

        insert_path_frame *leaff = &path[depth - 1];
        ztree_page *leaf = &leaff->page;

        int updated = 0;
        for (uint32_t i = 0; i < leaf->num_keys; i++)
        {
            if ((int64_t)leaf->leaf[i].key == key)
            {
                memcpy(leaf->leaf[i].record.value, value, 120);
                updated = 1;
                break;
            }
        }

        if (!updated)
        {
            if (leaf->num_keys < ZTREE_LEAF_ORDER - 1)
            {
                uint32_t pos = 0;
                while (pos < leaf->num_keys && (int64_t)leaf->leaf[pos].key < key)
                    pos++;
                for (int64_t i = (int64_t)leaf->num_keys - 1; i >= (int64_t)pos; i--)
                    leaf->leaf[i + 1] = leaf->leaf[i];
                leaf->leaf[pos].key = (uint64_t)key;
                memcpy(leaf->leaf[pos].record.value, value, 120);
                leaf->num_keys++;
            }
            else
            {
                ztree_leaf_entity tmp[ZTREE_LEAF_ORDER];
                uint32_t pos = 0;
                while (pos < leaf->num_keys && (int64_t)leaf->leaf[pos].key < key)
                    pos++;
                for (uint32_t i = 0; i < pos; i++)
                    tmp[i] = leaf->leaf[i];
                for (uint32_t i = pos; i < leaf->num_keys; i++)
                    tmp[i + 1] = leaf->leaf[i];
                tmp[pos].key = (uint64_t)key;
                memcpy(tmp[pos].record.value, value, 120);

                uint32_t sp = ZTREE_LEAF_ORDER / 2;
                ztree_page right;
                memset(&right, 0, sizeof right);
                right.is_leaf = 1;
                right.node_id = assign_stable_node_id(t);

                zone_heat_inherit(&t->za, right.node_id, leaf->node_id);

                for (uint32_t i = 0; i < sp; i++)
                    leaf->leaf[i] = tmp[i];
                leaf->num_keys = sp;

                right.num_keys = ZTREE_LEAF_ORDER - sp;
                for (uint32_t i = 0; i < right.num_keys; i++)
                    right.leaf[i] = tmp[sp + i];
                right.ptr_node_id = leaf->ptr_node_id;
                right.ptr_zone_id = leaf->ptr_zone_id;

                leaf->ptr_node_id = right.node_id;
                leaf->ptr_zone_id = ZTREE_INVALID_ZONE_ID;

                int rzchg = 1;
                flush_page_immediate(t, &right, ZTREE_INVALID_ZONE_ID,
                                     leaff->zone_id,
                                     &rzchg, &prop.right_zone, &prop.right_slot);

                prop.split = 1;
                prop.promote_key = (int64_t)right.leaf[0].key;
                prop.right_id = right.node_id;
            }
        }

        flush_page_immediate(t, leaf, leaff->zone_id,
                             ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed, &prop.left_zone, &prop.left_slot);
        prop.left_id = leaf->node_id;

        node_unlock(t, leaff->node_id);

        if (prop.split || prop.left_zone_changed)
        {
            for (int level = depth - 2; level >= 0; level--)
            {
                if (!prop.split && !prop.left_zone_changed)
                    break;

                insert_path_frame *pf = &path[level];
                node_wrlock(t, pf->node_id);
                if (!load_latest_node(t, pf->zone_id, pf->node_id, &pf->zone_id, &pf->slot_id, &pf->page))
                {
                    node_unlock(t, pf->node_id);
                    goto retry_insert;
                }

                ztree_page *par = &pf->page;
                uint32_t cidx = child_pos_for_id(par, prop.left_id);
                if (cidx == UINT32_MAX)
                {
                    cidx = child_pos_for_key(par, key);
                    if (cidx != RIGHTMOST_IDX)
                    {
                        if (par->internal[cidx].child_node_id != prop.left_id)
                        {
                            node_unlock(t, pf->node_id);
                            goto retry_insert;
                        }
                    }
                    else if (par->ptr_node_id != prop.left_id)
                    {
                        node_unlock(t, pf->node_id);
                        goto retry_insert;
                    }
                }

                if (cidx == RIGHTMOST_IDX)
                    par->ptr_zone_id = prop.left_zone;
                else
                    par->internal[cidx].child_zone_id = prop.left_zone;

                if (prop.split)
                {
                    uint32_t pos = (cidx == RIGHTMOST_IDX) ? par->num_keys : cidx;
                    if (par->num_keys < ZTREE_INTERNAL_ORDER - 1)
                    {
                        for (int64_t j = (int64_t)par->num_keys - 1; j >= (int64_t)pos; j--)
                            par->internal[j + 1] = par->internal[j];

                        par->internal[pos].key = (uint64_t)prop.promote_key;
                        par->internal[pos].child_node_id = prop.left_id;
                        par->internal[pos].child_zone_id = prop.left_zone;

                        if (pos == par->num_keys)
                        {
                            par->ptr_node_id = prop.right_id;
                            par->ptr_zone_id = prop.right_zone;
                        }
                        else
                        {
                            par->internal[pos + 1].child_node_id = prop.right_id;
                            par->internal[pos + 1].child_zone_id = prop.right_zone;
                        }
                        par->num_keys++;
                        prop.split = 0;
                    }
                    else
                    {
                        int64_t tkeys[ZTREE_INTERNAL_ORDER];
                        ztree_node_id_t tchld[ZTREE_INTERNAL_ORDER + 1];
                        uint32_t tchld_zone[ZTREE_INTERNAL_ORDER + 1];

                        for (uint32_t j = 0; j < pos; j++)
                            tkeys[j] = (int64_t)par->internal[j].key;
                        tkeys[pos] = prop.promote_key;
                        for (uint32_t j = pos; j < ZTREE_INTERNAL_ORDER - 1; j++)
                            tkeys[j + 1] = (int64_t)par->internal[j].key;

                        for (uint32_t j = 0; j < pos; j++)
                        {
                            tchld[j] = par->internal[j].child_node_id;
                            tchld_zone[j] = par->internal[j].child_zone_id;
                        }
                        tchld[pos] = prop.left_id;
                        tchld_zone[pos] = prop.left_zone;
                        tchld[pos + 1] = prop.right_id;
                        tchld_zone[pos + 1] = prop.right_zone;
                        for (uint32_t j = pos + 1; j < ZTREE_INTERNAL_ORDER; j++)
                        {
                            if (j < ZTREE_INTERNAL_ORDER - 1)
                            {
                                tchld[j + 1] = par->internal[j].child_node_id;
                                tchld_zone[j + 1] = par->internal[j].child_zone_id;
                            }
                            else
                            {
                                tchld[j + 1] = par->ptr_node_id;
                                tchld_zone[j + 1] = par->ptr_zone_id;
                            }
                        }

                        uint32_t sp = (ZTREE_INTERNAL_ORDER + 1) / 2;
                        int64_t up_key = tkeys[sp - 1];

                        for (uint32_t j = 0; j < sp - 1; j++)
                        {
                            par->internal[j].key = (uint64_t)tkeys[j];
                            par->internal[j].child_node_id = tchld[j];
                            par->internal[j].child_zone_id = tchld_zone[j];
                        }
                        par->ptr_node_id = tchld[sp - 1];
                        par->ptr_zone_id = tchld_zone[sp - 1];
                        par->num_keys = sp - 1;

                        ztree_page right;
                        memset(&right, 0, sizeof right);
                        right.is_leaf = 0;
                        right.node_id = assign_stable_node_id(t);
                        atomic_fetch_add_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
                        right.num_keys = ZTREE_INTERNAL_ORDER - sp;
                        for (uint32_t j = sp; j < ZTREE_INTERNAL_ORDER; j++)
                        {
                            right.internal[j - sp].key = (uint64_t)tkeys[j];
                            right.internal[j - sp].child_node_id = tchld[j];
                            right.internal[j - sp].child_zone_id = tchld_zone[j];
                        }
                        right.ptr_node_id = tchld[ZTREE_INTERNAL_ORDER];
                        right.ptr_zone_id = tchld_zone[ZTREE_INTERNAL_ORDER];

                        int rzchg = 1;
                        flush_page_immediate(t, &right, ZTREE_INVALID_ZONE_ID,
                                             pf->zone_id,
                                             &rzchg, &prop.right_zone, &prop.right_slot);
                        prop.right_id = right.node_id;
                        prop.promote_key = up_key;
                        prop.split = 1;
                    }
                }

                flush_page_immediate(t, par, pf->zone_id,
                                     ZTREE_INVALID_ZONE_ID,
                                     &prop.left_zone_changed, &prop.left_zone, &prop.left_slot);
                prop.left_id = par->node_id;
                if (prop.left_zone_changed)
                {
                    atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1, memory_order_relaxed);
                }

                node_unlock(t, pf->node_id);
            }
        }

        ztree_node_id_t new_root_nid = root_nid;
        uint32_t new_root_zone = root_zone;
        uint32_t new_root_slot = root_slot;
        int need_root_publish = 0;

        if (prop.split)
        {
            ztree_page nr;
            memset(&nr, 0, sizeof nr);
            nr.is_leaf = 0;
            nr.node_id = assign_stable_node_id(t);
            atomic_fetch_add_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&t->tree_height, 1, memory_order_relaxed);
            nr.num_keys = 1;
            nr.internal[0].key = (uint64_t)prop.promote_key;
            nr.internal[0].child_node_id = prop.left_id;
            nr.internal[0].child_zone_id = prop.left_zone;
            nr.ptr_node_id = prop.right_id;
            nr.ptr_zone_id = prop.right_zone;

            int zchg = 1;
            flush_page_immediate(t, &nr, ZTREE_INVALID_ZONE_ID,
                                 ZTREE_INVALID_ZONE_ID,
                                 &zchg, &new_root_zone, &new_root_slot);
            new_root_nid = nr.node_id;
            need_root_publish = 1;
        }
        else if (prop.left_id == root_nid && prop.left_zone_changed)
        {
            new_root_nid = prop.left_id;
            new_root_zone = prop.left_zone;
            new_root_slot = prop.left_slot;
            need_root_publish = 1;
        }

        {
            uint64_t apply_dt = monotonic_ns() - apply_t0;
            atomic_fetch_add_explicit(&t->stat_apply_ns_sum, apply_dt, memory_order_relaxed);
            atomic_fetch_add_explicit(&t->stat_apply_ns_samples, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&t->stat_flush_ns_sum, apply_dt, memory_order_relaxed);
            atomic_fetch_add_explicit(&t->stat_flush_ns_samples, 1, memory_order_relaxed);
        }

        if (!need_root_publish)
        {
            atomic_fetch_add_explicit(&t->stat_inserts, 1, memory_order_relaxed);
            return;
        }

        if (new_root_nid == root_nid &&
            new_root_zone == root_zone &&
            new_root_slot == root_slot)
        {
            atomic_fetch_add_explicit(&t->stat_inserts, 1, memory_order_relaxed);
            return;
        }

        if (try_publish_root_if_unchanged(t, seq_snapshot,
                                          new_root_nid, new_root_zone, new_root_slot))
        {
            atomic_fetch_add_explicit(&t->stat_inserts, 1, memory_order_relaxed);
            return;
        }

    retry_insert:;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW delete path (paper Algorithm 4 + cascade + root collapse)
 *
 * Same b1 concurrency model as base ctree.  In ilayer, internal-node CoW
 * always lands on CNS at slot_id == node_id, so flush_page_immediate
 * returns out_zone_changed == 0 for internals — propagate-zone-up will
 * naturally short-circuit at the leaf's immediate parent.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    ztree_node_id_t node_id;
    uint32_t zone_id;
    uint32_t slot_id;
    ztree_page page;
    uint32_t cidx_from_parent;  /* RIGHTMOST_IDX if linked via ptr_* */
} delete_path_frame;

#define DEL_OK          0
#define DEL_NOT_FOUND   1
#define DEL_RACE        2
#define DEL_NEEDS_MERGE 3

static void delete_release_path_locks(ztree_t *t, delete_path_frame *path, int n)
{
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (node_latch_for_id(t, path[i].node_id) ==
                node_latch_for_id(t, path[j].node_id)) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            node_unlock(t, path[i].node_id);
    }
}

static int delete_propagate_zone_up(ztree_t *t,
                                    delete_path_frame *path, int depth,
                                    int64_t key,
                                    propagate_state *prop)
{
    for (int level = depth - 2; level >= 0 && prop->left_zone_changed; level--) {
        delete_path_frame *pf = &path[level];
        node_wrlock(t, pf->node_id);
        if (!load_latest_node(t, pf->zone_id, pf->node_id,
                              &pf->zone_id, &pf->slot_id, &pf->page)) {
            node_unlock(t, pf->node_id);
            return -1;
        }
        ztree_page *par = &pf->page;
        uint32_t cidx = child_pos_for_id(par, prop->left_id);
        if (cidx == UINT32_MAX) {
            cidx = child_pos_for_key(par, key);
            ztree_node_id_t expected = (cidx == RIGHTMOST_IDX)
                                           ? par->ptr_node_id
                                           : par->internal[cidx].child_node_id;
            if (expected != prop->left_id) {
                node_unlock(t, pf->node_id);
                return -1;
            }
        }
        if (cidx == RIGHTMOST_IDX) {
            par->ptr_zone_id = prop->left_zone;
        } else {
            par->internal[cidx].child_zone_id = prop->left_zone;
        }
        flush_page_immediate(t, par, pf->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop->left_zone_changed,
                             &prop->left_zone, &prop->left_slot);
        prop->left_id = par->node_id;
        if (prop->left_zone_changed)
            atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1,
                                      memory_order_relaxed);
        node_unlock(t, pf->node_id);
    }
    return 0;
}

static int try_optimistic_delete(ztree_t *t, int64_t key,
                                 ztree_node_id_t root_nid, uint32_t root_zone,
                                 uint32_t root_slot, uint64_t seq_snapshot,
                                 ztree_node_id_t *out_root_nid,
                                 uint32_t *out_root_zone,
                                 uint32_t *out_root_slot,
                                 int *out_root_changed)
{
    delete_path_frame path[MAX_HEIGHT];
    int depth = 0;

    ztree_node_id_t cur_id = root_nid;
    uint32_t cur_zone = root_zone;
    node_rdlock(t, cur_id);

    while (1) {
        if (depth >= MAX_HEIGHT) {
            node_unlock(t, cur_id);
            fprintf(stderr, "delete: depth overflow\n");
            exit(EXIT_FAILURE);
        }
        delete_path_frame *f = &path[depth];
        f->node_id = cur_id;
        f->cidx_from_parent = (depth == 0) ? RIGHTMOST_IDX
                                            : path[depth - 1].cidx_from_parent;
        if (!load_latest_node(t, cur_zone, cur_id,
                              &f->zone_id, &f->slot_id, &f->page)) {
            node_unlock(t, cur_id);
            return DEL_RACE;
        }
        if (f->page.is_leaf) {
            node_unlock(t, cur_id);
            node_wrlock(t, cur_id);
            if (!load_latest_node(t, cur_zone, cur_id,
                                  &f->zone_id, &f->slot_id, &f->page)) {
                node_unlock(t, cur_id);
                return DEL_RACE;
            }
            if (!f->page.is_leaf) {
                node_unlock(t, cur_id);
                return DEL_RACE;
            }
            if (depth >= 1) {
                delete_path_frame *pf = &path[depth - 1];
                ztree_page latest_par;
                uint32_t pzone, pslot;
                if (!load_latest_node(t, pf->zone_id, pf->node_id,
                                      &pzone, &pslot, &latest_par)
                    || latest_par.is_leaf) {
                    node_unlock(t, cur_id);
                    return DEL_RACE;
                }
                uint32_t new_cidx = child_pos_for_key(&latest_par, key);
                ztree_node_id_t routed = (new_cidx == RIGHTMOST_IDX)
                                              ? latest_par.ptr_node_id
                                              : latest_par.internal[new_cidx].child_node_id;
                if (routed != cur_id) {
                    node_unlock(t, cur_id);
                    return DEL_RACE;
                }
            }
            depth++;
            break;
        }
        uint32_t cidx = child_pos_for_key(&f->page, key);
        ztree_node_id_t next_id = (cidx == RIGHTMOST_IDX)
                                       ? f->page.ptr_node_id
                                       : f->page.internal[cidx].child_node_id;
        if (node_latch_for_id(t, next_id) != node_latch_for_id(t, cur_id)) {
            node_rdlock(t, next_id);
            node_unlock(t, cur_id);
        }
        cur_zone = (cidx == RIGHTMOST_IDX) ? f->page.ptr_zone_id
                                            : f->page.internal[cidx].child_zone_id;
        cur_id = next_id;
        path[depth + 1].cidx_from_parent = cidx;
        depth++;
    }

    delete_path_frame *leaff = &path[depth - 1];
    ztree_page *leaf = &leaff->page;
    int found_idx = -1;
    for (uint32_t i = 0; i < leaf->num_keys; i++) {
        if ((int64_t)leaf->leaf[i].key == key) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        node_unlock(t, leaff->node_id);
        return DEL_NOT_FOUND;
    }

    int is_root_leaf = (depth == 1);
    uint32_t new_count = leaf->num_keys - 1;
    int needs_merge = (!is_root_leaf && new_count < ZTREE_LEAF_MIN);
    if (needs_merge) {
        node_unlock(t, leaff->node_id);
        return DEL_NEEDS_MERGE;
    }

    for (uint32_t i = (uint32_t)found_idx; i + 1 < leaf->num_keys; i++)
        leaf->leaf[i] = leaf->leaf[i + 1];
    leaf->num_keys = new_count;

    if (is_root_leaf && leaf->num_keys == 0) {
        node_unlock(t, leaff->node_id);
        *out_root_nid = ZTREE_INVALID_NODE_ID;
        *out_root_zone = ZTREE_INVALID_ZONE_ID;
        *out_root_slot = ZTREE_INVALID_SLOT_ID;
        *out_root_changed = 1;
        (void)seq_snapshot;
        return DEL_OK;
    }

    propagate_state prop;
    memset(&prop, 0, sizeof prop);
    flush_page_immediate(t, leaf, leaff->zone_id, ZTREE_INVALID_ZONE_ID,
                         &prop.left_zone_changed,
                         &prop.left_zone, &prop.left_slot);
    prop.left_id = leaf->node_id;
    node_unlock(t, leaff->node_id);

    if (delete_propagate_zone_up(t, path, depth, key, &prop) != 0)
        return DEL_RACE;

    if (prop.left_id == root_nid) {
        *out_root_nid = root_nid;
        *out_root_zone = prop.left_zone;
        *out_root_slot = prop.left_slot;
        *out_root_changed = 1;
    } else {
        *out_root_nid = root_nid;
        *out_root_zone = root_zone;
        *out_root_slot = root_slot;
        *out_root_changed = 0;
    }
    return DEL_OK;
}

static int try_pessimistic_delete(ztree_t *t, int64_t key,
                                  ztree_node_id_t root_nid, uint32_t root_zone,
                                  uint32_t root_slot,
                                  ztree_node_id_t *out_root_nid,
                                  uint32_t *out_root_zone,
                                  uint32_t *out_root_slot,
                                  int *out_root_changed)
{
    delete_path_frame path[MAX_HEIGHT];
    int depth = 0;

    ztree_node_id_t cur_id = root_nid;
    uint32_t cur_zone = root_zone;
    uint32_t pending_cidx = RIGHTMOST_IDX;
    node_wrlock(t, cur_id);

    while (1) {
        if (depth >= MAX_HEIGHT) {
            delete_release_path_locks(t, path, depth);
            node_unlock(t, cur_id);
            fprintf(stderr, "delete: depth overflow (pessimistic)\n");
            exit(EXIT_FAILURE);
        }
        delete_path_frame *f = &path[depth];
        f->node_id = cur_id;
        f->cidx_from_parent = pending_cidx;
        if (!load_latest_node(t, cur_zone, cur_id,
                              &f->zone_id, &f->slot_id, &f->page)) {
            delete_release_path_locks(t, path, depth + 1);
            return DEL_RACE;
        }
        if (f->page.is_leaf) {
            depth++;
            break;
        }
        uint32_t cidx = child_pos_for_key(&f->page, key);
        ztree_node_id_t next_id = (cidx == RIGHTMOST_IDX)
                                       ? f->page.ptr_node_id
                                       : f->page.internal[cidx].child_node_id;
        uint32_t next_zone = (cidx == RIGHTMOST_IDX)
                                  ? f->page.ptr_zone_id
                                  : f->page.internal[cidx].child_zone_id;

        int already_held = 0;
        for (int j = 0; j <= depth; j++) {
            if (node_latch_for_id(t, next_id) ==
                node_latch_for_id(t, path[j].node_id)) {
                already_held = 1;
                break;
            }
        }
        if (!already_held)
            node_wrlock(t, next_id);

        cur_id = next_id;
        cur_zone = next_zone;
        pending_cidx = cidx;
        depth++;
    }

    delete_path_frame *leaff = &path[depth - 1];
    ztree_page *leaf = &leaff->page;
    int found_idx = -1;
    for (uint32_t i = 0; i < leaf->num_keys; i++) {
        if ((int64_t)leaf->leaf[i].key == key) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        delete_release_path_locks(t, path, depth);
        return DEL_NOT_FOUND;
    }

    for (uint32_t i = (uint32_t)found_idx; i + 1 < leaf->num_keys; i++)
        leaf->leaf[i] = leaf->leaf[i + 1];
    leaf->num_keys--;

    if (depth == 1 && leaf->num_keys == 0) {
        delete_release_path_locks(t, path, depth);
        *out_root_nid = ZTREE_INVALID_NODE_ID;
        *out_root_zone = ZTREE_INVALID_ZONE_ID;
        *out_root_slot = ZTREE_INVALID_SLOT_ID;
        *out_root_changed = 1;
        return DEL_OK;
    }

    propagate_state prop;
    memset(&prop, 0, sizeof prop);
    int level = depth - 1;
    int merge_done_at_level = -1;
    int root_collapsed = 0;
    ztree_node_id_t collapsed_root_nid = ZTREE_INVALID_NODE_ID;
    uint32_t collapsed_root_zone = ZTREE_INVALID_ZONE_ID;
    uint32_t collapsed_root_slot = ZTREE_INVALID_SLOT_ID;

    while (level > 0) {
        delete_path_frame *cur_f = &path[level];
        delete_path_frame *par_f = &path[level - 1];
        ztree_page *cur = &cur_f->page;
        ztree_page *par = &par_f->page;
        uint32_t cidx = cur_f->cidx_from_parent;

        uint32_t min_keys = cur->is_leaf ? ZTREE_LEAF_MIN : ZTREE_INTERNAL_MIN;
        if (cur->num_keys >= min_keys) {
            break;
        }

        ztree_node_id_t sib_id = ZTREE_INVALID_NODE_ID;
        uint32_t sib_zone = ZTREE_INVALID_ZONE_ID;
        uint32_t sib_cidx = UINT32_MAX;
        int sib_is_right = 0;
        int sib_was_rightmost = 0;
        if (cidx == RIGHTMOST_IDX) {
            if (par->num_keys == 0) break;
            sib_cidx = par->num_keys - 1;
            sib_id = par->internal[sib_cidx].child_node_id;
            sib_zone = par->internal[sib_cidx].child_zone_id;
            sib_is_right = 0;
        } else if (cidx + 1 < par->num_keys) {
            sib_cidx = cidx + 1;
            sib_id = par->internal[sib_cidx].child_node_id;
            sib_zone = par->internal[sib_cidx].child_zone_id;
            sib_is_right = 1;
        } else if (cidx + 1 == par->num_keys) {
            sib_cidx = par->num_keys;
            sib_id = par->ptr_node_id;
            sib_zone = par->ptr_zone_id;
            sib_is_right = 1;
            sib_was_rightmost = 1;
        } else if (cidx > 0) {
            sib_cidx = cidx - 1;
            sib_id = par->internal[sib_cidx].child_node_id;
            sib_zone = par->internal[sib_cidx].child_zone_id;
            sib_is_right = 0;
        } else {
            break;
        }

        int sib_already_held = 0;
        for (int j = 0; j < depth; j++) {
            if (node_latch_for_id(t, sib_id) ==
                node_latch_for_id(t, path[j].node_id)) {
                sib_already_held = 1;
                break;
            }
        }
        if (!sib_already_held)
            node_wrlock(t, sib_id);

        ztree_page sib_page;
        uint32_t sib_actual_zone, sib_actual_slot;
        if (!load_latest_node(t, sib_zone, sib_id,
                              &sib_actual_zone, &sib_actual_slot, &sib_page)) {
            if (!sib_already_held) node_unlock(t, sib_id);
            delete_release_path_locks(t, path, depth);
            return DEL_RACE;
        }

        uint32_t cur_keys = cur->num_keys;
        uint32_t sib_keys = sib_page.num_keys;
        uint32_t merged_keys, max_keys;
        if (cur->is_leaf) {
            merged_keys = cur_keys + sib_keys;
            max_keys = ZTREE_LEAF_ORDER - 1;
        } else {
            merged_keys = cur_keys + 1 + sib_keys;
            max_keys = ZTREE_INTERNAL_ORDER - 1;
        }

        if (merged_keys > max_keys) {
            if (!sib_already_held) node_unlock(t, sib_id);
            break;
        }

        if (cur->is_leaf) {
            if (sib_is_right) {
                for (uint32_t i = 0; i < sib_keys; i++)
                    cur->leaf[cur_keys + i] = sib_page.leaf[i];
                cur->num_keys = cur_keys + sib_keys;
                cur->ptr_node_id = sib_page.ptr_node_id;
                cur->ptr_zone_id = sib_page.ptr_zone_id;
            } else {
                ztree_leaf_entity tmp[ZTREE_LEAF_ORDER];
                for (uint32_t i = 0; i < sib_keys; i++) tmp[i] = sib_page.leaf[i];
                for (uint32_t i = 0; i < cur_keys; i++) tmp[sib_keys + i] = cur->leaf[i];
                for (uint32_t i = 0; i < sib_keys + cur_keys; i++) cur->leaf[i] = tmp[i];
                cur->num_keys = sib_keys + cur_keys;
            }
        } else {
            uint64_t separator;
            if (sib_is_right) {
                separator = par->internal[cidx].key;
            } else {
                separator = par->internal[sib_cidx].key;
            }
            ztree_page *left_p = sib_is_right ? cur : &sib_page;
            ztree_page *right_p = sib_is_right ? &sib_page : cur;

            ztree_internal_entity tmp_ent[ZTREE_INTERNAL_ORDER];
            uint32_t pos = 0;
            for (uint32_t i = 0; i < left_p->num_keys; i++)
                tmp_ent[pos++] = left_p->internal[i];
            tmp_ent[pos].key = separator;
            tmp_ent[pos].child_node_id = left_p->ptr_node_id;
            tmp_ent[pos].child_zone_id = left_p->ptr_zone_id;
            pos++;
            for (uint32_t i = 0; i < right_p->num_keys; i++)
                tmp_ent[pos++] = right_p->internal[i];

            for (uint32_t i = 0; i < pos; i++) cur->internal[i] = tmp_ent[i];
            cur->num_keys = pos;
            cur->ptr_node_id = right_p->ptr_node_id;
            cur->ptr_zone_id = right_p->ptr_zone_id;
        }

        flush_page_immediate(t, cur, cur_f->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed,
                             &prop.left_zone, &prop.left_slot);
        prop.left_id = cur->node_id;

        uint32_t k_idx = sib_is_right ? cidx : sib_cidx;
        uint32_t c_idx = sib_is_right ? (sib_was_rightmost ? par->num_keys
                                                            : sib_cidx)
                                       : sib_cidx;
        for (uint32_t j = k_idx; j + 1 < par->num_keys; j++)
            par->internal[j].key = par->internal[j + 1].key;
        if (sib_was_rightmost) {
            par->ptr_node_id = cur->node_id;
            par->ptr_zone_id = prop.left_zone;
        } else {
            for (uint32_t j = c_idx; j + 1 < par->num_keys; j++) {
                par->internal[j].child_node_id =
                    par->internal[j + 1].child_node_id;
                par->internal[j].child_zone_id =
                    par->internal[j + 1].child_zone_id;
            }
        }
        par->num_keys--;

        if (sib_is_right && !sib_was_rightmost) {
            par->internal[cidx].child_zone_id = prop.left_zone;
            par->internal[cidx].child_node_id = cur->node_id;
        } else if (!sib_is_right) {
            par->internal[sib_cidx].child_node_id = cur->node_id;
            par->internal[sib_cidx].child_zone_id = prop.left_zone;
        }

        if (!sib_already_held) node_unlock(t, sib_id);

        atomic_fetch_add_explicit(&t->stat_delete_merges, 1, memory_order_relaxed);

        if (level - 1 == 0 && par->num_keys == 0) {
            nlt_location_t q = { .zone_id = par->ptr_zone_id,
                                 .node_id = par->ptr_node_id,
                                 .slot_id = ZTREE_INVALID_SLOT_ID };
            nlt_location_t r;
            if (!nlt_lookup(&t->nlt, &q, &r)) {
                delete_release_path_locks(t, path, depth);
                return DEL_RACE;
            }
            root_collapsed = 1;
            collapsed_root_nid = r.node_id;
            collapsed_root_zone = r.zone_id;
            collapsed_root_slot = r.slot_id;
            atomic_fetch_add_explicit(&t->stat_delete_root_collapses, 1,
                                      memory_order_relaxed);
            level = 0;
            break;
        }

        flush_page_immediate(t, par, par_f->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed,
                             &prop.left_zone, &prop.left_slot);
        prop.left_id = par->node_id;
        if ((level - 1) < merge_done_at_level || merge_done_at_level < 0)
            merge_done_at_level = level - 1;

        if (level - 1 == 0) {
            break;
        }

        if (par->num_keys < ZTREE_INTERNAL_MIN) {
            atomic_fetch_add_explicit(&t->stat_delete_cascades, 1,
                                      memory_order_relaxed);
            level--;
            continue;
        }
        break;
    }

    if (!root_collapsed && merge_done_at_level < 0) {
        delete_path_frame *lf = &path[depth - 1];
        flush_page_immediate(t, &lf->page, lf->zone_id,
                             ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed,
                             &prop.left_zone, &prop.left_slot);
        prop.left_id = lf->page.node_id;
        merge_done_at_level = depth - 1;
    }

    if (!root_collapsed) {
        int start_level = merge_done_at_level - 1;
        for (int lvl = start_level; lvl >= 0 && prop.left_zone_changed; lvl--) {
            delete_path_frame *pf = &path[lvl];
            ztree_page *par = &pf->page;
            uint32_t cidx = child_pos_for_id(par, prop.left_id);
            if (cidx == UINT32_MAX) {
                cidx = child_pos_for_key(par, key);
                ztree_node_id_t expected = (cidx == RIGHTMOST_IDX)
                                                ? par->ptr_node_id
                                                : par->internal[cidx].child_node_id;
                if (expected != prop.left_id) {
                    delete_release_path_locks(t, path, depth);
                    return DEL_RACE;
                }
            }
            if (cidx == RIGHTMOST_IDX) {
                par->ptr_zone_id = prop.left_zone;
            } else {
                par->internal[cidx].child_zone_id = prop.left_zone;
            }
            flush_page_immediate(t, par, pf->zone_id, ZTREE_INVALID_ZONE_ID,
                                 &prop.left_zone_changed,
                                 &prop.left_zone, &prop.left_slot);
            prop.left_id = par->node_id;
            if (prop.left_zone_changed)
                atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1,
                                          memory_order_relaxed);
        }
    }

    delete_release_path_locks(t, path, depth);

    if (root_collapsed) {
        *out_root_nid = collapsed_root_nid;
        *out_root_zone = collapsed_root_zone;
        *out_root_slot = collapsed_root_slot;
        *out_root_changed = 1;
    } else if (prop.left_id == root_nid) {
        *out_root_nid = root_nid;
        *out_root_zone = prop.left_zone;
        *out_root_slot = prop.left_slot;
        *out_root_changed = 1;
    } else {
        *out_root_nid = root_nid;
        *out_root_zone = root_zone;
        *out_root_slot = root_slot;
        *out_root_changed = 0;
    }
    return DEL_OK;
}

int cow_delete(cow_tree *t, int64_t key)
{
    for (uint32_t retry = 0;; retry++) {
        if (retry >= 1000000U) {
            fprintf(stderr, "cow_delete: excessive retries (key=%ld)\n", (long)key);
            exit(EXIT_FAILURE);
        }
        if (retry > 0 && (retry & 0xFU) == 0) sched_yield();

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot;
        uint64_t seq_snapshot;
        for (;;) {
            uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 & 1ULL) continue;
            root_nid = atomic_load_explicit(&t->volatile_sb.root_node_id,
                                            memory_order_acquire);
            root_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id,
                                             memory_order_acquire);
            root_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id,
                                             memory_order_acquire);
            uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 == s2 && (s2 & 1ULL) == 0) {
                seq_snapshot = s2;
                break;
            }
        }
        if (root_nid == ZTREE_INVALID_NODE_ID) return 0;

        ztree_node_id_t out_nid = root_nid;
        uint32_t out_zone = root_zone;
        uint32_t out_slot = root_slot;
        int root_changed = 0;

        int res = try_optimistic_delete(t, key, root_nid, root_zone, root_slot,
                                        seq_snapshot,
                                        &out_nid, &out_zone, &out_slot,
                                        &root_changed);
        if (res == DEL_NOT_FOUND) return 0;
        if (res == DEL_RACE) continue;
        if (res == DEL_NEEDS_MERGE) {
            res = try_pessimistic_delete(t, key, root_nid, root_zone, root_slot,
                                         &out_nid, &out_zone, &out_slot,
                                         &root_changed);
            if (res == DEL_NOT_FOUND) return 0;
            if (res == DEL_RACE) continue;
        }

        if (root_changed &&
            !(out_nid == root_nid && out_zone == root_zone && out_slot == root_slot))
        {
            if (!try_publish_root_if_unchanged(t, seq_snapshot,
                                               out_nid, out_zone, out_slot))
                continue;
        }
        atomic_fetch_add_explicit(&t->stat_deletes, 1, memory_order_relaxed);
        return 1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cow_open / cow_insert / cow_close
 * ═══════════════════════════════════════════════════════════════════════════ */

cow_tree *cow_open(const char *path)
{
    fprintf(stderr, "[ctree_ilayer] opening %s  cache_sets=%d ways=%d\n",
            path, ZTREE_CACHE_NUM_SETS, ZTREE_CACHE_WAYS);

    ztree_t *t = calloc(1, sizeof *t);
    if (!t)
    {
        perror("cow_open calloc");
        return NULL;
    }

    if (pthread_mutex_init(&t->sb_lock, NULL) != 0)
    {
        perror("cow_open sb_lock");
        free(t);
        return NULL;
    }

    t->fd = zbd_open(path, O_RDWR, &t->info);
    if (t->fd < 0)
    {
        perror("zbd_open");
        free(t);
        return NULL;
    }

    if (nvme_get_nsid(t->fd, &t->nsid) != 0)
    {
        perror("nvme_get_nsid");
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    t->direct_fd = open(path, O_RDWR | O_DIRECT);

    /* CNS device is REQUIRED — internal nodes have no ZNS fallback.
     * Default: buffered I/O (page cache absorbs hot-LBA rewrites).
     * Override with env CNS_ODIRECT=1 for O_DIRECT (no kernel cache). */
    {
        const char *od = getenv("CNS_ODIRECT");
        g_cns_odirect = (od && atoi(od) != 0) ? 1 : 0;
    }
    int cns_flags = O_RDWR | O_CREAT | (g_cns_odirect ? O_DIRECT : 0);
    for (int k = 0; k < CTREE_CNS_SHARDS; k++)
    {
        char path_k[256];
        snprintf(path_k, sizeof path_k, CTREE_CNS_FILE_FMT, k);
        int fd = open(path_k, cns_flags, 0644);
        if (fd < 0)
        {
            fprintf(stderr,
                    "[ctree_ilayer] FATAL: cannot open CNS file %s: %s\n"
                    "  Ensure F2FS is mounted on %s:\n"
                    "    sudo mkfs.f2fs -f /dev/nvme3n1 && sudo mount -t f2fs /dev/nvme3n1 %s\n",
                    path_k, strerror(errno), CTREE_CNS_DIR, CTREE_CNS_DIR);
            for (int j = 0; j < k; j++) close(t->cns_fd_shard[j]);
            if (t->direct_fd >= 0) close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            exit(EXIT_FAILURE);
        }
        if (ftruncate(fd, 0) != 0)
            fprintf(stderr, "[ctree_ilayer] WARNING: ftruncate(0) on %s: %s\n",
                    path_k, strerror(errno));
        t->cns_fd_shard[k] = fd;
    }
    t->cns_fd = t->cns_fd_shard[0];  /* alias for validity checks */
    fprintf(stderr,
            "[ctree_ilayer] CNS mode: %s  (%d shards in %s)\n",
            g_cns_odirect ? "O_DIRECT (page cache bypassed)" : "buffered I/O",
            CTREE_CNS_SHARDS, CTREE_CNS_DIR);

    /* No CNS bitmap in this variant — location is determined by pg->is_leaf. */
    t->cns_bitmap = NULL;
    t->cns_bitmap_bytes = 0;

    /* Trace: per-time-step ZNS leaf vs CNS internal valid page counts.
     * Columns match plot_zns_cns_trace.py schema. */
    {
        const char *trace_path = getenv("CTREE_DYNAMIC_TRACE_PATH");
        if (!trace_path || !*trace_path)
            trace_path = "/tmp/ctree_ilayer_trace.csv";
        t->trace_fp = fopen(trace_path, "w");
        if (t->trace_fp)
            fprintf(stderr, "[ctree_ilayer] trace -> %s\n", trace_path);
    }
    t->trace_start_ns = monotonic_ns();
    if (t->trace_fp)
        fprintf(t->trace_fp,
                "time_sec,zns_current,cns_current,appends,cns_writes,height,cns_phys_bytes,zns_phys_bytes\n");
    atomic_store_explicit(&t->stat_cns_current, 0, memory_order_relaxed);
    atomic_store_explicit(&t->stat_cns_writes, 0, memory_order_relaxed);
    atomic_store_explicit(&t->tree_height, 1, memory_order_relaxed);

    t->zones = calloc(t->info.nr_zones, sizeof *t->zones);
    t->zone_wp_bytes = calloc(t->info.nr_zones, sizeof *t->zone_wp_bytes);
    t->zone_full = calloc(t->info.nr_zones, sizeof *t->zone_full);
    t->zone_valid_leaves = calloc(t->info.nr_zones, sizeof *t->zone_valid_leaves);
    if (!t->zones || !t->zone_wp_bytes || !t->zone_full || !t->zone_valid_leaves)
    {
        perror("calloc zones");
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        free(t->zone_valid_leaves);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    unsigned int nr = t->info.nr_zones;
    if (zbd_report_zones(t->fd, 0, 0, ZBD_RO_ALL, t->zones, &nr) != 0)
    {
        perror("zbd_report_zones");
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    for (uint32_t z = 0; z < t->info.nr_zones; z++)
    {
        atomic_store_explicit(&t->zone_wp_bytes[z], t->zones[z].wp, memory_order_relaxed);
        atomic_store_explicit(&t->zone_full[z],
                              (t->zones[z].cond == ZBD_ZONE_COND_FULL) ? 1 : 0,
                              memory_order_relaxed);
    }

    cache_init(t);

    uint64_t zone_pages = (t->info.nr_zones > 0)
                              ? (t->zones[0].capacity / ZTREE_PAGE_SIZE)
                              : 65536ULL;
    size_t tracker_cap = (size_t)zone_pages * 4ULL;
    if (tracker_cap < 4096) tracker_cap = 4096;
    size_t zones_cap = (size_t)t->info.nr_zones * 2ULL;
    if (zones_cap < 256) zones_cap = 256;
    nlt_init(&t->nlt, zones_cap, tracker_cap);

    t->node_latches = calloc(ZTREE_NODE_LATCH_BUCKETS, sizeof(*t->node_latches));
    if (!t->node_latches)
    {
        perror("cow_open node_latches");
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }
    for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
    {
        if (pthread_rwlock_init(&t->node_latches[i], NULL) != 0)
        {
            perror("cow_open node_latches init");
            for (size_t j = 0; j < i; j++)
                pthread_rwlock_destroy(&t->node_latches[j]);
            free(t->node_latches);
            cache_destroy(t);
            nlt_destroy(&t->nlt);
            free(t->zones);
            free(t->zone_wp_bytes);
            free(t->zone_full);
            close(t->cns_fd);
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    t->zone_write_locks = calloc(t->info.nr_zones, sizeof(*t->zone_write_locks));
    if (!t->zone_write_locks)
    {
        perror("cow_open zone_write_locks");
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }
    for (uint32_t z = 0; z < t->info.nr_zones; z++)
    {
        if (pthread_mutex_init(&t->zone_write_locks[z], NULL) != 0)
        {
            perror("cow_open zone_write_locks init");
            for (uint32_t j = 0; j < z; j++)
                pthread_mutex_destroy(&t->zone_write_locks[j]);
            free(t->zone_write_locks);
            for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
                pthread_rwlock_destroy(&t->node_latches[i]);
            free(t->node_latches);
            cache_destroy(t);
            nlt_destroy(&t->nlt);
            free(t->zones);
            free(t->zone_wp_bytes);
            free(t->zone_full);
            close(t->cns_fd);
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    /* Zone layout: 0-1 RLayer (ZNS), 2..N-1 LLayer (ZNS).  ILayer pool is
     * dormant — internals go to CNS, allocator never invoked for IZ. */
    uint32_t ilayer_pool_base  = 2U;   /* dormant; overlaps hot pool start */
    uint32_t ilayer_pool_size  = 1U;   /* allocator clamps to >=1, never used */
    uint32_t ilayer_init       = 1U;

    uint32_t llayer_pool_base  = 2U;   /* RLayer ends at zone 1 */
    uint32_t llayer_pool_total = (t->info.nr_zones > llayer_pool_base)
                                     ? (t->info.nr_zones - llayer_pool_base) : 1;

    uint32_t hot_pool_size  = (llayer_pool_total * 80U) / 100U;
    uint32_t cold_pool_size = llayer_pool_total - hot_pool_size;
    if (hot_pool_size  < ZTREE_LZGROUP_HOT_INIT)  hot_pool_size  = ZTREE_LZGROUP_HOT_INIT;
    if (cold_pool_size < ZTREE_LZGROUP_COLD_INIT) cold_pool_size = ZTREE_LZGROUP_COLD_INIT;

    uint32_t hot_pool_base  = llayer_pool_base;
    uint32_t cold_pool_base = llayer_pool_base + hot_pool_size;

    zone_alloc_init(&t->za,
                    ilayer_pool_base, ilayer_pool_size, ilayer_init,
                    hot_pool_base,  hot_pool_size,  ZTREE_LZGROUP_HOT_INIT,
                    cold_pool_base, cold_pool_size, ZTREE_LZGROUP_COLD_INIT,
                    t->zone_full, t->zone_wp_bytes, t->zones, t->info.nr_zones,
                    t->fd, (uint64_t)t->info.zone_size,
                    t->zone_write_locks);

    /* Active-zone admission (finish-then-release); leaf blocks at cap. */
    t->za.admission_enabled = 1;
    t->za.active_cap = 13;
    atomic_store_explicit(&t->za.active_zones, 0, memory_order_relaxed);

    fprintf(stderr,
            "[ctree_ilayer] internal nodes → CNS %s (slot = node_id)\n",
            CTREE_CNS_DIR);
    fprintf(stderr,
            "[ctree_ilayer] LLayer hot-pool [%u, %u)  init_group=%u"
            "  cold-pool [%u, %u)  init_group=%u\n",
            hot_pool_base,  hot_pool_base  + hot_pool_size,  ZTREE_LZGROUP_HOT_INIT,
            cold_pool_base, cold_pool_base + cold_pool_size, ZTREE_LZGROUP_COLD_INIT);

    load_superblock(t);

    atomic_store_explicit(&t->dirty_sb, false, memory_order_relaxed);
    atomic_store_explicit(&t->stop_flusher, false, memory_order_relaxed);
    atomic_store_explicit(&t->txg_next, 0, memory_order_relaxed);
    atomic_store_explicit(&t->txg_synced, 0, memory_order_relaxed);

    if (pthread_create(&t->flusher_tid, NULL, sb_flusher_thread, t) != 0)
    {
        perror("pthread_create sb_flusher_thread");
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        zone_alloc_destroy(&t->za);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        close(t->cns_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    /* ZNS GC background thread (env CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS, default off;
     * actual work also requires CTREE_DYNAMIC_ZNS_GC=1). */
    {
        const char *env = getenv("CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS");
        long ms = env ? atol(env) : 0;
        if (ms < 0) ms = 0;
        g_zns_gc_interval_ms = (unsigned)ms;
    }
    if (g_zns_gc_interval_ms > 0) {
        atomic_store_explicit(&g_zns_gc_stop, false, memory_order_relaxed);
        atomic_store_explicit(&g_zns_gc_running, true, memory_order_release);
        if (pthread_create(&g_zns_gc_tid, NULL, ilayer_zns_gc_thread, t) != 0) {
            perror("pthread_create ilayer_zns_gc_thread");
            atomic_store_explicit(&g_zns_gc_running, false, memory_order_release);
        } else {
            fprintf(stderr,
                    "[ctree_ilayer] ZNS GC thread enabled: interval=%ums "
                    "(needs CTREE_DYNAMIC_ZNS_GC=1)\n",
                    g_zns_gc_interval_ms);
        }
    }

    return t;
}

void cow_insert(cow_tree *t, int64_t key, const char *value)
{
    do_single_insert(t, key, value);
}

/* ZNS GC: trywrlock migrate + paper Algorithm 2 line 12 parent rewrite. */

static int gc_maint_threads(void) {
    const char *e = getenv("CTREE_DYNAMIC_MAINT_THREADS");
    int n = e ? atoi(e) : 8;
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return n;
}

/* Read root snapshot via seqlock.  Returns even seq_no on success. */
static uint64_t gc_root_snapshot(ztree_t *t,
                                 ztree_node_id_t *out_nid,
                                 uint32_t *out_zone,
                                 uint32_t *out_slot) {
    for (;;) {
        uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 & 1ULL) continue;
        *out_nid  = atomic_load_explicit(&t->volatile_sb.root_node_id, memory_order_acquire);
        *out_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id, memory_order_acquire);
        *out_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id, memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 == s2 && (s2 & 1ULL) == 0) return s2;
    }
}

/* Descend from root using `key`, find parent of target_nid, rewrite its
 * child_zone_id to NLT-current.  Parent is CNS — no further cascade. */
static int gc_cascade_parent(ztree_t *t,
                             ztree_node_id_t target_nid,
                             int64_t key) {
    ztree_node_id_t root_nid;
    uint32_t root_zone, root_slot;
    uint64_t seq_snapshot = gc_root_snapshot(t, &root_nid, &root_zone, &root_slot);
    if (root_nid == ZTREE_INVALID_NODE_ID) return 0;

    nlt_location_t want_q = { .zone_id = ZTREE_INVALID_ZONE_ID,
                              .node_id = target_nid,
                              .slot_id = ZTREE_INVALID_SLOT_ID };
    nlt_location_t want;
    if (!nlt_lookup(&t->nlt, &want_q, &want)) return 0;

    if (root_nid == target_nid) {
        if (root_zone == want.zone_id && root_slot == want.slot_id) return 1;
        return try_publish_root_if_unchanged(t, seq_snapshot,
                                             target_nid, want.zone_id, want.slot_id);
    }

    insert_path_frame path[MAX_HEIGHT];
    int depth = 0;
    ztree_node_id_t cur_id   = root_nid;
    uint32_t        cur_zone = root_zone;

    while (1) {
        if (depth >= MAX_HEIGHT) return 0;
        insert_path_frame *f = &path[depth++];
        f->node_id = cur_id;
        if (!load_latest_node(t, cur_zone, cur_id,
                              &f->zone_id, &f->slot_id, &f->page))
            return 0;
        if (f->page.is_leaf) return 0;  /* target not reachable via key */
        uint32_t cidx = child_pos_for_key(&f->page, key);
        ztree_node_id_t child_nid;
        uint32_t        child_zone;
        if (cidx == RIGHTMOST_IDX) {
            child_nid  = f->page.ptr_node_id;
            child_zone = f->page.ptr_zone_id;
        } else {
            child_nid  = f->page.internal[cidx].child_node_id;
            child_zone = f->page.internal[cidx].child_zone_id;
        }
        if (child_nid == target_nid) break;
        cur_id   = child_nid;
        cur_zone = child_zone;
    }

    insert_path_frame *pf = &path[depth - 1];
    if (!node_trywrlock(t, pf->node_id)) return 0;
    if (!load_latest_node(t, pf->zone_id, pf->node_id,
                          &pf->zone_id, &pf->slot_id, &pf->page)) {
        node_unlock(t, pf->node_id); return 0;
    }
    uint32_t cidx = child_pos_for_id(&pf->page, target_nid);
    if (cidx == UINT32_MAX) {
        node_unlock(t, pf->node_id); return 0;
    }
    uint32_t cur_child_zone = (cidx == RIGHTMOST_IDX)
                                  ? pf->page.ptr_zone_id
                                  : pf->page.internal[cidx].child_zone_id;
    if (cur_child_zone == want.zone_id) {
        node_unlock(t, pf->node_id); return 1;
    }
    if (cidx == RIGHTMOST_IDX)
        pf->page.ptr_zone_id = want.zone_id;
    else
        pf->page.internal[cidx].child_zone_id = want.zone_id;

    propagate_state prop;
    memset(&prop, 0, sizeof prop);
    flush_page_immediate(t, &pf->page, pf->zone_id, ZTREE_INVALID_ZONE_ID,
                         &prop.left_zone_changed, &prop.left_zone, &prop.left_slot);
    node_unlock(t, pf->node_id);
    return 1;
}

static int zns_gc_migrate_leaf(cow_tree *t, uint32_t victim,
                               ztree_node_id_t nid, uint32_t slot) {
    if (!node_trywrlock(t, nid)) return 0;  /* skip contended; next cycle */
    nlt_location_t query  = { .zone_id = victim, .node_id = nid,
                              .slot_id = ZTREE_INVALID_SLOT_ID };
    nlt_location_t actual;
    if (!nlt_lookup(&t->nlt, &query, &actual)
        || actual.zone_id != victim || actual.slot_id != slot) {
        node_unlock(t, nid);
        return 0;
    }
    ztree_page p;
    ztree_pagenum_t pn = zone_slot_to_pn(t, victim, slot);
    load_page_by_pn(t, pn, &p);
    if (!p.is_leaf || p.num_keys == 0) { node_unlock(t, nid); return 0; }
    int64_t cascade_key = (int64_t)p.leaf[0].key;

    /* Prefer an open zone (no admission slot); open new only if none has space. */
    uint32_t target = zone_alloc_llayer_existing(&t->za, nid, victim);
    if (target == ZTREE_INVALID_ZONE_ID)
        target = zone_alloc_llayer(&t->za, nid, victim);
    if (target == ZTREE_INVALID_ZONE_ID) { node_unlock(t, nid); return 0; }
    pthread_mutex_lock(&t->zone_write_locks[target]);
    if (atomic_load_explicit(&t->zone_full[target], memory_order_acquire)) {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid); return 0;
    }
    uint64_t zone_end = t->zones[target].start + t->zones[target].capacity;
    uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target], memory_order_relaxed);
    if (wp + ZTREE_PAGE_SIZE > zone_end) {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        atomic_store_explicit(&t->zone_full[target], 1, memory_order_release);
        zone_seal_and_replace(&t->za, target);
        node_unlock(t, nid); return 0;
    }
    if (wp == t->zones[target].start
        && !zone_admission_acquire(&t->za, target)) {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid); return 0;
    }
    atomic_store_explicit(&t->zone_wp_bytes[target], wp + ZTREE_PAGE_SIZE,
                          memory_order_relaxed);
    uint64_t cur_wp = wp;
    uint32_t new_slot = (uint32_t)((cur_wp - t->zones[target].start) / ZTREE_PAGE_SIZE);
    p.zone_id = target; p.slot_id = new_slot;

    _Alignas(ZTREE_PAGE_SIZE) char bounce[ZTREE_PAGE_SIZE];
    memcpy(bounce, &p, ZTREE_PAGE_SIZE);
    int wfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;
    ssize_t pwr = pwrite(wfd, bounce, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE) {
        atomic_fetch_sub_explicit(&t->zone_wp_bytes[target], ZTREE_PAGE_SIZE,
                                  memory_order_relaxed);
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid); return 0;
    }
    pthread_mutex_unlock(&t->zone_write_locks[target]);

    nlt_location_t newloc = { .zone_id = target, .node_id = nid, .slot_id = new_slot };
    nlt_update_migrate(&t->nlt, &newloc, victim);
    zone_valid_leaves_move(t, victim, target);
    node_unlock(t, nid);
    (void)gc_cascade_parent(t, nid, cascade_key);
    return 1;
}

struct zns_gc_ctx { cow_tree *t; uint32_t victim; size_t seen, migrated; };
static void zns_gc_migrate_cb(ztree_node_id_t nid, uint32_t slot, void *vp) {
    struct zns_gc_ctx *c = vp;
    c->seen++;
    c->migrated += (size_t)zns_gc_migrate_leaf(c->t, c->victim, nid, slot);
}

struct zns_victim_result { uint32_t zone, counter_before; size_t seen, migrated; int done; };
static void zns_gc_migrate_victim(cow_tree *t, struct zns_victim_result *r) {
    r->counter_before = atomic_load_explicit(&t->zone_valid_leaves[r->zone],
                                             memory_order_relaxed);
    struct zns_gc_ctx ctx = { .t = t, .victim = r->zone, .seen = 0, .migrated = 0 };
    nlt_zone_for_each(&t->nlt, r->zone, zns_gc_migrate_cb, &ctx);
    r->seen = ctx.seen; r->migrated = ctx.migrated;
}

struct zns_gc_arg { cow_tree *t; struct zns_victim_result *results; int v_start, v_end; };
static void *zns_gc_worker(void *p) {
    struct zns_gc_arg *a = p;
    for (int v = a->v_start; v < a->v_end; v++)
        zns_gc_migrate_victim(a->t, &a->results[v]);
    return NULL;
}

size_t cow_gc_zns(cow_tree *t) {
    if (!t) return 0;
    const char *e = getenv("CTREE_DYNAMIC_ZNS_GC");
    if (!e || e[0] != '1') return 0;
    force_trace_sample(t);
    size_t before = zns_physical_bytes(t);

    uint32_t *victims = malloc(sizeof(uint32_t) * t->info.nr_zones);
    if (!victims) return 0;
    int nvictims = 0;
    for (uint32_t z = ILAYER_LLAYER_BASE; z < t->info.nr_zones; z++) {
        if (!atomic_load_explicit(&t->zone_full[z], memory_order_acquire)) continue;
        uint64_t start = t->zones[z].start;
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[z], memory_order_relaxed);
        if (wp <= start) continue;
        uint64_t used = wp - start;
        uint64_t valid = (uint64_t)atomic_load_explicit(&t->zone_valid_leaves[z],
                                                       memory_order_relaxed)
                         * ZTREE_PAGE_SIZE;
        double sr = (used > valid) ? (1.0 - (double)valid / (double)used) : 0.0;
        if (sr > ZNS_GC_STALE_THRESHOLD) victims[nvictims++] = z;
    }
    if (nvictims == 0) { free(victims); return 0; }

    fprintf(stderr, "[ctree_ilayer] cow_gc_zns: %d victim(s)\n", nvictims);
    struct zns_victim_result *results = malloc(sizeof(*results) * (size_t)nvictims);
    if (!results) { free(victims); return 0; }
    for (int v = 0; v < nvictims; v++)
        results[v] = (struct zns_victim_result){ .zone = victims[v] };

    int N = gc_maint_threads();
    if (N > nvictims) N = nvictims;
    if (N < 1) N = 1;
    pthread_t tids[32]; struct zns_gc_arg args[32];
    int per = (nvictims + N - 1) / N;
    for (int i = 0; i < N; i++) {
        int s = i * per, ee = ((i + 1) * per > nvictims) ? nvictims : (i + 1) * per;
        args[i] = (struct zns_gc_arg){ .t = t, .results = results, .v_start = s, .v_end = ee };
        pthread_create(&tids[i], NULL, zns_gc_worker, &args[i]);
    }
    for (int i = 0; i < N; i++) pthread_join(tids[i], NULL);

    size_t total_migrated = 0, zones_reset = 0;
    for (int v = 0; v < nvictims; v++)
        total_migrated += results[v].migrated;

    /* Reset drained victims; re-migrate stragglers (transient trylock holds)
     * over a few short passes. */
    for (int pass = 0; pass <= 3; pass++) {
        int pending = 0;
        for (int v = 0; v < nvictims; v++) {
            struct zns_victim_result *r = &results[v];
            if (r->done) continue;
            uint32_t remaining = atomic_load_explicit(&t->zone_valid_leaves[r->zone],
                                                      memory_order_acquire);
            if (remaining == 0) {
                off_t zstart = (off_t)t->zones[r->zone].start;
                if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
                    continue;
                atomic_store_explicit(&t->zone_wp_bytes[r->zone],
                                      t->zones[r->zone].start, memory_order_release);
                zone_admission_release_zone(&t->za, r->zone);
                atomic_store_explicit(&t->zone_full[r->zone], 0, memory_order_release);
                nlt_set_zone_sealed(&t->nlt, r->zone, false);
                r->done = 1; zones_reset++;
            } else {
                pending = 1;
            }
        }
        if (!pending || pass == 3) break;
        usleep(20000);
        for (int v = 0; v < nvictims; v++) {
            struct zns_victim_result *r = &results[v];
            if (r->done) continue;
            zns_gc_migrate_victim(t, r);
            total_migrated += r->migrated;
        }
    }
    free(results); free(victims);
    size_t after = zns_physical_bytes(t);
    force_trace_sample(t);
    fprintf(stderr,
            "[ctree_ilayer] cow_gc_zns: migrated=%zu  reset=%zu  "
            "zns_phys: %zu → %zu KB  (freed %zu KB)\n",
            total_migrated, zones_reset, before/1024, after/1024,
            (before > after) ? (before - after)/1024 : 0);
    return (before > after) ? (before - after) : 0;
}

static void *ilayer_zns_gc_thread(void *arg) {
    cow_tree *t = (cow_tree *)arg;
    while (!atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire)) {
        unsigned slept = 0;
        while (slept < g_zns_gc_interval_ms
            && !atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire)) {
            unsigned step = (g_zns_gc_interval_ms - slept > 50)
                                ? 50 : (g_zns_gc_interval_ms - slept);
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step * 1000000L };
            nanosleep(&ts, NULL);
            slept += step;
        }
        if (atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire)) break;
        cow_phase_mark(t, "begin:zns_gc_periodic");
        cow_gc_zns(t);
        cow_phase_mark(t, "end:zns_gc_periodic");
    }
    return NULL;
}

void cow_close(cow_tree *t)
{
    if (!t)
        return;

    if (atomic_load_explicit(&g_zns_gc_running, memory_order_acquire)) {
        atomic_store_explicit(&g_zns_gc_stop, true, memory_order_release);
        pthread_join(g_zns_gc_tid, NULL);
        atomic_store_explicit(&g_zns_gc_running, false, memory_order_release);
    }

    atomic_store_explicit(&t->stop_flusher, true, memory_order_release);
    pthread_join(t->flusher_tid, NULL);

    uint64_t inserts = atomic_load_explicit(&t->stat_inserts, memory_order_relaxed);
    uint64_t deletes = atomic_load_explicit(&t->stat_deletes, memory_order_relaxed);
    uint64_t del_merges = atomic_load_explicit(&t->stat_delete_merges, memory_order_relaxed);
    uint64_t del_cascades = atomic_load_explicit(&t->stat_delete_cascades, memory_order_relaxed);
    uint64_t del_root_collapses = atomic_load_explicit(&t->stat_delete_root_collapses, memory_order_relaxed);
    uint64_t ch = atomic_load_explicit(&t->stat_cache_hit, memory_order_relaxed);
    uint64_t cm = atomic_load_explicit(&t->stat_cache_miss, memory_order_relaxed);
    uint64_t appends = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    uint64_t nlt_only = atomic_load_explicit(&t->stat_nlt_only_updates, memory_order_relaxed);
    uint64_t zone_chg = atomic_load_explicit(&t->stat_zone_changes, memory_order_relaxed);
    uint64_t par_rew = atomic_load_explicit(&t->stat_parent_rewrites, memory_order_relaxed);
    uint64_t fl_sum = atomic_load_explicit(&t->stat_flush_ns_sum, memory_order_relaxed);
    uint64_t fl_samp = atomic_load_explicit(&t->stat_flush_ns_samples, memory_order_relaxed);
    uint64_t cns_writes = atomic_load_explicit(&t->stat_cns_writes, memory_order_relaxed);

    fprintf(stderr,
            "\n[ctree_ilayer profile]\n"
            "  inserts        = %llu\n"
            "  deletes        = %llu  (merges=%llu cascades=%llu root_collapses=%llu)\n"
            "  cache_hit      = %llu  miss = %llu  hit_rate = %.1f%%\n"
            "  page_appends   = %llu\n"
            "  internal_writes_to_cns = %llu  (%.1f%% of page_appends)\n"
            "  leaf two-stage tracking:\n"
            "    nlt_only_updates  = %llu  (parent skipped, same zone OR internal CoW)\n"
            "    zone_changes      = %llu  (leaf moved to new zone)\n"
            "    parent_rewrites   = %llu\n"
            "  avg_flush_us   = %.1f\n",
            (unsigned long long)inserts,
            (unsigned long long)deletes,
            (unsigned long long)del_merges,
            (unsigned long long)del_cascades,
            (unsigned long long)del_root_collapses,
            (unsigned long long)ch,
            (unsigned long long)cm,
            (ch + cm > 0) ? 100.0 * (double)ch / (double)(ch + cm) : 0.0,
            (unsigned long long)appends,
            (unsigned long long)cns_writes,
            (appends > 0) ? 100.0 * (double)cns_writes / (double)appends : 0.0,
            (unsigned long long)nlt_only,
            (unsigned long long)zone_chg,
            (unsigned long long)par_rew,
            (fl_samp > 0) ? (double)fl_sum / (double)fl_samp / 1000.0 : 0.0);

    uint64_t nlt_wait  = atomic_load_explicit(&t->nlt.prof_wait_ns_sum,   memory_order_relaxed);
    uint64_t nlt_hold  = atomic_load_explicit(&t->nlt.prof_hold_ns_sum,   memory_order_relaxed);
    uint64_t nlt_cnt   = atomic_load_explicit(&t->nlt.prof_acquire_count, memory_order_relaxed);

    uint64_t iz_wait   = atomic_load_explicit(&t->prof_zwl_iz_wait_ns_sum,   memory_order_relaxed);
    uint64_t iz_hold   = atomic_load_explicit(&t->prof_zwl_iz_hold_ns_sum,   memory_order_relaxed);
    uint64_t iz_cnt    = atomic_load_explicit(&t->prof_zwl_iz_acquire_count, memory_order_relaxed);

    uint64_t hot_wait  = atomic_load_explicit(&t->prof_zwl_hot_wait_ns_sum,   memory_order_relaxed);
    uint64_t hot_hold  = atomic_load_explicit(&t->prof_zwl_hot_hold_ns_sum,   memory_order_relaxed);
    uint64_t hot_cnt   = atomic_load_explicit(&t->prof_zwl_hot_acquire_count, memory_order_relaxed);

    uint64_t cold_wait = atomic_load_explicit(&t->prof_zwl_cold_wait_ns_sum,   memory_order_relaxed);
    uint64_t cold_hold = atomic_load_explicit(&t->prof_zwl_cold_hold_ns_sum,   memory_order_relaxed);
    uint64_t cold_cnt  = atomic_load_explicit(&t->prof_zwl_cold_acquire_count, memory_order_relaxed);

    uint64_t zwl_wait  = iz_wait + hot_wait + cold_wait;
    uint64_t zwl_hold  = iz_hold + hot_hold + cold_hold;
    uint64_t zwl_cnt   = iz_cnt  + hot_cnt  + cold_cnt;

    uint64_t nlrd_wait = atomic_load_explicit(&t->prof_nl_rd_wait_ns_sum,   memory_order_relaxed);
    uint64_t nlrd_cnt  = atomic_load_explicit(&t->prof_nl_rd_acquire_count, memory_order_relaxed);

    uint64_t nlwr_wait = atomic_load_explicit(&t->prof_nl_wr_wait_ns_sum,   memory_order_relaxed);
    uint64_t nlwr_cnt  = atomic_load_explicit(&t->prof_nl_wr_acquire_count, memory_order_relaxed);

#define _AVG_US(sum, cnt) ((cnt) > 0 ? (double)(sum) / (double)(cnt) / 1000.0 : 0.0)
#define _MS(ns)           ((double)(ns) / 1.0e6)
#define _PCT(part, total) ((total) > 0 ? 100.0 * (double)(part) / (double)(total) : 0.0)

    fprintf(stderr,
            "\n[ctree_ilayer lock profile]\n"
            "  %-16s %10s %12s %12s %12s\n",
            "", "acquires", "wait", "avg_wait", "avg_hold");
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us %10.2f us\n",
            "NLT alloc lock", (unsigned long long)nlt_cnt,
            _MS(nlt_wait), _AVG_US(nlt_wait, nlt_cnt), _AVG_US(nlt_hold, nlt_cnt));
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us %10.2f us\n",
            "Zone wrlock", (unsigned long long)zwl_cnt,
            _MS(zwl_wait), _AVG_US(zwl_wait, zwl_cnt), _AVG_US(zwl_hold, zwl_cnt));
    fprintf(stderr,
            "    IZ    (%5.1f%%) %10llu %10.1f ms %10.2f us %10.2f us\n",
            _PCT(iz_wait, zwl_wait), (unsigned long long)iz_cnt,
            _MS(iz_wait), _AVG_US(iz_wait, iz_cnt), _AVG_US(iz_hold, iz_cnt));
    fprintf(stderr,
            "    Hot   (%5.1f%%) %10llu %10.1f ms %10.2f us %10.2f us\n",
            _PCT(hot_wait, zwl_wait), (unsigned long long)hot_cnt,
            _MS(hot_wait), _AVG_US(hot_wait, hot_cnt), _AVG_US(hot_hold, hot_cnt));
    fprintf(stderr,
            "    Cold  (%5.1f%%) %10llu %10.1f ms %10.2f us %10.2f us\n",
            _PCT(cold_wait, zwl_wait), (unsigned long long)cold_cnt,
            _MS(cold_wait), _AVG_US(cold_wait, cold_cnt), _AVG_US(cold_hold, cold_cnt));
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us\n",
            "Node rdlock", (unsigned long long)nlrd_cnt,
            _MS(nlrd_wait), _AVG_US(nlrd_wait, nlrd_cnt));
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us\n",
            "Node wrlock", (unsigned long long)nlwr_cnt,
            _MS(nlwr_wait), _AVG_US(nlwr_wait, nlwr_cnt));

#undef _AVG_US
#undef _MS
#undef _PCT

    cache_destroy(t);
    nlt_destroy(&t->nlt);
    zone_alloc_destroy(&t->za);

    if (t->node_latches)
    {
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
    }

    if (t->zone_write_locks)
    {
        for (uint32_t z = 0; z < t->info.nr_zones; z++)
            pthread_mutex_destroy(&t->zone_write_locks[z]);
        free(t->zone_write_locks);
    }

    free(t->zones);
    free(t->zone_wp_bytes);
    free(t->zone_full);
    free(t->zone_valid_leaves);

    zbd_close(t->fd);
    if (t->direct_fd >= 0)
        close(t->direct_fd);
    for (int k = 0; k < CTREE_CNS_SHARDS; k++)
        if (t->cns_fd_shard[k] >= 0)
            close(t->cns_fd_shard[k]);
    if (t->trace_fp)
        fclose(t->trace_fp);

    pthread_mutex_destroy(&t->sb_lock);

    free(t);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Point lookup
 * ═══════════════════════════════════════════════════════════════════════════ */

ztree_record *ztree_find(ztree_t *t, int64_t key)
{
    /* volatile_sb's root_slot goes stale on same-zone CoW; resolve via NLT. */
    ztree_node_id_t root_nid;
    uint32_t root_zone;
    for (;;)
    {
        uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 & 1ULL)
            continue;
        root_nid = atomic_load_explicit(&t->volatile_sb.root_node_id, memory_order_acquire);
        root_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id, memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 == s2 && (s2 & 1ULL) == 0)
            break;
    }

    if (root_nid == ZTREE_INVALID_NODE_ID)
        return NULL;

    ztree_page pg;
    {
        nlt_location_t root_query = {
            .zone_id = root_zone,
            .node_id = root_nid,
            .slot_id = ZTREE_INVALID_SLOT_ID,
        };
        nlt_location_t root_result;
        if (!nlt_lookup(&t->nlt, &root_query, &root_result))
            return NULL;
        if (root_result.zone_id == CTREE_CNS_ZONE_ID)
            load_page_from_cns(t, root_result.slot_id, &pg);
        else
        {
            ztree_pagenum_t pn = zone_slot_to_pn(t, root_result.zone_id, root_result.slot_id);
            load_page_by_pn(t, pn, &pg);
        }
    }

    while (!pg.is_leaf)
    {
        ztree_node_id_t child_nid = pg.ptr_node_id;
        uint32_t child_zone = pg.ptr_zone_id;

        for (uint32_t i = 0; i < pg.num_keys; i++)
        {
            if (key < (int64_t)pg.internal[i].key)
            {
                child_nid = pg.internal[i].child_node_id;
                child_zone = pg.internal[i].child_zone_id;
                break;
            }
        }

        nlt_location_t query = {
            .zone_id = child_zone,
            .node_id = child_nid,
            .slot_id = ZTREE_INVALID_SLOT_ID,
        };
        nlt_location_t result;
        if (nlt_lookup(&t->nlt, &query, &result))
        {
            if (result.zone_id == CTREE_CNS_ZONE_ID)
            {
                load_page_from_cns(t, result.slot_id, &pg);
                continue;
            }
            ztree_pagenum_t pn = zone_slot_to_pn(t, result.zone_id, result.slot_id);
            load_page_by_pn(t, pn, &pg);
        }
        else
        {
            fprintf(stderr, "ztree_find: NLT miss for child node_id=%llu "
                            "(zone_hint=%u)\n",
                    (unsigned long long)child_nid, child_zone);
            return NULL;
        }
    }

    for (uint32_t i = 0; i < pg.num_keys; i++)
    {
        if ((int64_t)pg.leaf[i].key == key)
        {
            ztree_record *r = malloc(sizeof *r);
            if (!r)
                return NULL;
            *r = pg.leaf[i].record;
            return r;
        }
    }
    return NULL;
}
