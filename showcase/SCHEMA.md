# Trace Bundle Schema

**Status:** Final

`lang_trace` writes one deterministic bundle per run:

- `events.jsonl` — ordered runtime, collector, and replay-evidence observations;
- `snapshots.jsonl` — seek points for the logical heap graph;
- `stats.json` — measured run totals;
- `positions.json` — the compiled module's pc-to-source export; and
- the program's VM output bytes on `lang_trace` stdout.

All files use UTF-8 JSON and LF (`0x0a`) line endings. JSONL files contain exactly one
object per line. Numeric values are integers; the format has no floating-point values,
wall-clock timestamps, host addresses, process identifiers, or randomized hashes.

The conservation checker proves internal consistency of the supplied bundle: lifecycle,
movement, graph, snapshots, and independently repeated stats ledgers must agree. The
bundle is not a cryptographic attestation. A party that can coherently reauthor every
event and every repeated ledger can construct a different valid bundle; provenance
against that threat requires an externally trusted digest or signature, which is outside
this schema and T2.

## Bundle layout

The checked-in showcase index is `showcase/manifest.json`, with the authoritative schema
copy at `showcase/SCHEMA.md`. Each measured workload lives at
`showcase/traces/<demo>/`: `events.jsonl`, `snapshots.jsonl`, `stats.json`, and
`positions.json` are unmodified `lang_trace` outputs; `program.lang` is the exact source
passed to the emitter; and `output.txt` is the VM stdout captured as raw bytes.

The manifest has a top-level `note` naming the emitter commit and an ordered `artifacts`
array. Every artifact records `id`, `type` (`trace-bundle`, `schema`, or `source`), the
fixed `label` `measured`, its showcase-relative `path`, `desc`, executed `schedule`
(`null` for the schema), and per-file byte `sizes` and lowercase hexadecimal `sha256`
maps. A trace-bundle entry covers all six files in its demo directory. The manifest does
not hash itself; `showcase_pin` regenerates and byte-compares the entire tree, including
the manifest, schema, sources, stdout, and every emitter file.

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
The first ID at a base has slot generation 1 and every reuse increments it by exactly
one. Every ID is printed as a JSON integer, never as a pointer or hexadecimal string.

Movement changes the move-sensitive ID. `relocate` and `promote` carry the old ID in
`id` and the new ID in additive `to_id`. A renderer preserves logical identity by
following `id -> to_id` chains and rewriting references that name `id`. A physical move
that also promotes may produce adjacent `relocate` and `promote` observations with the
same mapping; the second is a generation annotation and the mapping is idempotent.
`relocate` also carries `move_kind`: `compaction` for collector movement and `growth`
for a Map/Builder identity transition that precedes a width-changing mutation.

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
`update`, `intern`, and `trap` events. It is `null` for collector-internal movement, death,
marking, eviction, and validation work, and for hand-built bytecode without a position.

### Event kinds

| `kind` | Fixed fields | Additive fields and semantics |
|---|---|---|
| `alloc` | `id`, `size`, `gen`, `refs`, `src_pos` | `object_kind` is the heap descriptor name. One event is emitted for every object kind. `refs` contains all non-nil outgoing object IDs after constructor initialization: strong fields in descriptor order, followed by the weak-category fields described under snapshots. No references is `[]`. |
| `mark_slice` | `size` | `size` is the number of complete descriptor objects scanned by that incremental-marking budget step, including zero. |
| `relocate` | `id`, `size`, `gen`, `from`, `to` | `to_id` is the post-move ID. `move_kind` is `compaction` or `growth`. Emitted for each base-slot move during atomic or incremental compaction and for deterministic Map/Builder width-growth relocation, so no runtime identity transition is hidden. Collector source position is null. |
| `promote` | `id`, `size`, `gen=0`, `from`, `to` | `to_id` is the survivor ID. One event is emitted for every young-to-old survivor, including an in-place promotion where `from == to`. |
| `die` | `id`, `size`, `gen` | One event per object selected as dead. Within a collection, `die` observations use ascending source base-slot order. |
| `intern` | `id`, `size`, `gen`, `src_pos` | `hit` is integer `1` for an existing canonical and `0` for a miss-created canonical. `id` is the canonical `Str`. |
| `evict` | `id`, `size`, `gen` | The weak intern-table canonical selected for eviction. Emission precedes removal and does not keep the string alive. |
| `trap` | `src_pos` | `reason` is the caught runtime diagnostic string. `id`, `size`, `gen`, movement fields, and `refs` are null. Uncaught typed exceptions use their existing deterministic message. |
| `verify_step` | optional `size` | `check` names the validator. Current names are `validate_after_collection`, `incremental_tricolor`, `shadow_marking`, `shadow_compaction`, `remembered_set`, `weak_targets`, `ephemerons`, and `intern_table`. `size` is null unless the validator already has an exact element count; no count is estimated. |
| `update` | `id`, `size`, `gen`, `refs` | Complete post-mutation traced state for an already-live object. `object_kind` repeats the immutable descriptor name. Identity, kind, and young/old generation cannot change; `size` and ordered `refs` replace their prior values. Mutator updates carry the current `src_pos`; collector weak/ephemeron clearing is null-positioned. |
| `gc` | fixed payload fields `id` through `src_pos` are null | `op` carries replayable collector control evidence. Its additive fields are defined below. |

