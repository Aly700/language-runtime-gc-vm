# Trace Bundle Schema (Draft)

`lang_trace` writes one deterministic bundle per run:

- `events.jsonl` — ordered runtime and collector observations;
- `snapshots.jsonl` — seek points for the logical heap graph;
- `stats.json` — measured run totals;
- `positions.json` — the compiled module's pc-to-source export; and
- the program's VM output bytes on `lang_trace` stdout.

All files use UTF-8 JSON and LF (`0x0a`) line endings. JSONL files contain exactly one
object per line. Numeric values are integers; the format has no floating-point values,
wall-clock timestamps, host addresses, process identifiers, or randomized hashes.

## Units and identity

A storage slot is 8 bytes. `size` is the collector's complete logical storage width in
slots, using the same descriptor-width function as allocation and compaction. Fixed
payloads embedded by the runtime, notably `Pair`, retain their existing one-logical-slot
accounting. Variable-width strings, arrays, closures, maps, records, variants, and
Builders use their exact collector width.

An object ID is the unsigned integer

```text
id = base_slot | (slot_generation << 32)
```

where `base_slot` is the low 32 bits. The high component is the slot-reuse generation,
not the young/old generation. The separate `gen` field is `0` for young and `1` for old.
Every ID is printed as a JSON integer, never as a pointer or hexadecimal string.

Movement changes the move-sensitive ID. `relocate` and `promote` carry the old ID in
`id` and the new ID in additive `to_id`. A renderer preserves logical identity by
following `id -> to_id` chains and rewriting references that name `id`. A physical move
that also promotes may produce adjacent `relocate` and `promote` observations with the
same mapping; the second is a generation annotation and the mapping is idempotent.

## Event stream

Every event has the following keys in exactly this order:

```json
{"tick":0,"seq":0,"kind":"alloc","id":4294967296,"size":1,"gen":0,"from":null,"to":null,"refs":[],"src_pos":{"line":1,"col":1,"fn":"<entry>"},"object_kind":"pair"}
```

The fixed prefix is:

1. `tick`
2. `seq`
3. `kind`
4. `id`
5. `size`
6. `gen`
7. `from`
8. `to`
9. `refs`
10. `src_pos`

Non-applicable fixed fields are JSON `null`; `refs` is an array only when the event
defines it. Kind-specific additive fields follow `src_pos` in the order documented
below.

`tick` is the number of VM bytecode instructions retired before the current instruction.
Events caused before, during, or after one instruction share that instruction's tick.
Collector work at an instruction boundary uses the upcoming instruction's tick. A
trapping instruction is not retired. The exit snapshot and `stats.json` `ticks` contain
the final retired-instruction count.

`seq` is a zero-based event index, increasing by one for every event. It disambiguates
events with equal ticks. It is scoped to one run.

`src_pos` is `{"line":L,"col":C}` with optional additive `"fn":"NAME"` when the
frontend debug table contains the current pc. It is populated for mutator-side `alloc`,
`intern`, and `trap` events. It is `null` for collector-internal movement, death,
marking, eviction, and validation work, and for hand-built bytecode without a position.

### Required kinds

| `kind` | Fixed fields | Additive fields and semantics |
|---|---|---|
| `alloc` | `id`, `size`, `gen`, `refs`, `src_pos` | `object_kind` is the heap descriptor name. One event is emitted for every object kind. `refs` contains all non-nil outgoing object IDs after constructor initialization: strong fields in descriptor order, followed by the weak-category fields described under snapshots. No references is `[]`. |
| `mark_slice` | `size` | `size` is the number of complete descriptor objects scanned by that incremental-marking budget step, including zero. |
| `relocate` | `id`, `size`, `gen`, `from`, `to` | `to_id` is the post-move ID. Emitted for each base-slot move during atomic or incremental compaction and for deterministic Map/Builder width-growth relocation, so no runtime identity transition is hidden. Collector source position is null. |
| `promote` | `id`, `size`, `gen=0`, `from`, `to` | `to_id` is the survivor ID. One event is emitted for every young-to-old survivor, including an in-place promotion where `from == to`. |
| `die` | `id`, `size`, `gen` | One event per object selected as dead. Within a collection, `die` observations use ascending source base-slot order. |
| `intern` | `id`, `size`, `gen`, `src_pos` | `hit` is integer `1` for an existing canonical and `0` for a miss-created canonical. `id` is the canonical `Str`. |
| `evict` | `id`, `size`, `gen` | The weak intern-table canonical selected for eviction. Emission precedes removal and does not keep the string alive. |
| `trap` | `src_pos` | `reason` is the caught runtime diagnostic string. `id`, `size`, `gen`, movement fields, and `refs` are null. Uncaught typed exceptions use their existing deterministic message. |
| `verify_step` | optional `size` | `check` names the validator. Current names are `validate_after_collection`, `incremental_tricolor`, `shadow_marking`, `shadow_compaction`, `remembered_set`, `weak_targets`, `ephemerons`, and `intern_table`. `size` is null unless the validator already has an exact element count; no count is estimated. |

