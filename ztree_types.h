/*
 * ztree_types.h  –  Shared type definitions for ZTree
 *
 * CoW B+-Tree for ZNS NVMe with:
 *   1. NLT: NodeID → (zone_id, slot_id) indirection
 *   2. 3-Layer: RLayer (meta), ILayer (internal), LLayer (leaf, heat-aware)
 *   3. Two-Stage Tracking: same-zone updates skip parent rewrite
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
 * Zone layout: zones 0-1 RLayer, 2-17 ILayer, 18+ LLayer (80/20 hot/cold).
 * Active-zone budget (device limit=14): IZ(3)+Hot(8)+Cold(2)+Meta(1)=14.
 * Original paper split (IZ=4, Hot=6, Cold=2) in comments per #define.
 */
#define ZTREE_META_ZONE_0          0U
#define ZTREE_META_ZONE_1          1U
#define ZTREE_ILAYER_ZONE_START    2U   /* first zone of ILayer pool        */
#define ZTREE_ILAYER_POOL_SIZE    16U   /* ILayer pool size (zones 2-17)    */
#define ZTREE_ILAYER_INIT_COUNT    3U   /* initial IZGroup size (paper: 4)  */
#define ZTREE_LLAYER_ZONE_START   18U   /* first zone of LLayer pool        */
#define ZTREE_LZGROUP_INIT_SIZE   10U   /* initial LZGroup total size       */
#define ZTREE_LZGROUP_HOT_INIT     8U   /* initial hot  zones (paper: 6)    */
#define ZTREE_LZGROUP_COLD_INIT    2U   /* initial cold zones (paper: 2)    */

/* Magic numbers for on-disk structures */
#define ZTREE_ZH_MAGIC   0x5A545245455A4E53ULL  /* "ZTREEZNS" */
#define ZTREE_SB_MAGIC   0x5A5452454553420AULL  /* "ZTREESB\n" */
#define ZTREE_ZH_ACTIVE  0x01U

/* ═══════════════════════════════════════════════════════════════════════════
 * B+-Tree fan-out constants
 *
 * 128B header + 3968B body = 4096B page.
 * Leaf: 3968/128=31 entries (ORDER=32). Internal: 3968/16=248 (ORDER=249).
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

/* Internal child pointer: (zone_id, node_id) indirection via NLT,
 * so same-zone child updates skip parent rewrite. */
typedef struct {
    uint64_t        key;            /* separator / fence key           */
    ztree_node_id_t child_node_id;  /* stable child identifier         */
    uint32_t        child_zone_id;  /* zone that currently holds child */
} ztree_internal_entity;  /* exactly 16 bytes */

/* ═══════════════════════════════════════════════════════════════════════════
 * Page layout (4096 B = 128 B header + 3968 B body)
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

/* Superblock entry – appended to active RLayer zone after each TXG. */
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
 * Global page cache (4-way set-associative, LRU eviction)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define ZTREE_CACHE_NUM_SETS   (1024)
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
