# ADR 0024: Observer-Only Deterministic Heap Tracing

## Status

Accepted for showcase instrumentation milestone T1; replay-evidence amendment accepted
for milestone T2.

## Context

The showcase renderer needs an allocation, liveness, movement, interning, trap, and
validation history from the precise moving generational collector. Instrumentation at
the wrong layer could become a third edge category, perturb stress scheduling, alter
metrics, retain weak targets, or make output depend on host I/O timing. Any of those
would invalidate the repository's existing schedule-differential proof.

ADR-0021 already establishes the safe source-coordinate model: copied function names and
line/column leaves are verifier-inert plain data. ADR-0013, ADR-0014, ADR-0015, and
ADR-0020 establish deterministic descriptor order, integer work budgets, movement IDs,
and weak-category processing. The emitter must observe those authorities rather than
create parallel ones.

## Decision

`lang::TraceSink` is an optional public one-way observer installed on both `VM` and
`gc::Heap`. Both constructors default to null. A null check and callback at an existing
funnel is the only collector branch introduced for tracing. Runtime code never reads a
sink result, trace counter, file state, snapshot, or event history. No trace value enters
a root, mark bit, worklist, forwarding table, remembered set, weak registry, ephemeron
registry, intern table, allocation search, stress trigger, or VM output buffer.

The VM publishes the current deterministic retired-instruction count and the ADR-0021
pc position before instruction-boundary collector work and dispatch. Mutator allocation,
interning, and terminal trap callbacks copy that context. Collector callbacks deliberately
write a null source position.

The heap emits only at existing authorities:

- the single `allocate_object` funnel for all object kinds;
- incremental budget-step functions;
- ascending-slot atomic compaction and incremental-compaction plan entries;
- the liveness-end dead-object scans;
- weak intern lookup and weak-processing eviction;
- the existing validator functions; and
- the VM's existing terminal failure catch boundary.

Map and Builder width-growth relocation also emits the same ID transition because those
paths already forward the complete runtime graph. Omitting them would leave a renderer
with an unexplained stale ID.

Snapshots use `Heap::trace_snapshot`, an ascending-slot, read-only descriptor walk. It
passes no `HeapMetrics` accumulator to descriptor validation and canonicalizes only in
host trace data. WeakRef and Ephemeron fields are copied from their collector-owned slots
without marking them. To place a periodic snapshot between logical collection events
before the production heap is installed, the JSONL writer owns a private mirror seeded
and reconciled against that walk. Reconciliation emits explicit `update` evidence rather
than silently importing graph state. The mirror is not accessible to the heap and cannot
affect liveness.

T2's strict replay found that private mirror resynchronization alone could conceal a
mutator field write or Map/Builder width change between periodic snapshots. The writer
therefore diffs each authorized heap sample against its mirror and emits a complete
post-state `update` for every changed object in ascending base-slot order. Identity,
kind, and generation drift remain lifecycle errors; they cannot be imported by an
update. Pair, RefArray, Map, Record, Ephemeron, and Builder mutation funnels sample after
publish and before any stress-triggered collection.

The pause and unequal-ID forwarding observations now emit graph-neutral `gc` events as
well as incrementing stats. Forward events carry the direct full-ID source/destination
mapping plus the live owner for snapshot-visible heap fields, and classify those fields,
precise/mutator-local roots, and collector registries. Replay can therefore require exact
per-owner/per-mapping visible-edge forwarding, while allowing only explicit owner death
or weak/ephemeron clearing to cancel an unperformed visible rewrite, without treating
opaque registries as heap edges. Logical collection begin/end events align with the actual
major/minor metric increments, while separately numbered movement transactions bracket
atomic movement, incremental death accounting/steps/finalization, and Map/Builder growth
relocation. These observations remain one-way: no runtime path reads their IDs, labels,
counters, or serialized state.

`stats.json` records the positive snapshot interval so a checker can prove seek-point
cadence rather than only validating lines that remain. Its collection count is the
major-plus-minor metric delta from `on_program_start` to exit, which keeps trace evidence
run-scoped even when an embedder attaches a writer to a previously used heap. Forwarded
reference totals are also partitioned by heap/root/registry classification, providing a
second deterministic ledger for the event classification.

These ledgers establish deterministic internal consistency, not cryptographic
provenance. Authenticating a bundle against a party able to rewrite every file requires
an externally trusted signature or digest and is deliberately separate from the runtime
observer contract.