Strings in `kind`, `reason`, `check`, `object_kind`, and `fn` are schema labels or copied
diagnostics, not numeric measurements. Boolean measurements such as `hit` are encoded as
integer `0` or `1`.

### Replay evidence and transaction boundaries

The writer never silently imports a changed heap graph. At a heap sample it compares the
read-only physical snapshot with its private logical mirror. Equal IDs must retain their
kind and generation. Changed `size` or `refs` produce one `update` per changed object in
ascending base-slot order. An unexplained birth, death, identity change, or kind/generation
change is an emitter error because only `alloc`, `die`, `relocate`, and `promote` may make
those transitions.

`gc` has the following ordered additive fields:

| `op` | Additive fields | Meaning |
|---|---|---|
| `pause` | `collection_id` | One event per measured stop-the-world entry. `collection_id` is the active logical collection or null when the pause precedes its begin event. |
| `forward` | `collection_id`, `from_id`, `to_id`, `owner_id`, `forward_kind` | One event per unequal-ID rewrite counted by `forwarded_reference_count`. `from_id -> to_id` must be the direct full-ID mapping already established by a `relocate`/`promote` event. `forward_kind` is `heap` for a snapshot-visible object field, `root` for a precise or mutator-local root slot, and `registry` for collector-owned IDs not present in snapshots. `owner_id` is the exact live owning object for `heap`; it is null for `root` and `registry`. |
| `collection_begin` / `collection_end` | `collection_id`, `collection_kind`, `live_bytes`, `live_objects` | Balanced logical collection boundaries. IDs start at zero and increase by one at the exact major/minor metric increment. Marking-to-compaction remains one major collection. Boundary counts describe the replay mirror. |
| `move_begin` / `move_end` | `transaction_id`, `parent_transaction_id`, `depth`, `cause`, `collection_id`, `live_bytes`, `live_objects` | Balanced, nestable death-accounting/movement transactions. Transaction IDs start at zero. Parent is null at depth one. |

`collection_kind` is `major` or `minor`. Movement `cause` is one of
`atomic_major`, `atomic_minor`, `incremental_death_accounting`,
`incremental_compaction_step`, `incremental_compaction_finalize`,
`incremental_mark_compact`, `map_growth`, or `builder_growth`.

Cause labels are semantic, not decorative. `atomic_major` is inside a major collection,
`atomic_minor` inside a minor collection, and every `incremental_*` cause inside a major
collection. Atomic and `incremental_mark_compact` transactions may account deaths and
compact; `incremental_death_accounting` may account deaths but cannot move;
`incremental_compaction_step` may compact but cannot account deaths; and
`incremental_compaction_finalize` may reconcile references but cannot move or account
deaths. A Map/Builder growth transaction contains exactly one `growth` relocation of the
matching object kind and no death or promotion. Changed-base promotion is represented by
an immediately preceding identical `relocate`; a promotion without that pair is in place
and keeps the same full ID. Deaths are strictly ascending by source base within each
logical collection.

Within every move transaction, allocation and descriptor-width change are forbidden.
Every opening object either has an explicit `die` or resolves one-to-one through its full
ID forwarding chain to an ending object with the same kind and size. Generation may only
remain equal or change from young to old. Therefore:

```text
end live bytes   = begin live bytes - explicit die bytes
end object count = begin object count - explicit die count
```

Relocation and promotion contribute zero bytes and zero objects. A Map/Builder growth
transaction brackets only the identity-preserving relocation and reference rewrites; its
later width/ref change is a separate `update`. `die` removes only the named object from
replay. Incoming weak/ephemeron clearing and surviving-owner edge changes must be explicit
`update` events before the transaction ends.

