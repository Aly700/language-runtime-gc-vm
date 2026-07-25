# ADR 0020: Explicit Weak String Interning

## Status

Accepted for Iteration 44.

## Context

Strings are immutable byte objects and `StrEq` is structural. Before this
iteration, equal strings could be distinct heap objects and source had no
operation that promised sharing. The canonical graph oracle removes raw object
IDs but preserves aliasing, so a new interning operation makes one additional
fact observable: two live results for equal bytes must be the same graph node.

A strong intern table would satisfy that fact but would also retain every
canonical string forever. It would turn an optimization-oriented lookup
facility into a new root set, change collection and benchmark behavior for
programs that use it, and make bounded heap behavior depend on all historically
interned bytes. A weak table avoids that retention, but only if collection
schedule cannot change a program's two observables.

## Decision

Source exposes the explicit builtin expression `intern(s)`. The type checker
requires one proven non-nil `str`, the compiler emits append-only `StrIntern`,
and the verifier models the opcode as `str -> str`. The operand stack slot is a
precise root at the opcode boundary and remains rooted across a miss
allocation, exactly as the source operand of `StrConcat` and `StrSub` remains
rooted.

The heap owns a private ordered vector of entries:

```text
(content_hash: u64, canonical: ObjectId)
```

An entry is a weak edge in the existing collector-owned weak-edge category. It
is not a descriptor edge, root, remembered-set entry, ephemeron, or third
reference path. Marking never visits it. After liveness is fixed, the existing
weak-processing phase forwards a canonical with a forwarding entry and evicts
an entry whose canonical has no forwarding entry. Minor collection applies the
same rule: an independently live young canonical is promoted and forwarded,
while a weak-only young canonical is evicted.

Entries are unique by immutable byte content and kept in strict ascending
canonical base-slot order. Lookup scans in that deterministic order, checks the
stored hash first, and confirms a candidate with byte-for-byte structural
comparison. The hash is Iteration 40's fixed 64-bit FNV-1a encoding: offset
basis `14695981039346656037`, prime `1099511628211`, string domain byte `3`,
then the immutable bytes in order. Object ID, slot, generation, address,
`std::hash`, host representation, random seed, process state, and collection
schedule do not participate.

On a miss, the heap allocates a fresh canonical copy rather than registering the
argument object itself. This keeps the contract independent of how the
argument was constructed and gives the potential allocation one explicit
rooting path. On a hit, the existing canonical is returned.

## Schedule-transparency argument

Fix a byte sequence `b`, an `intern(b)` call, and any two collector schedules.
There are two exhaustive cases immediately before that call.

1. Another live strong reference reaches the canonical `c`. Every sound
   schedule must preserve `c`. A moving schedule forwards both that strong
   reference and the weak table entry to the same current object. Therefore
   every equal call returns `c`, and all sharing visible in the canonical graph
   is identical.
2. No live strong reference reaches `c`. A schedule that collected since the
   last use has evicted the entry, so the call allocates a fresh canonical
   `c'`. A schedule that has not collected may still have the physically
   allocated but unreachable `c`; the call returns it and thereby makes it
   reachable again. No other live incoming reference can distinguish `c` from
   `c'` by the case premise. Strings are immutable, both contain exactly `b`,
   structural operations and output see the same bytes, and the ID-free graph
   oracle differs only by an unobservable renaming of one otherwise fresh
   node.

Thus weak-table clearing can change allocation and raw identity history, but it
cannot change output bytes or the canonical graph. This is narrower than
general finalizer resurrection: no callback runs, no arbitrary object state is
revived, and no dead object can be observed through a second live reference.
The isolated `interning` grammar checks both oracles for 32 programs under all
15 shared schedules.

Case 2's uncleared branch is deliberately supported. The mutator reads the
table only between collection boundaries, when every allocated object is still
physically present. The returned value is immediately an ordinary precise
stack root and thereafter participates in strong tracing normally.

## Movement and incremental phase discipline

Atomic major and minor compaction process the table from the completed
forwarding table before installing moved storage. All-live map-growth
relocation forwards collector-owned tables without making a liveness decision.

Incremental compaction prunes dead entries after final liveness and before
sweeping dead headers. Each complete-object relocation forwards a matching
canonical entry before the mutator boundary validator runs. The independent
atomic shadow snapshot includes the table and separately rewrites and compares
every hash/target pair.

`intern_string` does not inspect a partially forwarded table. If called during
incremental compaction, it finishes that phase first with its source value as
an extra precise root, then performs lookup. This simple phase rule avoids a
second lookup representation and keeps resurrection restricted to a coherent
heap boundary. During incremental marking, a hit is a weak-to-strong mutator
publication and enqueues the canonical on the ordinary grey worklist; a miss
allocation already enters that worklist.

`validate_intern_table` runs after insertion and at collection, relocation,
incremental-step, phase-finalization, and test invariant boundaries. It proves
that every target is a current generation-valid `Str`, entries are in strict
base-slot order, hashes recompute from the target bytes, and no two entries are
structurally equal. A one-shot test hook skips dead-entry eviction; the
validator must reject the resulting stale target, proving the boundary is
non-vacuous.

## Consequences

- While a canonical is strongly reachable, equal `intern` calls return that
  exact object across every movement and collection schedule.
- The table never extends liveness and adds no remembered-set or write-barrier
  edge.
- Equal strings remain structurally equal whether or not either was interned;
  maps, records, closure captures, and output require no special cases.
- Programs that never execute `intern` allocate no table entries. Existing
  opcodes, source corpora, benchmark workloads, metrics, and counter streams
  retain their prior behavior.

## Rejected alternatives

- A strong table was rejected because it would be a permanent root set.
- Implicit literal or allocation interning was rejected because it would change
  legacy allocation, movement, graph sharing, and benchmark behavior.
- Object-ID, address, pointer, or host-library hashing was rejected because it
  is movement- and process-dependent.
- Registering the caller's argument as canonical was rejected in favor of one
  explicit miss-allocation/rooting contract.
- Treating table entries as descriptor fields, remembered-set entries, or a
  new reference category was rejected because it would violate the exhaustive
  strong/weak edge rule.
- Looking up through a partially forwarded incremental-compaction table was
  rejected because completing the phase is simpler and independently
  validated.
- Cached hashes on string objects and bucketed table indices remain deferred;
  the deterministic content scan is sufficient for this explicit semantic
  surface.
