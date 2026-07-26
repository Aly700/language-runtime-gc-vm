# ADR 0023: Deterministic Mutable String Builder

## Status

Accepted for Iteration 47.

## Context

`Str` is an immutable, GC-opaque byte object. `StrConcat` therefore allocates a
new `Str` and copies both inputs. Reassigning the result in an incremental
construction loop repeatedly copies the complete prefix and has quadratic total
copy cost.

ADR-0009 deliberately rejects string views: every derived string is an
independent immutable copy with no owner edge. That rule must remain true.
ADR-0004's `Map` is the only existing container whose logical heap width grows
after allocation. It establishes the required discipline for deterministic
out-of-collection relocation and complete forwarding.

The missing abstraction is a mutable, append-oriented byte buffer that can grow
geometrically, while keeping `Str` immutable and keeping arbitrary payload bits
outside the collector's reference graph.

## Decision

### Source surface

The language adds the non-nil reference type `builder`:

```text
let b: builder = builder();
b.append("count=");
b.append(42);
b.append(",");
b.append(true);
let frozen: str = b.to_str();
let size: i64 = b.len;
b.clear();
```

`builder()` takes no arguments. `append` accepts exactly `str`, `i64`, or
`bool`. The compiler lowers `i64` and `bool` through the existing `I64ToStr`
and `BoolToStr` operations, so the heap mutation receives only a `Str`.
`append` and `clear` are statements; `.len` produces `i64`; `.to_str()`
produces `str`.

Builders are ordinary reference values. They may be assigned, passed,
returned, captured, stored in reference-typed array, pair, record, variant, or
map positions, and targeted by `weak<builder>`. Aliases observe the same mutable
buffer.

`clear` sets the logical length to zero but retains identity and capacity.
The Builder remains usable. There is no shrink operation.

### Immutable snapshots

`to_str()` always allocates a fresh `ObjectKind::Str` and copies exactly the
Builder's current logical bytes. This includes the empty case. The Builder
remains usable, and later append or clear operations cannot change any earlier
snapshot.

This is intentionally consistent with ADR-0009: a Builder snapshot is neither a
view nor shared mutable storage. `Str` remains one immutable, zero-edge
representation.

### Heap representation and opacity

`ObjectKind::Builder` consists of an ordinary object header plus:

- a `uint32_t` logical length;
- a `uint32_t` deterministic capacity; and
- a raw byte vector whose size equals the logical length.

The descriptor visitor visits **zero references**. Builder bytes are never
tagged `Value`s or `ObjectId`s. A byte sequence equal to the complete bits of a
live, dead, stale, or forwarded object ID must never mark, retain, forward,
clear, barrier, or validate that ID.

Shape validation is loud and runs at allocation, every mutation, relocation,
and collection boundary. It proves:

- payload byte count equals logical length;
- logical length does not exceed capacity;
- capacity belongs to the deterministic ladder; and
- every non-Builder descriptor has zero Builder-only metadata.

The logical heap width is:

```text
1 + ceil(capacity / 8)
```

where the header is one logical slot and each payload slot represents eight
bytes. Logical length does not determine width.

### Deterministic capacity policy

A fresh Builder has capacity eight bytes. When an append would exceed capacity,
capacity doubles until it covers the required logical length. The ladder is:

```text
8, 16, 32, 64, ...
```

The final step saturates at `UINT32_MAX`; requests beyond the object-header
limit trap with a length error. No address, allocator result, native `size_t`
width, clock, random value, or process state influences capacity.

`clear` retains capacity. A subsequent append reuses it.

### Growth and relocation

Growth occurs only between collections. The heap first computes the new
capacity and logical width. If the additional adjacent logical slots are free,
the Builder extends in place and retains its complete object ID. If another
object blocks the run, the heap uses the ADR-0004 growth pattern:

1. build an identity forwarding table for every current object;
2. assign the Builder a deterministic new run at the old heap end;
3. copy its descriptor and bytes, clear the old header, and forward its ID;
4. rewrite all precise roots and descriptor-declared strong edges;
5. forward the remembered set, handles, VM roots, explicit roots, WeakRef
   targets/owners, ephemeron registry state, weak intern-table entries, and an
   active incremental-marking worklist;
6. install the moved heap and run every layout/registry validator; and
7. update capacity and append only after storage is secured.

The old complete ID becomes stale and its old run is immediately reusable under
the generation discipline. Growth increments `objects_moved` exactly once when
relocation is required.

Collection records one width per object and asserts that the width is unchanged
until that collection completes. A width-changing append during incremental
compaction first finishes compaction, with both the Builder receiver and source
`Str` held as temporary precise roots. The append reacquires both descriptors
after forwarding before reading or publishing bytes. Fixed-width append and
clear remain legal during active incremental compaction and update its
authoritative shadow without ending the phase.

