# ZTree Implementation (Paper-Aligned)

This document is a paper-aligned implementation summary of Section 3 (Design) and Section 3.2 (Tree Operations), with explicit constants, field sizes, and runtime behavior.

## 3.1 Architecture

ZTree is organized into three logical layers:

| Layer | Role | Stored Data |
| --- | --- | --- |
| RLayer | Global entry point | Latest root node version |
| ILayer | Routing layer | Internal nodes (min key + child location) |
| LLayer | Data layer | Leaf nodes with KV entries in ascending key order |

### Layer-wise Zone Layout

ZTree maps each logical layer to independent ZNS zones:

1. RLayer uses a fixed high-priority zone (Zone 0 by default), kept open during runtime.
2. ILayer and LLayer use dynamic allocation across multiple zones to exploit inter-zone parallelism.

### Slot Model and SlotID Definition

ZTree partitions each zone into fixed-size slots.

1. Slot size equals node size (4 KB by default).
2. Each slot receives a unique SlotID, assigned sequentially within a zone.
3. Updated nodes are appended to new slots (out-of-place write), not overwritten in-place.

## 3.1.1 Dynamic Node Allocation

ZTree uses a node allocator with two logical zone groups:

1. IZGroup for internal nodes.
2. LZGroup for leaf nodes.

### Default Group Sizes (Paper)

The paper states these initial defaults (user-customizable):

1. IZGroup: 4 zones.
2. LZGroup: 6 zones. (In my implement, 8 zones for LZGroup)

If a group is exhausted, idle zones are attached dynamically.

1. The newly attached zone is opened.
2. Its write pointer is reset to the beginning.

### IZGroup Scheduling

Internal nodes are distributed round-robin across available IZGroup zones to maximize inter-zone parallelism.

### LZGroup Heat-Aware Scheduling

LZGroup uses hot/cold separation to reduce GC migration overhead.

Per-node heat metadata:

1. Counter: 2-byte field, tracks historical access frequency.
2. Timestamp: 2-byte field, tracks last access time.

Every SEARCH or INSERT updates these two fields.

Default hot/cold split:

1. 80 percent of LZGroup zones are hot zones.
2. 20 percent are cold zones.

Hotness rule in the paper:

1. Nodes whose Counter or Timestamp is in top 50th percentile are treated as hot.
2. Hot nodes are round-robin scheduled across hot zones.
3. Cold nodes are consolidated into fewer cold zones.
4. Metadata is reset when a node is reallocated to a new zone.

### Zone Rebalancing

To mitigate long-term skew and improve wear leveling:

1. Hot/cold labels are periodically rebalanced across physical zones.
2. Trigger occurs at zone initialization.
3. This is outside the critical path of tree operations.

## 3.1.2 Two-Stage Node Tracking

### Core Idea

Out-of-place updates in ZNS would normally trigger recursive parent rewrites. ZTree reduces this by decoupling child location tracking from direct page addresses.

### Node and Pointer Fields

1. NodeID is 4 bytes per node.
2. Parent entries store ZoneID + NodeID, not raw LBA/page address.

A child update within the same zone can often avoid parent rewrite.

### NLTable Structure (Paper-Level Detail)

Each NLTable zone entry contains:

1. ZoneID: 2 bytes.
2. Sealed flag: 1 bit.
3. One Node Tracker structure.

Node Tracker entry format:

1. NodeID: 4 bytes.
2. SlotID: 3 bytes.

### Address Resolution

Node physical location is computed as:

$$
Offset_{Node} = Offset_{Zone} + SlotID \times NodeSize
$$

This lookup path is lock-free in the paper design, minimizing contention for concurrent reads.

### Memory-Constrained NLTable Policy

When NLTable memory is limited:

1. RLayer and ILayer table entries are always cached in memory.
2. LLayer prioritizes hot-zone entries in memory.
3. Other entries are flushed to SSD.
4. Entries are loaded on zone open and released on zone close.

## 3.2 Tree Operations

ZTree supports SEARCH, UPDATE, INSERT, and DELETE.

### SEARCH

Top-down traversal:

1. Read root from RLayer.
2. In each internal node, binary-search greatest key less than or equal to target.
3. Extract child ZoneID and NodeID.
4. Open child zone and resolve SlotID via NLTable.
5. Compute node offset and continue.
6. At leaf, binary search target key.

Concurrency control: latch crabbing with read locks from parent to child in top-down order.

### UPDATE

1. Find target leaf.
2. Update value in target KV entry.
3. If zone usage is below threshold (95 percent capacity), append updated node in same zone.
4. Otherwise allocate new zone dynamically and append there.
5. Sync NLTable.
6. Update parent ZoneID reference only when zone migration occurs.

### INSERT

1. Find target leaf.
2. If space exists, insert KV and append updated node.
3. If overflow occurs, split and propagate median.

Paper split persistence order (crash consistency):

1. Phase 1: persist sibling node first.
2. Phase 2: update parent with sibling pointer (ZoneID, NodeID) and persist parent.
3. Phase 3: persist updated original node last.

Reason: persisting original before parent can leave sibling unreachable after crash.

### DELETE

1. Remove target KV from leaf.
2. If node still above minimum capacity, append updated node.
3. If underfull and current + sibling data fits one node, merge.
4. Persist merged current node.
5. Remove sibling index from parent and persist parent.
6. Invalidate sibling in NLTable and sync table.

ZTree avoids aggressive borrow-based balancing to reduce extra writes and cascading parent updates.

## Implementation Checklist (Paper-Conformant)

Use this checklist when validating an implementation against the paper:

1. SlotID is explicitly defined and tracked per-zone sequential append order.
2. Default IZGroup/LZGroup initial counts are 4 and 6 (or clearly documented override).
3. Heat metadata stores explicit field sizes (Counter 2-byte, Timestamp 2-byte) if claiming strict paper fidelity.
4. NLTable data structure includes ZoneID, Sealed flag, and Node Tracker with NodeID + 3-byte SlotID semantics.
5. Insert split persistence strictly follows sibling -> parent -> original.
6. Update threshold behavior at 95 percent zone capacity is implemented.
7. Latch-crabbing read path is parent-before-child and deadlock-safe.