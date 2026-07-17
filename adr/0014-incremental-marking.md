# ADR 0014: Deterministic Incremental-Update Marking

## Status

Accepted for Iteration 37.

## Decision

Major marking may run as a heap-owned tri-colour cycle interleaved with VM instruction
boundaries. Each step receives a positive integer budget and consumes at most that many
complete grey-object descriptor scans. Budgets never depend on time, bytes, field count,
host addresses, or threads. The existing deterministic LIFO worklist and descriptor field
order define replay exactly.

Mutator stores use an incremental-update insertion barrier. Every mutable strong
publication already passes through a heap funnel and performs barriers before publish;
when such a funnel sees a black owner publish a white object, it shades the target first.
Grey owners need no shade because their later descriptor scan observes the edge. Objects
allocated during a cycle enter the same grey worklist, so immutable constructor payloads
need no special tracing path. A validator scans only descriptor-declared edges and rejects
black-to-white; a barrier-elision test proves that check is non-vacuous.

The final pause validates tri-colour safety and recomputes atomic liveness from the current
precise roots. This differential final remark removes floating garbage after edge deletion
and guarantees the selected live set matches stop-the-world major marking at the same
boundary. It then drains the Iteration 36 ephemeron fixpoint. Weak targets and inactive
ephemerons are decided and cleared only after that final liveness decision.

Compaction, forwarding, root/field rewriting, weak/ephemeron processing, installation,
and post-collection validation remain stop-the-world. Moving objects concurrently would
require read barriers or stable indirection and would contradict the current `ObjectId`
capability rule. Map-growth relocation is the one existing movement-only transaction and
now forwards the incremental grey worklist alongside every other collector-owned ID.

Incremental budgets are a first-class stress dimension (`incremental_1` and
`incremental_3_1`) and participate in the combined schedule. Explicit atomic major or
minor collection first completes an active incremental major cycle. VM execution resets
the budget cursor and cannot return with an active cycle.

## Rejected alternatives

- SATB was rejected because it requires deletion barriers and an old-value log absent from
  the existing barrier-before-publish architecture.
- Wall-clock slices were rejected because identical executions would not replay exactly.
- Field or byte budgets were rejected because descriptor shape would change scheduling.
- Incremental compaction was rejected because it requires a read barrier or indirection.
- Mid-cycle weak or ephemeron clearing was rejected because an intermediate white state is
  not a final death decision.
