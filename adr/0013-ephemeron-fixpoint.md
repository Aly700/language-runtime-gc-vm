# ADR 0013: Ephemeron Fixpoint Marking

## Status

Accepted for Iteration 36.

## Decision

The heap provides fixed-width single-entry ephemerons with an immutable object key and a
mutable scalar-or-reference value. The object descriptor exposes neither slot. An exact
registry in ascending heap-slot order is the collector authority for the weak key and
conditional value.

After ordinary strong marking, collection repeatedly scans the registry in order. A live
owner activates its value only when its key is independently live: major collection
requires the key's mark bit; minor collection accepts an old key or a marked young key.
Each productive scan marks at least one previously unmarked collectible object, and mark
bits never revert. A finite heap therefore permits at most one activation per collectible
object before a final no-progress scan. The algorithm is deterministic and terminating.

Fixpoint marking precedes compaction. Movement forwards live owners and active keys/values;
entries with inactive keys become canonical nil/nil. Movement-only relocation rewrites
the same registry without making a new liveness decision. Cleared entries cannot
resurrect when a slot is reused.

All value mutation goes through `Heap::store_ephemeron_value`. It validates the retained
value category, performs an old-to-young barrier before publication, and then stores. Keys
are immutable and weak, so they never enter descriptor traversal or the remembered set.

## Rejected alternatives

- A weak-key map adds equality, lookup, resizing, iteration, and deletion semantics that
  do not strengthen the collector proof.
- A single registry pass is incorrect for chains whose activated value is a later key.
- Treating the key as a descriptor edge incorrectly extends key liveness.
- Finalizers or callbacks introduce resurrection and ordering behavior outside this scope.
