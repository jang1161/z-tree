/*
 * ztree_types.h  –  Shared type definitions for ZTree
 *
 * ZTree is a Copy-on-Write B+-Tree optimized for ZNS (Zoned Namespace) NVMe
 * devices.  It extends the baseline COW tree with three new mechanisms:
 *
 *   1. Node Location Table (NLT): maps a stable NodeID to its current
 *      physical (zone_id, slot_id) location, so parent nodes only need to
 *      store (zone_id, node_id) pairs rather than raw LBAs.
 *
 *   2. 3-Layer Architecture: RLayer (metadata/root), ILayer (internal nodes,
 *      round-robin over dedicated zones), LLayer (leaf nodes, heat-aware
 *      80/20 hot/cold zone split).
 *
 *   3. Two-Stage Tracking: when a node is re-appended within the same zone,
 *      only the NLT slot entry is updated – the parent node does NOT need to
 *      be rewritten.  A parent rewrite is triggered only when a child moves
 *      to a different zone or a node split occurs.
 *
 * Compile with:
 *   gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
 *       -Iinclude -I. \
 *       src/Z-tree/ztree_nlt.c src/Z-tree/ztree_zone.c src/Z-tree/ztree_main.c \
 *       -o build/bin/ztree -lzbd -lnvme -lpthread
 */

#pragma once

#include <libnvme.h>
#include <libzbd/zbd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Physical / layout constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ZTREE_PAGE_SIZE         4096U

/*
 * Zone layout on device (paper-aligned):
 *
 *   Zone  0- 1  → RLayer  (meta / superblock ping-pong pair)
 *   Zone  2-17  → ILayer pool  (16 zones available for IZGroup)
 *                  Initial IZGroup = zones 2-5 (4 zones)
 *                  Dynamic expansion attaches zones 6-17 as IZGroup fills
 *   Zone 18+    → LLayer pool  (all remaining zones for LZGroup)
 *                  Initial LZGroup = 8 zones (6 hot + 2 cold)
 *                  80% of pool → hot zones; 20% → cold zones
 *                  Dynamic expansion attaches more zones as LZGroup fills
 *
 * Active-zone budget (device hard limit = 13):
 *   IZGroup initial (4) + LZGroup hot initial (6) + LZGroup cold initial (2) = 12 ≤ 13
 */
#define ZTREE_META_ZONE_0          0U
#define ZTREE_META_ZONE_1          1U
#define ZTREE_ILAYER_ZONE_START    2U   /* first zone of ILayer pool        */
#define ZTREE_ILAYER_POOL_SIZE    16U   /* ILayer pool size (zones 2-17)    */
#define ZTREE_ILAYER_INIT_COUNT    4U   /* initial IZGroup size             */
#define ZTREE_LLAYER_ZONE_START   18U   /* first zone of LLayer pool        */
#define ZTREE_LZGROUP_INIT_SIZE    8U   /* initial LZGroup total size       */
#define ZTREE_LZGROUP_HOT_INIT     6U   /* initial hot  zone count in LZGroup */
#define ZTREE_LZGROUP_COLD_INIT    2U   /* initial cold zone count in LZGroup */

/* Magic numbers for on-disk structures */
#define ZTREE_ZH_MAGIC   0x5A545245455A4E53ULL  /* "ZTREEZNS" */
#define ZTREE_SB_MAGIC   0x5A5452454553420AULL  /* "ZTREESB\n" */
#define ZTREE_ZH_ACTIVE  0x01U

/* ═══════════════════════════════════════════════════════════════════════════
 * B+-Tree fan-out constants
 *
 * Page header is padded to exactly 128 bytes.
 * Remaining 3968 bytes hold the union of leaf / internal entries.
 *
 *   Leaf entry       = key(8) + value(120)           = 128 bytes
 *   Internal entry   = key(8) + node_id(4) + zone_id(4) = 16 bytes
 *
 *   Max leaf entries     = 3968 / 128 = 31  → LEAF_ORDER     = 32
 *   Max internal entries = 3968 / 16  = 248 → INTERNAL_ORDER = 249
 * ═══════════════════════════════════════════════════════════════════════════ */
#define ZTREE_LEAF_ORDER     32U
#define ZTREE_INTERNAL_ORDER 249U

/* ═══════════════════════════════════════════════════════════════════════════
 * Identifier types
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Stable, globally-unique node identifier.  Assigned once at node creation
 * and never changes even as the node migrates between zones / slots.       */
typedef uint32_t ztree_node_id_t;

/* Physical page number: byte_offset / PAGE_SIZE */
typedef uint64_t ztree_pagenum_t;

#define ZTREE_INVALID_NODE_ID  ((ztree_node_id_t)0)
#define ZTREE_INVALID_ZONE_ID  ((uint32_t)0xFFFFFFFFU)
#define ZTREE_INVALID_SLOT_ID  ((uint32_t)0xFFFFFFFFU)
#define ZTREE_INVALID_PGN      ((ztree_pagenum_t)(~0ULL))

/* Temp-ID flag: top bit set = this ID was allocated in the current TXG and
 * has not yet been written to disk.                                         */