Strings in `kind`, `reason`, `check`, `object_kind`, and `fn` are schema labels or copied
diagnostics, not numeric measurements. Boolean measurements such as `hit` are encoded as
integer `0` or `1`.

## Snapshots

Each line of `snapshots.jsonl` has exactly:

```json
{"tick":12,"seq":4096,"live":[{"id":4294967296,"kind":"pair","size":1,"gen":1,"refs":[4294967297]}]}
```

Snapshot top-level key order is `tick`, `seq`, `live`. Object key order is `id`, `kind`,
`size`, `gen`, `refs`. Objects are ordered by ascending base slot. Strong references are
ordered by the existing descriptor visitor: Pair left/right, RefArray index order,
Closure capture-bitmap order, Map insertion key/value order, Record declaration order,
and active Variant payload order. Scalar payloads never appear.

The collector's weak category is reported without changing it. A non-nil WeakRef target
is appended after its empty strong descriptor. An Ephemeron appends its non-nil key and
then its reference-typed non-nil conditional value. Nil weak/ephemeron fields are omitted.
This reporting neither marks nor roots the targets.

There is a snapshot at program start, at every configured `N`-event boundary, and at
program exit. The default interval is 4096. A snapshot's `seq` names the next event to
apply: the graph is the state immediately before event `seq`. Start is therefore
`seq: 0`; an exit snapshot may intentionally repeat a periodic `seq` when the run ends on
that boundary.

Snapshot material comes from `Heap::trace_snapshot`, a read-only ascending-slot walk
using the production descriptor visitor with metric accounting disabled. While a
collection has produced logical death/movement observations but has not yet installed
its physical result, the writer advances a private observer mirror from that read-only
walk. Collection transaction boundaries resynchronize it from another read-only walk.
The mirror is host-only trace state: it cannot mark, forward, retain, or influence a heap
object.

### Seeking and replay

To render event `S`:

1. choose the snapshot with the greatest `seq <= S`;
2. load its `live` list as the complete graph immediately before that sequence;
3. replay events beginning at the snapshot's `seq` and ending at `S`;
4. remove objects on `die`, add them on `alloc`, rewrite IDs and all matching references
   on `relocate`/`promote`, and treat other events as annotations.

Following `to_id` chains is sufficient to preserve logical identity through repeated
movement. Renderers must not infer identity from a base slot alone.

## Statistics

`stats.json` is one JSON object with keys in this order:

```json
{"live_bytes_final":0,"forwarded_reference_count":0,"pause_slices":0,"collection_count":0,"event_totals":{"alloc":0,"mark_slice":0,"relocate":0,"promote":0,"die":0,"intern":0,"evict":0,"trap":0,"verify_step":0},"ticks":0,"peak_live_bytes":0}
```

- `live_bytes_final`: sum of final live descriptor widths multiplied by 8.
- `forwarded_reference_count`: actual old-ID-to-new-ID rewrites performed in precise
  `Value` slots and collector-owned reference registries/worklists. Equal-ID rewrites and
  clears are not counted.
- `pause_slices`: observer count of explicit stop-the-world entries: atomic collections,
  incremental cycle starts, marking/compaction budget steps, final liveness pauses, and
  compaction finalization.
- `collection_count`: the real `HeapMetrics.major_collections +
  HeapMetrics.minor_collections` total. Incremental major cycles use the same existing
  major counter.
- `event_totals`: exact emitted count for every required kind, including zeros.
- `ticks`: final VM retired-instruction count.
- `peak_live_bytes`: greatest exact live descriptor-width total sampled at program start,
  every allocation, every Map/Builder width change, collection completion, and exit.

No statistic is inferred from wall time or filled with a demonstration constant.

## Position export

`positions.json` has one `functions` array in module index order. Each element is

```json
{"index":0,"name":"<entry>","positions":[{"pc":0,"source":{"line":1,"col":1}}]}
```

`name` or `source` is null when verifier-inert debug metadata is absent. Positions are in
ascending pc order and are copied directly from the ADR-0021 tables.

## Determinism guarantee

For an equal verified program, equal named stress schedule, equal snapshot interval, and
equal runtime version, all four trace files are byte-identical. Ordering is derived only
from VM retired instructions, event sequence, heap base slots, descriptor visitors,
ordered collector registries, and module pc order. The emitter never reads a wall clock,
host pointer, random device, thread schedule, or unordered container.
