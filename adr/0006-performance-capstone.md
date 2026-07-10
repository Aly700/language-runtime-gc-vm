# ADR 0006: Measure every object kind and optimize first-fit occupancy discovery

## Status

Accepted.

## Context

Iterations 20–26 added variable-sized scalar/reference arrays, immutable strings,
closures, insertion-order maps, and weak references after the iteration-16 benchmark
baseline and iteration-19 verifier optimization. The original five workloads therefore
did not measure the new descriptor, movement, lookup, growth, or weak-processing paths.

Iteration 27 first adds five deterministic workloads and passive work counters. Phase A
changes no runtime branch or representation. The clean revision and post-instrumentation
tree produce byte-identical dumps for the nine legacy fuzz corpora plus the iteration-26
weak corpus, byte-identical original fourteen counter values for every pre-existing
workload, and the same 23/23 green CTest gate.

## Measurement

In the assertions-enabled Debug build, seven-run medians for the new workloads were
21.343ms (`string_heavy`), 120.740ms (`closure_heavy`), 1580.233ms (`map_heavy`),
1.462ms (`weak_heavy`), and 14.393ms (`mixed_graph`). `map_heavy` performed
306,009,624 storage-occupancy header examinations. The next-largest new workload
performed 2,388,789. By contrast, `map_heavy` performed only 41,809 map-key probes and
481,745 map descriptor-entry scans.

The cause is the deterministic first-fit allocation search. For every candidate base and
every slot in the requested run, `find_free_storage_run` calls `is_storage_slot_free`;
that helper scans every object header to decide whether the single slot falls inside a
variable-width object. The same unchanged heap layout is therefore rediscovered hundreds
of millions of times.

## Decision

Optimize only occupancy discovery inside `find_free_storage_run`. Walk current object
headers once in ascending slot order, advance by each descriptor width, and count free
slots between occupied intervals until the first sufficiently long run appears. This
preserves the exact allocation result: both algorithms choose the lowest base whose
complete logical storage interval is outside every live object's
`[base, base + storage_slot_count)` interval.

`is_storage_slot_free` itself remains unchanged for map-growth adjacency checks and the
allocator's post-selection overlap assertions. `validate_heap_storage_layout`,
`validate_remembered_set`, `validate_weak_targets`, descriptor validation, all barriers,
all assertions, and all traversal/diagnostic order remain unchanged and execute at the
same points.

## Rejected alternatives

- Removing or reducing full-heap validator calls was rejected even though their counters
  are nonzero. That would weaken the invariant boundary, and validator work is not the
  dominant measured cost.
- Hoisting `checked_map`/descriptor validation out of map stores was rejected because it
  would remove repeated invariant checks from the mutation funnel.
- Changing linear map semantics or adding hashing was rejected because lookup probes are
  not dominant and ADR-0004's deterministic structural-key model remains appropriate.
- Short-circuiting closure descriptor/capture scans was rejected because the capture-map
  precision property must be checked and visited exactly.
- Weak-registry indexing was rejected because the full weak workload takes 1.462ms and
  processes only 577 targets.

## Equivalence argument

The optimization changes neither the set of occupied logical slots nor first-fit ordering.
It changes only how often that immutable occupancy fact is rediscovered during one
allocation search. The selected run is still checked afterward by the existing
`is_storage_slot_free` assertions before publication. Object IDs, allocation order,
generations, collection schedules, descriptor order, map insertion order, weak registry
order, stack maps, diagnostics, barriers, and all observable counters that predate
iteration 27 therefore remain unchanged.

## Outcome

On `map_heavy`, candidate slots examined fell from 1,767,785 to 636,870,
requested-run slots checked fell from 1,771,813 to 6,900, and storage-occupancy header
examinations fell from 306,009,624 to 8,002,102. The seven-run median fell from
1580.233ms to 84.943ms, a 94.6% reduction.

The first implementation experiment materialized a temporary occupancy bitmap. It
removed the map rescan but regressed dense pair allocation because every allocation paid
for an extra buffer and full pass. That experiment was rejected and removed. The accepted
interval sweep needs no buffer and keeps the legacy runtime workload medians flat or
favorable in the final capture.

Every deterministic counter outside the three allocator-work counters is byte-identical
to the Phase-A capture. The original fourteen counters for all five pre-existing
workloads, all ten corpus dumps, verifier diagnostics, generated stack-map assertions,
and pinned snapshots remain covered by the final double CTest gate.
