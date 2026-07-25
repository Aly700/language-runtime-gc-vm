# ADR 0017: Deterministic Content-Hashed Map Lookup Index

## Status

Accepted for Iteration 40.

This ADR refines ADR-0004's lookup-representation decision. ADR-0004 remains
authoritative for insertion-order payloads, structural key equality, layout precision,
growth relocation, and iteration semantics. Its rejection of `ObjectId` and host-pointer
identity hashing also remains authoritative. This ADR supersedes only its broader
conclusion that maps should use no hashing.

## Context

ADR-0004 correctly observed that a moving collector changes an object's slot and
generation. Hashing a string key by `ObjectId`, address, or pointer would therefore make
the bucket depend on movement or process state. It deferred open addressing until a
profile justified the added representation and validation obligations.

The existing `map_heavy` and `mixed_graph` workloads did not isolate lookup: they combined
map operations with insertion, allocation, growth relocation, descriptor validation, and
collection. Iteration 40 therefore added `map_lookup_heavy`, which builds 192 immutable
long-common-prefix string keys once and performs 2,000 rounds of first, middle, last, and
missing lookup through fresh byte-equal query strings. Its lookup loop allocates nothing
and performs no collection.

On the assertions-enabled Apple Clang 17 build, the pre-change workload executed 982,336
candidate comparisons and had a seven-run median of 278.967 ms. Temporary disjoint timing
instrumentation, removed before implementation, attributed 58.1%–65.7% of end-to-end time
to the linear structural scan across five independent runs. Lookup was therefore a
measured dominant cost center and admitted Phase B.

## Decision

The ordered `std::vector<MapEntry>` remains the sole language-visible map payload and the
only authority for:

- insertion order and positional `MapKeyAt`/`MapValueAt` access;
- descriptor traversal and the order of strong edges;
- mutation-during-iteration entry-count checks;
- logical width `1 + 2N`, growth relocation, and compaction payload accounting; and
- canonical graph and fuzz-corpus observables.

Each map additionally owns a private `std::vector<std::size_t>` lookup index. Bucket zero
is empty; occupied value `n` names ordered entry `n - 1`. Maps do not delete entries, so
the table needs no tombstones. An empty map has an empty table. The first insertion uses
capacity eight; capacity is always a power of two and doubles before an insertion would
exceed one-half load. A rebuild inserts entries in ascending ordered-vector index and
resolves collisions by ascending linear probing. Capacity, rebuild points, collision
order, and bucket contents are consequently deterministic.

Key hashing is 64-bit FNV-1a with fixed offset basis
`14695981039346656037` and prime `1099511628211`. A fixed leading domain byte is `1` for
`i64`, `2` for `bool`, and `3` for `str`.

- An `i64` contributes the eight bytes of its `uint64_t` conversion in explicit
  little-endian order.
- A `bool` contributes exactly one byte, `0` or `1`.
- A `str` contributes its immutable bytes in order.

No `ObjectId`, slot, generation, pointer, address, native object representation,
`std::hash`, locale, random seed, per-process seed, thread, clock, or host endianness
participates. Equal key contents therefore hash identically before and after movement and
on every run. Hash equality is only a candidate filter; final equality remains the
existing tagged scalar or byte-for-byte structural comparison.

Lookup hashes the query once, probes from its home bucket, and compares only occupied
candidate entries on that probe path. A miss stops at the first empty bucket. Updating an
existing key retains its ordered entry and all buckets. Inserting a new key prepares any
resized table before publication, appends the entry exactly once, publishes its encoded
entry index, and updates the descriptor length through the existing
`Heap::store_map_entry` barrier-before-publish funnel.

The index is collector-transparent scalar metadata. It contains no `Value`, `ObjectId`,
pointer, or owner handle and defines no root, strong edge, weak edge, ephemeron edge,
remembered-set edge, forwarding input, or liveness fact. Atomic compaction, incremental
compaction, and map-growth relocation copy it with the object. Forwarding string key IDs
does not require rehashing because immutable content and ordered entry positions do not
change. The auxiliary host vector does not change logical heap width or compaction byte
accounting.

A loud coherence validator runs immediately after every map mutation and through heap
layout validation at every collection boundary. It proves exact capacity/load policy,
in-range and unique entry indices, presence of every ordered entry, probe reachability,
and byte-for-byte equality with a deterministic rebuild. The incremental-compaction
shadow equality oracle also includes the index. A test-only hook clears an occupied bucket;
the invariant validator must reject the resulting missing entry, proving the check is
non-vacuous.

`map_lookup_entries_examined` continues to count candidate equality checks only.
`map_hash_probes` counts every lookup bucket probe, and
`map_index_validation_entries` counts ordered entries consumed by coherence rebuilds.
These are passive measurements and never influence control flow.

## Measured consequences

The final deterministic captures are:

| workload | comparisons before | comparisons after | hash probes | validation entries |
| --- | ---: | ---: | ---: | ---: |
| `map_lookup_heavy` | 982,336 | 8,069 | 10,260 | 38,746 |
| `map_heavy` | 41,809 | 951 | 1,046 | 97,831 |
| `mixed_graph` | 320 | 320 | 320 | 632 |

Candidate comparisons fell 99.2% on the isolated lookup workload and 97.7% on
`map_heavy`. A complete counter-stream comparison against revision `9223111` plus only
the Phase-A workload extension found every pre-existing counter other than the deliberately
optimized comparison counter byte-identical. In particular, map descriptor scans,
allocator work, heap shape, movement, barriers, collections, logical map-copy bytes, and
validation counters outside the new index metric did not change.

Seven-run medians on the same assertions-enabled host were:

| workload | before ms | after ms | change |
| --- | ---: | ---: | ---: |
| `map_lookup_heavy` | 278.967 | 153.575 | -44.9% (1.816x) |
| `map_heavy` | 133.031 | 121.033 | -9.0% (1.099x) |
| `mixed_graph` | 15.684 | 11.877 | -24.3% |

The isolated workload is the decision result. `mixed_graph` performs no fewer candidate
comparisons, so its wall-time change is reported as host-run variance rather than a second
optimization claim. Timings are informational and never test thresholds.

All 19 pinned fuzz corpus dumps, including maps, for-in loops, incremental compaction, and
tail calls, remain byte-identical to the clean revision captures.

## Rejected alternatives

- `ObjectId` or pointer hashing remains rejected because movement changes identity bits
  and addresses expose process nondeterminism while creating another reference path.
- Reordering or sorting the entry vector remains rejected because insertion order is
  observable language semantics under ADR-0007.
- Storing object references in buckets remains rejected because it would add liveness and
  forwarding obligations outside the two-category edge rule.
- Per-process hash seeding remains rejected because it would make collision order and
  passive probe counts nondeterministic.
- Cached hashes beside entries or on string objects remain deferred. Scalar cached hashes
  would be movement-safe, but the measured index is sufficient and a parallel cache would
  add another coherence contract.
- Removing or sampling existing descriptor and collection validation remains rejected.
  The measured speedup must coexist with the runtime's loud invariant boundaries.