T2 derives exact forwarding obligations for snapshot-visible heap fields. Root and
registry observations are opaque evidence: replay validates their direct movement
mapping and classification totals, but does not reconstruct VM root locations or
collector-registry membership, which are outside the snapshot graph contract.

The JSON writer uses only ordered vectors/maps and fixed field order. `tick` is the VM
counter, `seq` is a writer-local monotonic index, object IDs are the existing complete
`ObjectId` values, and every unit is an integer. Host pointers, wall clocks, random
sources, threads, and unordered iteration are forbidden.

## Protected invariants

This decision preserves the following existing contracts:

- **Schedule determinism:** sink state never participates in allocation, collection,
  incremental budgets, or stress triggers.
- **No behavior divergence:** the VM result, output channel, runtime diagnostic, and all
  `VMMetrics`/`HeapMetrics` counters are independent of sink presence.
- **Two exhaustive edge categories:** snapshots copy strong descriptor fields and the
  existing weak-category fields but introduce no root or edge.
- **Precise moving roots:** tracing observes complete pre/post IDs at the existing
  forwarding funnels and never stores an ID back into runtime state.
- **Weak and ephemeron semantics:** observation cannot retain, activate, resurrect, or
  delay clearing a target.
- **Diagnostic neutrality:** source positions reuse copied ADR-0021 metadata and remain
  absent from `trace_roots`.
- **Output determinism:** the writer receives no output reference; `lang_trace` copies
  the VM's existing byte channel to stdout only after execution.
- **Replay accountability:** every snapshot-visible graph field changes through `alloc`,
  `die`, `relocate`, `promote`, or `update`; every stats pause, forwarding rewrite, and
  logical collection increment has one countable `gc` event.

The only enabled-run failure newly possible is a host trace-output error (for example a
full disk). That error is an embedder I/O failure, not a collector decision. With a valid
sink, runtime language behavior remains identical.

## Proof strategy

`trace_transparency` compiles fixture programs covering pairs, scalar/reference arrays,
strings, closures, maps, records, variants, caught and uncaught typed exceptions, runtime traps, weak
references, ephemerons, weak interning/eviction, and Builder. It executes each program
with the sink absent and installed under all fifteen shared deterministic schedules. It
requires equal success/trap outcomes, VM output bytes, canonical result graphs, ADR-0021
runtime traces, and complete `VMMetrics`/`HeapMetrics` values.

`trace_determinism` executes the same compiled fixture twice under `no_stress`,
`major_every_1`, and `combined_mark_compact`, requiring byte equality for
`events.jsonl`, `snapshots.jsonl`, and `stats.json`. It also checks fixed event-key order
and LF-only framing.

The final proof is the untouched legacy gate: every pre-existing correctness test and all
pinned corpus/hash assertions run with the tracing code linked but the default null sink.
They must remain green without changing an existing test source or pin.

T2 adds a stdlib-only continuous replay checker. It verifies every snapshot without
rebasing, proves byte/object conservation at every numbered movement transaction,
resolves all references at movement and logical-collection completion, and cross-checks
stats against serialized evidence. A separate self-test mutates real generated bundles
in temporary directories and requires classified rejection of every corruption.

## Consequences

- Embedders pay no trace allocation or serialization cost unless they install a sink.
- Enabled tracing may allocate host memory and perform synchronous host file I/O, but it
  allocates nothing in the language heap.
- Object identity in a trace is a movement chain, not a permanent numeric ID.
- Enabled traces include explicit object-state and GC-control evidence. This increases
  event volume but makes collection statistics and snapshots independently replayable.
- Exact schema and replay rules live in `SCHEMA.md` and can evolve additively after T1.

## Rejected alternatives

- Collector callbacks that return policy or budget values were rejected because they
  would let observation influence behavior.
- Stable host pointers or a permanent identity table were rejected because they violate
  moving-ID and process-determinism contracts.
- Treating snapshots as roots was rejected because it would retain weak and dead objects.
- Rewalking unordered host containers was rejected because byte determinism would depend
  on implementation order.
- Wall-clock timestamps and time-budgeted slices were rejected because identical runs
  would not replay byte-for-byte.
- Inferring source locations after execution was rejected in favor of the existing
  ADR-0021 current-pc table.
