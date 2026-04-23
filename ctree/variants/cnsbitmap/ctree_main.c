/*
 * ctree_main.c  –  CTree (cnsbitmap variant)
 *
 * Same-zone trylock fail → CNS fallback (no Phase 1b, no parent rewrite).
 * CNS nodes tracked by bitmap; parent rewrite only on zone-full / split.
 *
 gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
      ctree/ctree_nlt.c ctree/ctree_zone.c \
      ctree/variants/cnsbitmap/ctree_main.c \
      ctree/variants/cnsbitmap/bench_main_ctree.c \
      -Ictree/variants/cnsbitmap -Ictree \
      -o build/ctree_bm -lzbd -lnvme -lpthread
 *
 * ────────────────────────────────────────────────────────────────────────── */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ctree_main.h"

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

/* ── CNS bitmap helpers ──────────────────────────────────────────────────── */
static inline void cns_bitmap_set(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (byte < t->cns_bitmap_bytes)
        atomic_fetch_or_explicit(&t->cns_bitmap[byte], bit, memory_order_relaxed);
}

static inline void cns_bitmap_clear(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (byte < t->cns_bitmap_bytes)
        atomic_fetch_and_explicit(&t->cns_bitmap[byte], ~bit, memory_order_relaxed);
}

static inline int cns_bitmap_test(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (byte >= t->cns_bitmap_bytes)
        return 0;
    return (atomic_load_explicit(&t->cns_bitmap[byte], memory_order_relaxed) & bit) != 0;
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

/* Per-zone write mutex (ZWL) profile recorders, broken out by zone group.
 * Routing: ilayer_pool_base..hot_pool_base → IZ, hot..cold → Hot, cold+ → Cold.
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

/* Instrumented node locking: records wait time for contention profiling.
 * No hold-time tracking (rd/wr share node_unlock). */
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

/* Helper: compute byte offset from zone + slot */
static inline uint64_t zone_slot_to_offset(ztree_t *t,
                                           uint32_t zone_id,
                                           uint32_t slot_id)
{
    return t->zones[zone_id].start + (uint64_t)slot_id * ZTREE_PAGE_SIZE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4-Way Set-Associative Global Page Cache
 * (keyed by physical page number; unchanged from cow_gtx_cache_p design)
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
            /* Cache hit: bump LRU counter */
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

    /* Find an empty slot or the LRU victim */
    int victim = 0;
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

    set->ways[victim].valid = 1;
    set->ways[victim].tag = pn;
    set->ways[victim].lru_counter = clock;
    set->ways[victim].data = *src;

    pthread_mutex_unlock(&set->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page I/O (NLT-aware load)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * load_page_by_pn  –  read a page from the global cache or disk.
 * Also populates the NLT lazily: when a page is first read from disk, its
 * embedded (zone_id, slot_id, node_id) fields are registered in the NLT so
 * that subsequent lookups avoid disk I/O.
 */
static void load_page_by_pn(ztree_t *t, ztree_pagenum_t pn, ztree_page *dst)
{
    /* 1. Try the global cache */
    if (cache_lookup(t, pn, dst))
        return;

    /* 2. Cache miss → read from device */
    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    off_t off = (off_t)pn * ZTREE_PAGE_SIZE;
    int rfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;

    if (t->direct_fd >= 0)
    {
        /* O_DIRECT requires aligned buffer */
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

    /* 3. Warm the cache */
    cache_insert(t, pn, dst);

    /* 4. Lazy NLT population: register this node's stable location */
    if (dst->node_id != ZTREE_INVALID_NODE_ID)
    {
        nlt_location_t loc = {
            .zone_id = dst->zone_id,
            .node_id = dst->node_id,
            .slot_id = dst->slot_id,
        };
        nlt_update(&t->nlt, &loc);
    }
}

/*
 * load_page_by_nlt  –  resolve a node's physical location via the NLT,
 * then delegate to load_page_by_pn.
 * Returns 1 on success, 0 if the node_id is not in the NLT.
 */
/*
 * load_page_from_cns  –  read a page from the CNS device.
 * slot_id is the page index on the CNS device (byte offset = slot_id * PAGE_SIZE).
 */
static void load_page_from_cns(ztree_t *t, uint32_t slot_id, ztree_page *dst)
{
    /* CNS cache tag: bit 63 set + slot_id (= node_id) */
    ztree_pagenum_t pn = (ztree_pagenum_t)slot_id | (1ULL << 63);

    /* Try cache first */
    if (cache_lookup(t, pn, dst))
        return;

    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    off_t off = (off_t)slot_id * ZTREE_PAGE_SIZE;

    /* CNS fd is always O_DIRECT → need aligned buffer */
    void *raw;
    if (posix_memalign(&raw, ZTREE_PAGE_SIZE, ZTREE_PAGE_SIZE) != 0)
    {
        perror("load_page_from_cns: posix_memalign");
        exit(EXIT_FAILURE);
    }
    ssize_t n = pread(t->cns_fd, raw, ZTREE_PAGE_SIZE, off);
    if (n != (ssize_t)ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "load_page_from_cns: pread ret=%ld err=%d off=%lu\n",
                (long)n, errno, (unsigned long)off);
        free(raw);
        exit(EXIT_FAILURE);
    }
    memcpy(dst, raw, ZTREE_PAGE_SIZE);
    free(raw);

    /* Warm cache */
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

/*
 * zone_finish_if_full  –  call zbd_finish_zones() on a full zone so the
 * device can reclaim its active-zone slot.
 */
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

/*
 * zone_append_page  –  append one page to zone_id at the current write
 * pointer.  Atomically advances the in-memory WP tracking.
 *
 * On success returns 0 and sets *out_slot_id and *out_pn.
 * Returns -1 if the zone is full after this write.
 */
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
        /* O_DIRECT requires page-aligned buffer; use per-call stack buffer. */
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

    /* Compute slot ID from the offset */
    uint32_t slot = (uint32_t)((cur_wp - t->zones[zone_id].start) / ZTREE_PAGE_SIZE);
    if (out_slot_id)
        *out_slot_id = slot;
    if (out_pn)
        *out_pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);

    /* Mark zone full if write pointer reached capacity */
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
 * Meta zone management (RLayer – superblock ping-pong)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t other_meta_zone(uint32_t z)
{
    return (z == ZTREE_META_ZONE_0) ? ZTREE_META_ZONE_1 : ZTREE_META_ZONE_0;
}

static void activate_meta_zone(ztree_t *t, uint32_t zone_id, uint64_t version)
{
    off_t zstart = (off_t)t->zones[zone_id].start;
    fdatasync(t->fd);
    zbd_finish_zones(t->fd, zstart, (off_t)t->info.zone_size); /* best-effort */
    if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
    {
        perror("activate_meta_zone: zbd_reset_zones");
        exit(EXIT_FAILURE);
    }

    atomic_store_explicit(&t->zone_wp_bytes[zone_id],
                          t->zones[zone_id].start, memory_order_release);
    atomic_store_explicit(&t->zone_full[zone_id], 0, memory_order_release);

    /* Write zone header in slot 0 */
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
    t->meta_wp = 1; /* slot 0 used by zone header */
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
        /* Fresh device – format superblock */
        memset(&t->durable_sb, 0, sizeof t->durable_sb);
        t->durable_sb.root_node_id = ZTREE_INVALID_NODE_ID;
        t->durable_sb.root_zone_id = ZTREE_INVALID_ZONE_ID;
        t->durable_sb.root_slot_id = ZTREE_INVALID_SLOT_ID;
        t->durable_sb.next_node_id = 1; /* IDs start at 1 */
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

    /* Seed the volatile superblock for lock-free readers */
    atomic_store_explicit(&t->volatile_sb.root_node_id,
                          t->durable_sb.root_node_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id,
                          t->durable_sb.root_zone_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id,
                          t->durable_sb.root_slot_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no,
                          t->durable_sb.seq_no * 2, memory_order_release);

    /* Seed the node ID counter from the last persisted value */
    atomic_store_explicit(&t->next_node_id,
                          t->durable_sb.next_node_id, memory_order_relaxed);

    /* Seed the NLT with the root's location so the first traversal works */
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

    /* Rotate meta zone if it is almost full */
    if (t->meta_wp >= t->zones[t->active_meta_zone].capacity / ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "[ctree] meta zone full, rotating\n");
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

/*
 * sb_flusher_thread  –  background thread that periodically flushes the
 * durable superblock without sitting on the critical flush path.
 */
static void *sb_flusher_thread(void *arg)
{
    ztree_t *t = (ztree_t *)arg;

    while (!atomic_load_explicit(&t->stop_flusher, memory_order_acquire))
    {
        usleep(ZTREE_FLUSH_INTERVAL_MS * 1000);

        if (!atomic_exchange_explicit(&t->dirty_sb, false, memory_order_acq_rel))
            continue;

        /* Read the volatile superblock with a seqlock-style read */
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
        /* Persist the node ID counter so recovery gets the right starting point */
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
 * flush_page_immediate  –  write one CoW page to ZNS.
 * prev_zone: current zone (INVALID for new nodes).
 * avoid_zone: skip this zone during allocation (split sibling rule).
 * Tries sticky append first (same zone → no parent rewrite), falls back
 * to dynamic allocation via round-robin (paper §3.2).
 */
static void flush_page_immediate(ztree_t *t,
                                 ztree_page *pg,
                                 uint32_t prev_zone,
                                 uint32_t avoid_zone,
                                 int *out_zone_changed,
                                 uint32_t *out_zone,
                                 uint32_t *out_slot)
{
    uint32_t target_zone;
    uint64_t cur_wp;
    bool     sticky_ok = false;
    bool     cns_path  = false;

    uint64_t zwl_hold_start = 0;

retry_flush:
    sticky_ok = false;
    cns_path  = false;

    /* ── Step 1: try stickiness (trylock on same zone) ─────────────────
     * Skip if prev_zone is CNS or INVALID.
     * If the zone is full, fall through to Step 2 (dynamic alloc).
     * If trylock fails (contention), fall through to Step 3 (CNS).     */
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && prev_zone != CTREE_CNS_ZONE_ID
        && !atomic_load_explicit(&t->zone_full[prev_zone], memory_order_acquire))
    {
        int rc = pthread_mutex_trylock(&t->zone_write_locks[prev_zone]);
        if (rc == 0)
        {
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, prev_zone, 0);
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
                /* Zone full — need dynamic alloc (Step 2). */
                record_zwl_hold(t, prev_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[prev_zone]);
                /* Mark zone full and seal. */
                atomic_store_explicit(&t->zone_full[prev_zone], 1, memory_order_release);
                nlt_set_zone_sealed(&t->nlt, prev_zone, true);
                zone_seal_and_replace(&t->za, prev_zone);
            }
        }
        /* else: trylock failed (contention) — fall through to Step 3 (CNS) */
    }

    /* ── Step 1.5: CNS node trying to return to ZNS (trylock, non-blocking) ─
     * If prev_zone was CNS, try one zone with trylock.
     * Success → ZNS (zone_changed=1, parent rewrite).
     * Failure → back to CNS (Step 3).                                       */
    if (!sticky_ok && !cns_path && prev_zone == CTREE_CNS_ZONE_ID)
    {
        target_zone = pg->is_leaf
                          ? zone_alloc_llayer(&t->za, pg->node_id, avoid_zone)
                          : zone_alloc_ilayer(&t->za, avoid_zone);
        int rc = pthread_mutex_trylock(&t->zone_write_locks[target_zone]);
        if (rc == 0)
        {
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, target_zone, 0);
            zwl_hold_start = lock_t1;

            if (!atomic_load_explicit(&t->zone_full[target_zone], memory_order_acquire))
            {
                uint64_t zone_end = t->zones[target_zone].start
                                    + t->zones[target_zone].capacity;
                uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                                   memory_order_relaxed);
                if (wp + ZTREE_PAGE_SIZE <= zone_end)
                {
                    cur_wp = wp;
                    atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                           wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
                    sticky_ok = true;
                    /* This is a real zone migration (CNS → ZNS) */
                }
            }
            if (!sticky_ok)
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            }
        }
        /* else: trylock failed → fall through to Step 3 (CNS) */
    }

    /* ── Step 2: dynamic allocation (trylock, zone-full / new node) ──────
     * Only reached when prev_zone is INVALID (new node) or zone is full.
     * NOT for CNS nodes — they use Step 1.5 above.
     * Trylock: success → real zone migration (parent rewrite).
     *          failure → fall through to Step 3 (CNS).                  */
    if (!sticky_ok && !cns_path
        && (prev_zone == ZTREE_INVALID_ZONE_ID
            || (prev_zone != CTREE_CNS_ZONE_ID
                && atomic_load_explicit(&t->zone_full[prev_zone], memory_order_acquire))))
    {
        target_zone = pg->is_leaf
                          ? zone_alloc_llayer(&t->za, pg->node_id, avoid_zone)
                          : zone_alloc_ilayer(&t->za, avoid_zone);

        int rc = pthread_mutex_trylock(&t->zone_write_locks[target_zone]);
        if (rc == 0)
        {
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, target_zone, 0);
            zwl_hold_start = lock_t1;

            if (!atomic_load_explicit(&t->zone_full[target_zone],
                                      memory_order_acquire))
            {
                uint64_t zone_end = t->zones[target_zone].start
                                    + t->zones[target_zone].capacity;
                uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                                   memory_order_relaxed);
                if (wp + ZTREE_PAGE_SIZE <= zone_end)
                {
                    cur_wp = wp;
                    atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                           wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
                    sticky_ok = true;
                }
            }
            if (!sticky_ok)
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            }
        }
        /* else: trylock failed → fall through to Step 3 (CNS) */
    }

    /* ── Step 3: CNS fallback (contention on a non-full zone) ─────────
     * No parent rewrite needed — bitmap tracks the CNS location.        */
    if (!sticky_ok)
    {
        if (t->cns_fd >= 0)
        {
            cns_path = true;
            cur_wp = (uint64_t)pg->node_id * ZTREE_PAGE_SIZE;
            target_zone = CTREE_CNS_ZONE_ID;
        }
        else
        {
            /* CNS unavailable — degrade to blocking lock */
            for (;;)
            {
                target_zone = pg->is_leaf
                                  ? zone_alloc_llayer(&t->za, pg->node_id, avoid_zone)
                                  : zone_alloc_ilayer(&t->za, avoid_zone);
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
                uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                                   memory_order_relaxed);
                cur_wp = wp;
                atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                       wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
                sticky_ok = true;
                break;
            }
        }
    }

    /* ── Compute slot_id and cache page number ─────────────────────── */
    uint32_t slot_id;
    ztree_pagenum_t pn;

    if (!cns_path)
    {
        slot_id = (uint32_t)((cur_wp - t->zones[target_zone].start) / ZTREE_PAGE_SIZE);
        pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);
    }
    else
    {
        slot_id = pg->node_id;
        pn = (ztree_pagenum_t)pg->node_id | (1ULL << 63);
    }

    pg->zone_id = target_zone;
    pg->slot_id = slot_id;

    /* ── Write to device ───────────────────────────────────────────── */
    int wfd;
    const void *wbuf;
    _Alignas(ZTREE_PAGE_SIZE) char local_bounce[ZTREE_PAGE_SIZE];

    if (cns_path)
    {
        memcpy(local_bounce, pg, ZTREE_PAGE_SIZE);
        wfd = t->cns_fd;
        wbuf = local_bounce;
    }
    else if (t->direct_fd >= 0)
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

    if (!cns_path)
    {
        uint64_t zs = t->zones[target_zone].start;
        uint64_t ze = zs + t->zones[target_zone].capacity;
        if (cur_wp < zs || cur_wp + ZTREE_PAGE_SIZE > ze)
        {
            fprintf(stderr,
                    "flush_page_immediate: cur_wp out of range "
                    "target_zone=%u prev_zone=%u avoid=%u is_leaf=%u "
                    "node_id=%llu cur_wp=0x%llx "
                    "zone_start=0x%llx zone_end=0x%llx\n",
                    target_zone, prev_zone, avoid_zone, pg->is_leaf,
                    (unsigned long long)pg->node_id,
                    (unsigned long long)cur_wp,
                    (unsigned long long)zs,
                    (unsigned long long)ze);
            exit(EXIT_FAILURE);
        }
    }

    ssize_t pwr = pwrite(wfd, wbuf, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
    {
        int e = errno;
        if (!cns_path && e == EOVERFLOW)
        {
            atomic_fetch_sub_explicit(&t->zone_wp_bytes[target_zone],
                                      ZTREE_PAGE_SIZE, memory_order_relaxed);
            record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
            pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            usleep(500);
            goto retry_flush;
        }
        fprintf(stderr,
                "flush_page_immediate: pwrite at 0x%llx ret=%zd errno=%d (%s) "
                "target_zone=%u prev_zone=%u avoid=%u is_leaf=%u cns=%d\n",
                (unsigned long long)cur_wp, pwr, e, strerror(e),
                target_zone, prev_zone, avoid_zone, pg->is_leaf,
                cns_path ? 1 : 0);
        exit(EXIT_FAILURE);
    }

    /* ── Post-write bookkeeping ────────────────────────────────────── */
    if (!cns_path)
    {
        record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
        pthread_mutex_unlock(&t->zone_write_locks[target_zone]);

        uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
        uint64_t zone_end = t->zones[target_zone].start + t->zones[target_zone].capacity;
        if (new_wp >= zone_end)
        {
            atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
            zone_seal_and_replace(&t->za, target_zone);
        }

        /* Node returned to ZNS — clear bitmap */
        cns_bitmap_clear(t, pg->node_id);
    }
    else
    {
        atomic_fetch_add_explicit(&t->stat_cns_writes, 1, memory_order_relaxed);
        /* Mark node as living on CNS */
        cns_bitmap_set(t, pg->node_id);
    }

    cache_insert(t, pn, pg);

    nlt_location_t loc = {
        .zone_id = target_zone,
        .node_id = pg->node_id,
        .slot_id = slot_id,
    };
    nlt_update(&t->nlt, &loc);

    if (!cns_path)
    {
        uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
        uint64_t zone_bytes_used = new_wp - t->zones[target_zone].start;
        uint64_t seal_threshold = (t->zones[target_zone].capacity * 95ULL) / 100ULL;
        if (zone_bytes_used >= seal_threshold)
        {
            atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
            nlt_set_zone_sealed(&t->nlt, target_zone, true);
            zone_seal_and_replace(&t->za, target_zone);
        }
    }

    if (pg->is_leaf)
    {
        if (prev_zone != ZTREE_INVALID_ZONE_ID
            && prev_zone != CTREE_CNS_ZONE_ID
            && prev_zone != target_zone)
            zone_heat_reset(&t->za, pg->node_id);
        zone_heat_record_write(&t->za, pg->node_id);
    }

    atomic_fetch_add_explicit(&t->stat_page_appends, 1, memory_order_relaxed);

    /* ── zone_changed: CNS spills are NOT zone changes (no parent rewrite).
     * Only real ZNS zone migrations trigger parent rewrite.              */
    if (cns_path)
    {
        /* CNS fallback — parent does NOT need updating */
        if (out_zone_changed)
            *out_zone_changed = 0;
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
    }
    else if (prev_zone != ZTREE_INVALID_ZONE_ID
             && prev_zone != CTREE_CNS_ZONE_ID
             && prev_zone == target_zone)
    {
        /* Same zone (stickiness) — no parent rewrite */
        if (out_zone_changed)
            *out_zone_changed = 0;
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
    }
    else
    {
        /* Real zone migration (zone-full / new node) — parent rewrite needed */
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

/* Finds child link position by node ID (used after reloading parent). */
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

/*
 * try_publish_root_if_unchanged
 *
 * Optimistic publish: commit this insert only if root seq_no is unchanged
 * from the snapshot used to build the overlay.
 */
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
    {
        return 0;
    }

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
            /* Under contention, avoid one worker losing CAS repeatedly. */
            if ((retry & 0xFU) == 0)
                sched_yield();
            if (retry >= 1000000U)
            {
                fprintf(stderr, "cow_insert: excessive publish retries (key=%ld)\n", (long)key);
                exit(EXIT_FAILURE);
            }
        }

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot;
        uint64_t seq_snapshot;

        /* Root snapshot (seqlock read). */
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

        /* Empty tree: create root leaf immediately. */
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

        /* ── Descent: read-crab internals, upgrade rdlock→wrlock at leaf ──
         *
         * At most 1 rdlock held during descent (crabbing).  At leaf:
         * unlock-rd, wrlock.  Non-atomic upgrade requires two post checks:
         *   (a) Reload leaf via NLT — confirm still a leaf (tree may have grown).
         *   (b) If depth≥1, reload parent — confirm it still routes to this leaf
         *       (concurrent split may have moved our key to a sibling).
         *
         * Deadlock-free: descent top-down ≤1 lock, ascent bottom-up ≤1 lock,
         * no nested holds across threads. Bucket collisions: skip relock
         * when parent/child share the same latch.
         */
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
                /* ── Upgrade rdlock → wrlock on the leaf ─────────────── */
                node_unlock(t, cur_id);
                node_wrlock(t, cur_id);

                /* Reload after upgrade; leaf may have been CoW-relocated,
                 * split, or otherwise modified during the unlock/wrlock gap. */
                if (!load_latest_node(t, cur_zone, cur_id,
                                      &f->zone_id, &f->slot_id, &f->page))
                {
                    node_unlock(t, cur_id);
                    goto retry_insert;
                }

                /* Check (a): tree may have grown — restart if no longer a leaf. */
                if (!f->page.is_leaf)
                {
                    node_unlock(t, cur_id);
                    goto retry_insert;
                }

                /* Check (b): confirm parent still routes our key here (split race). */
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

            /* Internal node — read-crab to the next child. */
            uint32_t cidx = child_pos_for_key(&f->page, key);
            f->cidx_from_parent = cidx;

            ztree_node_id_t next_id = (cidx == RIGHTMOST_IDX)
                                          ? f->page.ptr_node_id
                                          : f->page.internal[cidx].child_node_id;

            /* Skip relock if parent/child share the same latch bucket. */
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

        /* Modify leaf in-place (CoW page image in local stack frame). */
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

                /* Inherit parent's heat so sibling isn't misclassified cold. */
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

                /* Sibling first (paper order).
                 * avoid_zone = leaff->zone_id so the new right sibling is
                 * always allocated to a different zone than the left sibling. */
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

        /* Propagate to parents on split OR real zone migration.
         * CNS spills set zone_changed=0 so they never trigger parent rewrite.
         * Only zone-full / new-node migrations set zone_changed=1.          */
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
                    /*
                     * Rarely, parent reload may not match by child node ID.
                     * Fall back to the current key route before retrying.
                     */
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
            /* Root publish is required only when root physically migrated zones. */
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
 * cow_open / cow_insert / cow_close
 * ═══════════════════════════════════════════════════════════════════════════ */

cow_tree *cow_open(const char *path)
{
    fprintf(stderr, "[ctree] opening %s  cache_sets=%d ways=%d\n",
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

    /* ── Open CNS (Conventional Namespace) device for contention fallback ── */
    t->cns_fd = open(CTREE_CNS_DEV_PATH, O_RDWR | O_DIRECT);
    if (t->cns_fd < 0)
    {
        fprintf(stderr, "[ctree] WARNING: cannot open CNS device %s: %s\n"
                        "        CNS fallback disabled — will block on contention.\n",
                CTREE_CNS_DEV_PATH, strerror(errno));
        /* Non-fatal: we can still operate (degrades to ztree behavior). */
    }
    /* Allocate CNS bitmap */
    t->cns_bitmap_bytes = CTREE_CNS_BITMAP_MAX_NODES / 8;
    t->cns_bitmap = calloc(t->cns_bitmap_bytes, sizeof(*t->cns_bitmap));
    if (!t->cns_bitmap)
    {
        fprintf(stderr, "[ctree] WARNING: cannot allocate CNS bitmap (%u bytes)\n",
                t->cns_bitmap_bytes);
    }
    atomic_store_explicit(&t->stat_cns_writes, 0, memory_order_relaxed);

    /* Allocate per-zone arrays */
    t->zones = calloc(t->info.nr_zones, sizeof *t->zones);
    t->zone_wp_bytes = calloc(t->info.nr_zones, sizeof *t->zone_wp_bytes);
    t->zone_full = calloc(t->info.nr_zones, sizeof *t->zone_full);
    if (!t->zones || !t->zone_wp_bytes || !t->zone_full)
    {
        perror("calloc zones");
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
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

    /* Initialise subsystems */
    cache_init(t);

    nlt_init(&t->nlt, 65536); /* initial NLT capacity */

    t->node_latches = calloc(ZTREE_NODE_LATCH_BUCKETS, sizeof(*t->node_latches));
    if (!t->node_latches)
    {
        perror("cow_open node_latches");
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
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
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    /* Per-zone write mutexes: serialise pwrite calls within each zone for ZNS ordering. */
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
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    /* Zone layout: 0-1 RLayer, 2-17 ILayer, 18+ LLayer (80/20 hot/cold). */
    uint32_t ilayer_pool_base  = ZTREE_ILAYER_ZONE_START;   /* 2  */
    uint32_t ilayer_pool_size  = ZTREE_ILAYER_POOL_SIZE;    /* 16 */
    uint32_t ilayer_init       = ZTREE_ILAYER_INIT_COUNT;   /* 4  */

    uint32_t llayer_pool_base  = ZTREE_LLAYER_ZONE_START;   /* 18 */
    uint32_t llayer_pool_total = (t->info.nr_zones > llayer_pool_base)
                                     ? (t->info.nr_zones - llayer_pool_base) : 1;

    /* 80 / 20 hot-cold split of the LLayer pool */
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

    fprintf(stderr,
            "[ctree] ILayer pool [%u, %u)  init_group=%u\n",
            ilayer_pool_base, ilayer_pool_base + ilayer_pool_size, ilayer_init);
    fprintf(stderr,
            "[ctree] LLayer hot-pool [%u, %u)  init_group=%u"
            "  cold-pool [%u, %u)  init_group=%u\n",
            hot_pool_base,  hot_pool_base  + hot_pool_size,  ZTREE_LZGROUP_HOT_INIT,
            cold_pool_base, cold_pool_base + cold_pool_size, ZTREE_LZGROUP_COLD_INIT);

    load_superblock(t);

    /* Initialise atomic flags */
    atomic_store_explicit(&t->dirty_sb, false, memory_order_relaxed);
    atomic_store_explicit(&t->stop_flusher, false, memory_order_relaxed);
    atomic_store_explicit(&t->txg_next, 0, memory_order_relaxed);
    atomic_store_explicit(&t->txg_synced, 0, memory_order_relaxed);

    /* Start background superblock flusher thread. */
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
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    return t;
}

void cow_insert(cow_tree *t, int64_t key, const char *value)
{
    do_single_insert(t, key, value);
}

void cow_close(cow_tree *t)
{
    if (!t)
        return;

    /* Stop the superblock flusher */
    atomic_store_explicit(&t->stop_flusher, true, memory_order_release);
    pthread_join(t->flusher_tid, NULL);

    /* Print profile summary */
    uint64_t inserts = atomic_load_explicit(&t->stat_inserts, memory_order_relaxed);
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
            "\n[ctree profile]\n"
            "  inserts        = %llu\n"
            "  cache_hit      = %llu  miss = %llu  hit_rate = %.1f%%\n"
            "  page_appends   = %llu\n"
            "  two-stage tracking:\n"
            "    nlt_only_updates  = %llu  (parent skipped, same zone)\n"
            "    zone_changes      = %llu  (node moved to new zone)\n"
            "    parent_rewrites   = %llu\n"
            "  avg_flush_us   = %.1f\n",
            (unsigned long long)inserts,
            (unsigned long long)ch,
            (unsigned long long)cm,
            (ch + cm > 0) ? 100.0 * (double)ch / (double)(ch + cm) : 0.0,
            (unsigned long long)appends,
            (unsigned long long)nlt_only,
            (unsigned long long)zone_chg,
            (unsigned long long)par_rew,
            (fl_samp > 0) ? (double)fl_sum / (double)fl_samp / 1000.0 : 0.0);
    fprintf(stderr,
            "  cns_fallback_writes = %llu  (%.1f%% of page_appends)\n",
            (unsigned long long)cns_writes,
            (appends > 0) ? 100.0 * (double)cns_writes / (double)appends : 0.0);

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
            "\n[ctree lock profile]\n"
            "  %-16s %10s %12s %12s %12s\n",
            "", "acquires", "wait", "avg_wait", "avg_hold");
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us %10.2f us\n",
            "NLT wrlock", (unsigned long long)nlt_cnt,
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

    /* Destroy subsystems */
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

    zbd_close(t->fd);
    if (t->direct_fd >= 0)
        close(t->direct_fd);
    if (t->cns_fd >= 0)
        close(t->cns_fd);
    if (t->cns_bitmap)
        free(t->cns_bitmap);

    pthread_mutex_destroy(&t->sb_lock);

    free(t);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Point lookup (optional, not used by insert-only benchmark)
 * ═══════════════════════════════════════════════════════════════════════════ */

ztree_record *ztree_find(ztree_t *t, int64_t key)
{
    /* Read the current root safely using the seqlock volatile superblock */
    ztree_node_id_t root_nid;
    uint32_t root_zone, root_slot;
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
            break;
    }

    if (root_nid == ZTREE_INVALID_NODE_ID)
        return NULL;

    /* Traverse the tree from root to leaf using NLT resolution */
    (void)root_nid; /* identity tracked inside pg.node_id after first load */

    ztree_page pg;
    if (root_zone == CTREE_CNS_ZONE_ID)
        load_page_from_cns(t, root_slot, &pg);
    else
    {
        ztree_pagenum_t pn = zone_slot_to_pn(t, root_zone, root_slot);
        load_page_by_pn(t, pn, &pg);
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

        /* NLT may have a fresher slot than the page field */
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
                            "(zone_hint=%u) – NLT not yet populated?\n",
                    (unsigned long long)child_nid, child_zone);
            return NULL;
        }
    }

    /* Binary search within the leaf */
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
