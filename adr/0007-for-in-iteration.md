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

Iteration 30 adds unlabeled `break;` and `continue;` without changing this runtime
representation. Both bind to the nearest lexically enclosing `while` or `for-in`; a use
outside a loop is rejected at the keyword with the stable diagnostic `'break' is only
allowed inside a loop` or `'continue' is only allowed inside a loop`. A lambda is a new
function boundary, so loop control in its body cannot bind a loop surrounding the lambda
expression.

The compiler lowers both statements to ordinary `Jump` instructions through a stack of
loop patch contexts. Break targets the first instruction after its loop. While-continue
targets the condition header. Range-, array-, and map-continue target the four-instruction
hidden-index increment preamble (`LoadLocal`, `ConstantI64 1`, `AddI64`, `StoreLocal`),
whose backedge then reaches the loop header. The map header performs its entry-count
mutation check before the next bounds check or positional read, so continue cannot bypass
the insertion trap. A literal-`true` while omits the condition and false edge entirely;
this lets its post-loop state be derived solely from reachable breaks.

Type flow keeps the existing `FlowState` value and initialization lattice. A block has an
optional fallthrough state: break contributes its incoming state to the loop-exit set,
continue contributes its incoming state to the backedge set, and either ends that path.
Statements lexically following an unconditional terminator are accepted but excluded from
flow joins and bytecode emission. `if` joins only branches that fall through. At a loop
fixed point, body fallthrough plus continue states join into the header; break states plus
the ordinary condition/trip-count exhaustion state join after the loop. For-in always has
the zero-iteration exhaustion edge. A literal-true while has no exhaustion edge, so a
value assigned or refined on every break path remains initialized/refined afterward,
while a missing assignment on any break path is conservatively rejected by the existing
lattice.

The verifier receives no loop-specific rule or reason code. Every new edge goes through
the existing target validation, stack-height equality, definite-local intersection,
abstract-value join, reachability check, fixpoint worklist, and generated-stack-map path.
Hidden loop locals keep their iteration-28 slots and reference categories. Consequently a
collection at a break-exit pc or continue-increment pc sees the exact same ordinary local
root vector the verifier proved for that target.

## Consequences

- For-in adds no object descriptor, allocation path, strong or weak edge category, write
  barrier, remembered-set rule, forwarding rule, or collection phase.
- Insertion order is source-visible and remains deterministic across moving GC schedules.
- Type checking uses the same fixed-point body join as `while`; loop-local scope does not
  leak into following source statements, and nested loops receive distinct local slots.
- Compiler-produced loops remain subject to ordinary jump, initialized-local, type,
  compile-boundary agreement, and exact stack-map checks.
- The iteration-28 fuzzer remains isolated from all legacy generators. Iteration 30
  deliberately updates only its pin to mix both loop-control statements with all loop
  forms, heap-graph and output oracles, schedule replay, nesting, and positioned mutants.

## Deferred work

Labeled loops and labeled `break`/`continue` remain future work. Adding them requires an
explicit label namespace and target-resolution rules; unlabeled nearest-loop binding is
not generalized implicitly.

## Rejected alternatives

Heap-allocated iterator objects were rejected because they add an object kind, descriptor,
allocation/forwarding behavior, and new roots for no semantic benefit. Host iterators or
pointer cursors were rejected because movement and map-growth relocation would invalidate
them. Re-evaluating sources or bounds on every step was rejected because it changes side
effects and trip counts. Iterating newly inserted map keys was rejected because it makes
termination body-dependent; silently ignoring them was rejected because it hides mutation.
