# ADR 0007: Lower for-in to index loops with precise hidden locals

## Status

Accepted.

## Decision

The language adds three statement-only forms: `for x in array`, `for k, v in map`, and
`for i in lo..hi`. No iterator object or heap kind is introduced. The compiler lowers each
form to existing local loads/stores, comparisons, conditional branches, and backedges. It
allocates ordinary hidden locals for the induction index and fixed bound and, for container
forms, a snapshot of the container reference. Internal local names contain characters the
lexer cannot produce, preventing source capture. User loop bindings are immutable and
lexically scoped; assignment reports a positioned type error. Closures created in the body
reuse capture-by-value lowering, so each closure snapshots that iteration's value.

Array and map source expressions are evaluated once. Array length is snapshotted at entry,
but each element is loaded through the live container reference at its step. Element writes
are therefore visible, while the trip count stays fixed. The hidden array reference and any
current reference element are ordinary precise roots; moving collection must forward both.
Nullable containers, including `WeakGet` results, require the existing false-branch
`is_nil` refinement before they may enter this lowering.

Range bounds are evaluated once, lower then upper. The lower result initializes the hidden
induction local and the upper result initializes the hidden bound. The emitted comparison
implements half-open `[lo, hi)` semantics, including zero iterations for equal or inverted
bounds. Body arithmetic and assignments to the original bound variables cannot change the
trip count.

Map iteration is positional and follows ADR-0004 insertion order, making that ordering an
observable language guarantee. `MapKeyAt` and `MapValueAt` are the only new opcodes. Each
consumes a typed map and `i64` index; the verifier derives its exact result type from the
map's structural layout. The heap already owns checked positional access, so the opcodes add
no new payload API, representation, tracing path, barrier, or collector behavior.

Map entry count is snapshotted at entry. At every step the lowered loop compares current
count with the snapshot. Updating an existing key does not change count or insertion
position and the current value is read when its position is reached, so the update is
visible. Inserting a new key grows the count; because maps have no deletion operation, that
is the only mismatch. Lowering deliberately performs an invalid positional access, which
traps with the stable `map entry index out of bounds` diagnostic. New entries are never
visited silently.

Generated per-pc maps include exact operand-stack and local reference bits. Local root
category is independent of definite initialization: a hidden reference local can be `Nil`
on the entry edge and an object on a backedge without becoming readable before its store.
The VM asserts both maps at instruction boundaries around stress collection. This makes the
container snapshot a loud precise-root boundary rather than relying only on dynamic tags.

## Consequences

- For-in adds no object descriptor, allocation path, strong or weak edge category, write
  barrier, remembered-set rule, forwarding rule, or collection phase.
- Insertion order is source-visible and remains deterministic across moving GC schedules.
- Type checking uses the same fixed-point body join as `while`; loop-local scope does not
  leak into following source statements, and nested loops receive distinct local slots.
- Compiler-produced loops remain subject to ordinary jump, initialized-local, type,
  compile-boundary agreement, and exact stack-map checks.
- The iteration-28 fuzzer is isolated from all legacy generators and provides pinned source,
  schedule replay, nesting, and positioned mutant classes.

## Deferred work

`break` and `continue` are future work. They require explicit lowering rules for cleanup,
backedge targets, fixed-point flow, and stack-map boundaries and are not inferred from the
current statement forms.

## Rejected alternatives

Heap-allocated iterator objects were rejected because they add an object kind, descriptor,
allocation/forwarding behavior, and new roots for no semantic benefit. Host iterators or
pointer cursors were rejected because movement and map-growth relocation would invalidate
them. Re-evaluating sources or bounds on every step was rejected because it changes side
effects and trip counts. Iterating newly inserted map keys was rejected because it makes
termination body-dependent; silently ignoring them was rejected because it hides mutation.
