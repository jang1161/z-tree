# ZTree - CoW B+-Tree for ZNS SSDs

## Project Overview

ZTree is a Copy-on-Write B+-Tree optimized for ZNS (Zoned Namespace) NVMe SSDs. Based on the paper in `/paper/paper.txt`, with a structured implementation summary in `/paper/implementation.md`.

Key innovations:
- **Node Location Table (NLT)**: stable NodeID -> (zone_id, slot_id) mapping, avoiding recursive parent rewrites on same-zone updates
- **3-Layer Architecture**: RLayer (root/meta), ILayer (internal, round-robin), LLayer (leaf, heat-aware 80/20 hot/cold)
- **Two-Stage Tracking**: same-zone re-append only updates NLT slot, not parent node

## Build

```bash
gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
    ztree_nlt.c ztree_zone.c ztree_main.c bench_main_ztree.c \
    -o build/bin/ztree -lzbd -lnvme -lpthread
```

Dependencies: `libzbd`, `libnvme`, `pthread`

## Source Layout

| File | Lines | Role |
|---|---|---|
| `ztree_types.h` | 248 | Shared types, page layout (4KB), constants, B+-tree fan-out |
| `ztree_nlt.h/c` | 125/501 | Node Location Table - lock-free reads, write-lock for updates |
| `ztree_zone.h/c` | 167/269 | Zone allocator - IZGroup round-robin, LZGroup heat-aware hot/cold |
| `ztree_main.h/c` | 86/1835 | Core tree: open/close, insert, find, CoW, latch-crabbing, cache |
| `bench_main_ztree.c` | 204 | Benchmark harness with multi-threaded insert workload |
| `watch_zone.sh` | - | Zone state monitoring script |

## Architecture Constants

- Page/node size: 4KB
- Leaf order: 32 (max 31 entries, 128B each: 8B key + 120B value)
- Internal order: 249 (max 248 entries, 16B each: 8B key + 4B node_id + 4B zone_id)
- Zone layout: Zone 0-1 RLayer (meta ping-pong), Zone 2-17 ILayer pool, Zone 18+ LLayer pool
- Initial IZGroup: 4 zones, Initial LZGroup: 8 zones (6 hot + 2 cold)
- Max active zones budget: 13 (device limit)
- Page cache: 4-way set-associative, 196608 sets

## Implementation Status

### Implemented
- Full CoW insert path (`cow_insert` -> `do_single_insert`) with single-pass latch-crabbing descent
- Node splitting with paper-aligned persistence order (sibling -> parent -> original)
- NLT with lock-free lookup and write-lock protected updates, open-addressing hash
- Zone allocator: ILayer round-robin, LLayer heat-aware hot/cold scheduling
- 95% zone capacity threshold for sealing
- Superblock ping-pong (zone 0/1) with background flusher thread
- 4-way set-associative global page cache
- O_DIRECT support with aligned bounce buffer
- `ztree_find` (SEARCH) with top-down traversal and NLT resolution
- Benchmark harness with multi-threaded insert + progress reporting

### Not Yet Implemented
- UPDATE operation (in-place value modification without split)
- DELETE / merge operations
- Zone GC (garbage collection and valid-node migration)
- Zone rebalancing (periodic hot/cold label redistribution)
- Range scan
- Crash recovery (replay from superblock + NLT rebuild)

## Public API

```c
cow_tree *cow_open(const char *dev_path);       // Open device and init tree
void      cow_insert(cow_tree *t, int64_t key, const char *value);
void      cow_close(cow_tree *t);
ztree_record *ztree_find(ztree_t *t, int64_t key);  // Point lookup
```

## Key Design Details

- **Latch-crabbing**: hashed per-node rwlocks (65536 buckets), top-down write-lock acquisition
- **CoW insert**: single-pass descent, immediate page append + NLT update (no temp IDs or overlay)
- **Two-stage tracking**: same-zone update -> NLT-only update; cross-zone migration -> parent rewrite
- **Split persistence**: sibling first, then parent (with new separator), then original node last
- **Root publication**: lock-free via atomic superblock fields, durable checkpoint by background thread

## Conventions

- C11 with GNU extensions (`_GNU_SOURCE`)
- `_Atomic` for lock-free shared state
- `_Static_assert` for compile-time layout verification
- `ztree_` prefix for types/functions, `cow_` for legacy-compatible API
- `ZTREE_INVALID_*` sentinel values for invalid IDs