### Single mutation funnel

Every append and clear flows through `Heap::mutate_builder`. No mutable byte
payload is exposed.

The funnel validates receiver/source kinds, length arithmetic, capacity and
descriptor shape, performs any required growth under the phase discipline,
mutates the payload, updates the logical length, synchronizes an active
incremental-compaction shadow, and validates heap layout.

No write barrier is required. Builder payload bytes contain no reference edge
by construction, exactly like `ScalarArray` raw integers and immutable `Str`
bytes. The absence of a barrier is a consequence of the descriptor shape, not
an alternate mutation path.

### Bytecode and verification

Five append-only operations are added after the prior opcode tail:

```text
AllocBuilder
BuilderAppend
BuilderLen
BuilderToStr
BuilderClear
```

The verifier has a distinct `ValueKind::Builder` and abstract Builder kind.
`BuilderAppend` consumes `builder, str`; length, snapshot, and clear require a
Builder receiver. The two appended stable rejection reasons distinguish a
non-Builder receiver from a non-`Str` append operand.

Stack maps mark Builder and `Str` operands as exact roots. The VM leaves
operands on the frame until heap append/snapshot operations complete, allowing
collection or growth relocation to rewrite them before the stack is popped.

## Determinism and precision argument

Capacity and relocation order use only checked integer arithmetic, ascending
slot order, and the fixed doubling ladder. Payload order is append call order.
The visitor exposes zero payload fields, and every reference-bearing owner of a
Builder uses the pre-existing descriptor maps. All movement registries use the
same forwarding table as Map growth.

Consequently, GC scheduling may change physical IDs but cannot change Builder
bytes, snapshots, output, returned canonical graphs, weak clearing decisions at
a fixed liveness boundary, or capacity transitions.

## Validation evidence

The focused crown tests cover:

- exact `8 -> 16 -> 32` capacity changes;
- in-place extension and blocked deterministic relocation;
- stale-ID rejection and old-run reuse;
- append growth while incremental compaction is scheduled, with the source
  `Str` retained only by the temporary operand root;
- fixed-width append/clear shadow synchronization;
- Builder bytes equal to a dead `ObjectId` never being traced or rewritten;
- one Builder retained through a record field, closure capture, and map value
  during stepped incremental compaction;
- `weak<builder>` clearing;
- independent `to_str()` snapshots, including distinct empty snapshots; and
- exact source execution/output under all fifteen collection schedules.

The isolated `builder` grammar has 32 seeds, 15 schedules, two non-empty
oracles, and eight positioned rejection mutants per seed. Its pinned source,
representative graph/output outcome, and full corpus FNV-1a values are:

```text
10719162047016123221
3422408984983186133
1386632754159073109
```

The paired benchmark builds and prints the same 8 KiB byte stream from 64
fixed 128-byte chunks. On the assertions-enabled development host, seven-run
medians were 1,861.537 ms for reassigned `StrConcat` and 8.431 ms for Builder
(220.8x). Deterministic allocations fell from 66 to 3, heap peak slots from
33,362 to 2,067, and storage-occupancy header examinations from 567,995,444 to
2,119,005. Layout assertions amplify the wall-time difference, so timing is
informational; the allocation and growth semantics stand independently.

## Consequences

- Incremental string construction is amortized linear in appended bytes,
  excluding explicit `to_str()` snapshot copies.
- `Str` stays immutable, zero-edge, and copy-not-view.
- Builder mutation is observable through aliases and therefore intentionally
  differs from string value semantics.
- A Builder can move during growth even when no collection is active; callers
  must hold it in precise roots or handles rather than retain raw IDs.
- Capacity may exceed logical length after clear or a small append. Capacity is
  not source-observable and is omitted from canonical fuzz graphs.
- Builder payloads need no generational or incremental write barrier.

## Rejected alternatives

- Repeated `StrConcat` was rejected because it retains quadratic prefix-copy
  behavior.
- A mutable `Str` was rejected because it would invalidate ADR-0002/ADR-0009
  immutability, structural map keys, interning, and snapshot independence.
- Rope or substring-view snapshots were rejected because they introduce
  reference-bearing string representations and lifetime/forwarding edges.
- A reference-element buffer was rejected because bytes are scalar data and
  false reference edges would weaken collector precision.
- Fixed capacity was rejected because it merely moves the construction limit.
- Host allocator growth policy was rejected because capacity and logical width
  must be reproducible.
- Shrinking on clear was rejected because it adds another relocation policy and
  defeats reuse.
- Dedicated integer/bool append opcodes were rejected because existing
  conversion lowering already defines their exact textual form.