For `heap` forwarding, replay derives an obligation for each snapshot-visible reference
slot, owning object, and direct `from_id -> to_id` mapping, then requires the exact
per-edge multiplicity of `gc` `forward` events. Atomic, mark-compact, and Map/Builder
growth obligations must be satisfied when their movement transaction ends. Incremental
compaction-step obligations may remain open across steps because physical field rewriting
occurs at finalization, but must be satisfied by logical collection end. An obligation is
removed without a forward only when its owning object has an explicit `die`, or when an
explicit `update` removes that edge before the applicable accounting boundary. An
already-observed rewrite remains accounted when a later mutator update overwrites the
slot. Dead weak targets and inactive ephemeron key/value pairs are therefore explicit
clears, not forwarding. Root and registry forwarding remain individually serialized and
contribute to the exact stats total without being miscounted as visible heap edges.
Replay requires every opaque root/registry event to name an established direct movement
mapping and checks its classification total, but does not derive per-location
root/registry multiplicity because root stacks and collector registries are intentionally
outside the snapshot graph required by T2.

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
program exit. `stats.json` records `N` as `snapshot_interval`; the default is 4096. A
snapshot's `seq` names the next event to apply: the graph is the state immediately before
event `seq`. Start is therefore `seq: 0`; an exit snapshot may intentionally repeat a
periodic `seq` when the run ends on that boundary. The emitter's single exit line fulfills
both roles; readers also accept a periodic line immediately before the exit line at the
same final `seq`. In that two-line form, the periodic tick is the last event tick and the
exit tick is the final retired-instruction count.

Snapshot material comes from `Heap::trace_snapshot`, a read-only ascending-slot walk
using the production descriptor visitor with metric accounting disabled. While a
collection has produced logical death/movement observations but has not yet installed
its physical result, the writer advances a private observer mirror from that read-only
walk. Collection transaction boundaries reconcile it against another read-only walk and
emit any required `update`; no graph state is imported silently. The mirror is host-only
trace state: it cannot mark, forward, retain, or influence a heap object.

### Seeking and replay

To render event `S`:

1. choose the snapshot with the greatest `seq <= S`;
2. load its `live` list as the complete graph immediately before that sequence;
3. replay events beginning at the snapshot's `seq` and ending at `S`;
4. remove only the named object on `die`, add it on `alloc`, rewrite IDs and all matching
   references on `relocate`/`promote`, replace size/refs on `update`, and use `gc`
   boundaries to check conservation and post-move reference resolution.

Following `to_id` chains is sufficient to preserve logical identity through repeated
movement. Renderers must not infer identity from a base slot alone.

## Statistics

`stats.json` is one JSON object with keys in this order:

```json
{"live_bytes_final":0,"forwarded_reference_count":0,"forwarded_reference_totals":{"heap":0,"root":0,"registry":0},"pause_slices":0,"collection_count":0,"event_totals":{"alloc":0,"mark_slice":0,"relocate":0,"promote":0,"die":0,"intern":0,"evict":0,"trap":0,"verify_step":0},"snapshot_interval":4096,"ticks":0,"peak_live_bytes":0}
```

- `live_bytes_final`: sum of final live descriptor widths multiplied by 8.
- `forwarded_reference_count`: actual old-ID-to-new-ID rewrites performed in precise
  `Value` slots and collector-owned reference registries/worklists. Equal-ID rewrites and
  clears are not counted. It equals the number of `gc` `forward` events.
- `forwarded_reference_totals`: the same exact count partitioned into `heap`, `root`, and
  `registry` events. This second ledger detects event classification drift even when the
  aggregate event count is unchanged.
- `pause_slices`: observer count of explicit stop-the-world entries: atomic collections,
  incremental cycle starts, marking/compaction budget steps, final liveness pauses, and
  compaction finalization. It equals the number of `gc` `pause` events.
- `collection_count`: the run-local delta of
  `HeapMetrics.major_collections + HeapMetrics.minor_collections` from program start to
  exit. Incremental major cycles use the same existing major counter. It equals the
  number of `gc` `collection_begin` events.
- `event_totals`: exact emitted count for the original nine required runtime kinds
  (`alloc` through `verify_step`), including zeros. Replay-only `update` and `gc` counts
  are deliberately derived from `events.jsonl` rather than duplicated here.
- `snapshot_interval`: positive event cadence used to prove that no periodic seek point
  was deleted.
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
ordered collector registries, writer-local collection/transaction IDs, and module pc
order. The emitter never reads a wall clock, host pointer, random device, thread schedule,
or unordered container.
