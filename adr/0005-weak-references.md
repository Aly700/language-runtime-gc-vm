# ADR 0005: Process weak references in a deterministic post-mark phase

## Status

Accepted.

## Decision

Weak references use `ObjectKind::WeakRef`, a fixed-width heap object with one immutable
creation-time target slot. Its descriptor visits zero strong fields. Strong tracing
remains exclusively descriptor-driven, while an exact heap-owned registry of WeakRef
owner IDs is the exclusive authority for weak edges. No third object-payload reference
path is permitted.

The registry is duplicate-free and ordered by ascending heap slot. It contains every live
WeakRef owner and no other object, is rewritten and pruned across movement, and never acts
as a root. Major and minor collection first finish marking and build the forwarding table.
After roots and descriptor-declared strong fields are rewritten, but before the new heap is
installed, the weak phase visits registry entries in old-slot order. A live target with a
forwarding entry is rewritten; a target without one is cleared to canonical `Nil`. Nil
remains nil forever, so generation-tagged slot reuse cannot resurrect a cleared weak
reference. Map-growth relocation invokes the same weak forwarding hook.

Minor collection does not place weak edges in the remembered set. The registry directly
finds old WeakRefs: an unmarked young target clears, while a young target marked through a
strong root promotes and is forwarded. `validate_weak_targets` runs at collection and
relocation boundaries and traps on registry mismatch, non-reference targets, stale IDs,
duplicates, or non-slot order.

Bytecode adds append-only `AllocWeak` and `WeakGet` operations. Verification represents
`weak<T>` structurally and models `WeakGet` as a maybe-reference retaining the exact `T`
facts. The frontend exposes `weak<T>`, `weak(x)`, and `w.get()`. `T` must be an object type
(`pair`/named pair, array, map, `str`, or `fn`), construction requires a proven non-nil
object, and the get result must pass through the existing local `is_nil` refinement before
object use. `WeakIsAlive` is omitted so nil observation has one flow-sensitive rule.

## Consequences

- Weak targets never extend liveness, run a write barrier, or enter the remembered set.
- WeakRef objects themselves are ordinary strongly referenceable objects and may appear in
  pairs, reference arrays, map values, and closure captures.
- Weak clearing is deterministic for a fixed GC schedule. Different schedules may
  legitimately differ when one collects after the last strong reference and another does
  not; tests pin those outcomes explicitly.
- Existing object kinds, legacy fuzz generators, stack-map root bits, benchmark workloads,
  and strong descriptor traversal retain their previous ordering and representation.

## Deferred work

Weak maps, finalizers, resurrection, ephemerons, mutable weak targets, and user-visible
finalization ordering are out of scope. Each requires a separate liveness model and cannot
reuse this one-slot weak phase implicitly.

## Rejected alternatives

Scanning WeakRef targets through descriptors was rejected because it would make them
strong. Treating target bits as opaque scalars was rejected because moving collection must
forward surviving targets and clear dead ones. A remembered-set-only design was rejected
because weak edges must not keep young objects alive. Unordered registries were rejected
because visitation and diagnostics would become host-order dependent.