static inline int ztree_is_temp_id(ztree_node_id_t id)
{
    return (id & (1U << 31)) != 0;
}
static inline ztree_node_id_t ztree_make_temp_id(uint32_t n)
{
    return (1U << 31) | n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Record / leaf / internal entity types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { char value[120]; } ztree_record;  /* 120 B payload */

typedef struct {
    uint64_t     key;     /* sort key                    */
    ztree_record record;  /* associated value (120 B)    */
} ztree_leaf_entity;      /* exactly 128 bytes           */

/*
 * Internal child pointer.
 *
 * Unlike raw-LBA designs, ZTree stores (child_zone_id, child_node_id) so
 * that a child can be re-appended within the same zone without requiring
 * the parent to be rewritten.  The NLT resolves node_id → current slot.
 */
typedef struct {
    uint64_t        key;            /* separator / fence key           */
    ztree_node_id_t child_node_id;  /* stable child identifier         */
    uint32_t        child_zone_id;  /* zone that currently holds child */
} ztree_internal_entity;  /* exactly 16 bytes */

/* ═══════════════════════════════════════════════════════════════════════════
 * Page layout  (must be exactly ZTREE_PAGE_SIZE = 4096 bytes)
 *
 * Header (128 B):
 *   node_id      4   – stable global ID written on every append
 *   zone_id      4   – zone where this copy resides (cross-check)
 *   slot_id      4   – slot within zone
 *   is_leaf      4
 *   num_keys     4
 *   ptr_zone_id  4   – rightmost-child zone (internal only)
 *   ptr_node_id  4   – rightmost-child node_id (internal only)
 *   _pad0        4
 *   _pad1       96
 *
 * Body (3968 B): union of leaf[31] or internal[248]
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    /* ── header ─────────────────────────────────────────────────── */
    ztree_node_id_t node_id;       /*  4 B – stable identity          */
    uint32_t        zone_id;       /*  4 B – current zone (for NLT)   */
    uint32_t        slot_id;       /*  4 B – current slot (for NLT)   */
    uint32_t        is_leaf;       /*  4 B                            */
    uint32_t        num_keys;      /*  4 B                            */
    uint32_t        ptr_zone_id;   /*  4 B – rightmost child zone     */
    ztree_node_id_t ptr_node_id;   /*  4 B – rightmost child node_id  */
    uint32_t        _pad0;         /*  4 B                            */
    uint8_t         _pad1[96];     /* 96 B – pad header to 128 B      */

    /* ── body (union of leaf / internal entries) ─────────────────── */
    union {
        ztree_leaf_entity     leaf[ZTREE_LEAF_ORDER - 1];        /* 31 × 128 = 3968 B */
        ztree_internal_entity internal[ZTREE_INTERNAL_ORDER - 1];/* 248 × 16 = 3968 B */
    };
} ztree_page;   /* 128 + 3968 = 4096 bytes */

_Static_assert(sizeof(ztree_leaf_entity)     == 128, "leaf entity must be 128 B");
_Static_assert(sizeof(ztree_internal_entity) ==  16, "internal entity must be 16 B");
_Static_assert(sizeof(ztree_page) == ZTREE_PAGE_SIZE, "ztree_page must be PAGE_SIZE");

/* ═══════════════════════════════════════════════════════════════════════════
 * On-disk metadata structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Zone header – written as the first PAGE_SIZE slot of every active zone. */
typedef struct __attribute__((packed)) {
    uint64_t magic;    /* ZTREE_ZH_MAGIC */
    uint8_t  state;    /* ZTREE_ZH_ACTIVE or 0                              */
    uint64_t version;  /* monotonic version counter at zone activation time  */
    uint8_t  _pad[ZTREE_PAGE_SIZE - 8 - 1 - 8];
} ztree_zone_header;   /* 4096 bytes */

/*
 * Superblock entry – appended to the active RLayer zone after each TXG.
 * The root is identified by (root_node_id, root_zone_id) so the NLT can
 * be seeded at startup without a full zone scan.
 */
typedef struct __attribute__((packed)) {
    uint64_t        magic;          /* ZTREE_SB_MAGIC                       */
    uint64_t        seq_no;         /* monotonically increasing              */
    ztree_node_id_t root_node_id;   /* stable ID of tree root                */
    uint32_t        root_zone_id;   /* zone holding the current root         */
    uint32_t        root_slot_id;   /* slot within root_zone_id              */
    ztree_node_id_t next_node_id;   /* next free node ID (for crash recovery) */
    uint32_t        leaf_order;     /* ZTREE_LEAF_ORDER at format time        */
    uint32_t        internal_order; /* ZTREE_INTERNAL_ORDER at format time    */
    uint8_t         _pad[ZTREE_PAGE_SIZE - 8*2 - 4*6];
} ztree_superblock_entry;  /* 4096 bytes */

/* Volatile (in-memory) superblock for lock-free root reads by workers */
typedef struct {
    _Atomic(ztree_node_id_t) root_node_id;
    _Atomic(uint32_t)        root_zone_id;
    _Atomic(uint32_t)        root_slot_id;
    _Atomic(uint64_t)        seq_no;
} ztree_atomic_superblock;

/* ═══════════════════════════════════════════════════════════════════════════
 * Insert request  (same interface as cow_gtx_cache_p)
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct ztree_insert_req {
    int64_t  key;
    char     value[120];
    int      done;
    pthread_mutex_t done_lock;
    pthread_cond_t  done_cv;
    struct ztree_insert_req *next;
} ztree_insert_req;

/* ═══════════════════════════════════════════════════════════════════════════
 * Global page cache  (4-way set-associative, keyed by physical page number)
 *
 * The cache is kept here as a flat array of ztree_cache_set.  Each set has
 * ZTREE_CACHE_WAYS ways protected by a single per-set mutex.  LRU eviction
 * uses a global atomic clock counter.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define ZTREE_CACHE_NUM_SETS   (4096 * 4 * 12)
#define ZTREE_CACHE_WAYS       4

typedef struct {
    uint8_t         valid;
    uint64_t        lru_counter;
    ztree_pagenum_t tag;        /* physical page number as cache key */
    ztree_page      data;
} ztree_cache_way;

typedef struct {
    pthread_mutex_t lock;
    ztree_cache_way ways[ZTREE_CACHE_WAYS];
} ztree_cache_set;
