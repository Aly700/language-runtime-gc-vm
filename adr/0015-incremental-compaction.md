# ADR 0015: Deterministic Incremental Compaction

## Status

Accepted for Iteration 38.

## Decision

Major compaction may relocate objects between VM instructions. Liveness remains a
deterministic atomic boundary: a compaction-only cycle performs major marking and the
ephemeron fixpoint before movement, while a combined cycle runs Iteration 37 marking to
quiescence and transfers its final-remark live set directly into compaction. Incremental
marking and incremental compaction are mutually exclusive.

Preparation scans survivors in ascending source-slot order and records an immutable entry
containing the complete source `ObjectId`, source slot, descriptor width, complete
destination `ObjectId`, and destination slot. It decides dead weak targets and inactive
ephemerons, sweeps dead headers, and clears survivor mark bits before the mutator resumes.
The ordinary storage-layout, remembered-set, weak-target, ephemeron, root, and heap-field
validators therefore remain meaningful at preparation and every later boundary.

A compaction budget counts complete survivor relocation units. One unit validates the
saved width, copies one complete object, promotes it if young, installs the precomputed
destination generation, publishes forwarding, rewrites precise roots and collector owner
registries, records any promotion-created old-to-young edge, and validates. A zero budget
moves nothing. Budgets never count bytes, fields, time, allocations, addresses, or host
container operations.

The checked `ObjectId` dereference funnel is the phase-local read barrier. It first accepts
a physically current `(slot, generation)` header. Otherwise it accepts only a complete
source ID saved by the active plan whose forwarding entry has already been installed, and
then validates the complete destination ID. Slot-only matches are forbidden. Thus a stale
generation continues to trap before, during, and after compaction, while a current-cycle
source ID resolves only until final canonical rewriting discards the forwarding state.

Objects slide in place in ascending source order. Every destination is at or below its
source and covers only already-scanned storage or the current source run, so copying to a
temporary `Object` before clearing the source prevents overwrite of an unprocessed
header. Descriptor widths are immutable during the phase. Allocations, new map entries,
and explicit collections finish compaction first with their operands registered as
temporary roots. Fixed-width pair, scalar-array, reference-array, existing-map-value,
record, and ephemeron mutations remain legal; reference stores canonicalize targets before
the existing marking and generational barriers publish them.

After each move, VM frame stacks and locals (active and suspended), frame closures,
`pending_exception_`, embedder root providers, handles, and temporary roots are partially
rewritten. Heap fields may retain exact source IDs and resolve through the barrier until
the final descriptor-driven rewrite. Weak and ephemeron owner registries are forwarded in
physical slot order. Moved young owners are promoted immediately, and the remembered set
conservatively records their edges to unmoved young peers until final pruning.

Completion runs an independent differential oracle. Preparation snapshots the
post-liveness source heap and generations. Every legal mutation updates only the
corresponding field in this source-positioned shadow after inverse full-ID normalization.
The oracle independently recomputes an atomic ascending slide, generations, forwarding,
descriptor fields, weak/ephemeron fields and registries, roots, and final payloads without
using production destinations or installed forwarding. The first mismatch traps. A
test-only scalar corruption that passes ordinary structural validation proves this oracle
is non-vacuous.

The new schedules are append-only: `incremental_compact_1`,
`incremental_compact_3_1`, and `combined_mark_compact`. Empty compaction budgets keep all
legacy paths unchanged. Scheduling uses only instruction counts, positive integer object
budgets, ordered vectors, and descriptor order; there is no clock, thread, randomness, or
unordered iteration.

## Rejected alternatives

- A permanent stable-handle table was rejected because it weakens stale-generation traps,
  adds an always-paid indirection, and hides missed root rewrites.
- A separate to-space heap was rejected because simultaneous source/destination heaps
  require a broader selector on every access and a second identity transition at install.
- Incremental liveness decisions were rejected for this iteration. Atomic liveness plus
  mutually exclusive phases is smaller and makes weak/ephemeron death final before
  relocation.
- Byte, field, and wall-clock budgets were rejected because descriptor shape or machine
  timing would change replay boundaries.
