# Architecture — Language Runtime

## Pipeline

```text
source -> lexer -> parser -> AST -> type checker -> compiler -> verifier -> VM -> heap/GC
```

## Frontend surface

`lang::frontend::compile_program` is the narrow public entry point. It returns either a
`lang::Function` with verifier-generated stack maps attached, or deterministic diagnostics
with byte offsets plus 1-based line/column positions.

The source language is intentionally small:

- Types are `i64`, `bool`, and monomorphic `pair`.
- Top-level `let name: type = expression;` declarations introduce initialized locals.
- Assignment supports locals and `pair` fields (`left` and `right`).
- Expressions include i64/bool literals, variables, `+`, `<`, `pair(left, right)`, field
  access, and parentheses.
- `if condition { ... } else { ... }` and `while condition { ... }` are statement forms.
- A program ends with a final expression, which becomes the VM result.

Minimal cuts: there are no functions, parameters, nil literals, strings, expression-valued
blocks, user-defined types, parametric pair types, or block-local `let` declarations. Pair
is monomorphic at the local type level, but the type checker tracks pair field types
flow-sensitively by allocation site. This keeps cycles expressible by initializing a field
with any pair and later storing another pair into it, while still rejecting reads from a
field whose possible allocation sites disagree on field kind.

Rejected alternative: expose a parametric `pair<T, U>` surface. That would make simple
field reads obvious, but it would either reject useful cyclic structures outright or require
recursive type syntax that is larger than this phase needs. The current allocation-site
field lattice is closer to the bytecode verifier and keeps the agreement boundary small.

## Runtime scaffold

- `lang::Function` stores bytecode and optional stack maps.
- `lang::verify` is the bytecode safety gate. It runs a worklist dataflow analysis over
  reachable bytecode, proving stack depth before every instruction, `LoadLocal`
  initialization over all incoming paths, branch target validity, and value kinds for
  generated stack maps. This protects the VM invariants for stack depth and initialized
  local reads across loops and merge points.
- `lang::VM` owns stack/locals and registers itself as a `lang::gc::RootProvider`.
  Root tracing visits mutable `Value` slots, not copied root values, so a future moving
  collector can update roots before mutator execution resumes.
- `lang::gc::Heap` implements deterministic major and minor mark-compact collection over
  generation-tagged object IDs. The low bits identify the storage slot and the high bits
  identify that slot's current generation, so a swept or moved ID cannot alias a later
  object in the same slot.
- Pair field mutation is routed through `Heap::store_pair_field`. That method is the
  single hook point where the generational old-to-young write barrier runs before the new
  field value is published.
- Deterministic GC stress is configured with explicit counters: before every allocation,
  after every allocation, after every barrier-triggering old-to-young store, every N
  major-collection bytecode instructions, and every N minor-collection bytecode
  instructions. Every stress collection runs the same post-collection reference validation
  as explicit collections. No stress trigger depends on wall-clock time, randomness,
  threads, or host addresses.

## Source/Verifier agreement

Well-typed source must compile to bytecode accepted by `verify_with_stack_maps`. A verifier
rejection of type-checked compiler output is a compiler bug, not a user error.
`compile_program` enforces this by running `verify_with_stack_maps` on every emitted
function, asserting success, then attaching the generated maps and asserting the maps
round-trip through the verifier.

Compiler accommodations for verifier strictness:

- Local initialization: every source `let` has an initializer and compiles to
  `initializer; StoreLocal` before any `LoadLocal` for that local can be emitted.
- Stack discipline: statements are stack-neutral. Field assignment emits receiver then
  value then `SetLeft`/`SetRight`, which consumes both and pushes nothing.
- Bool literals: the bytecode has no Bool constant opcode, so `true` and `false` compile to
  constant i64 comparisons whose result kind is proven `Bool`.
- Branches and loops: conditions must type-check as `bool` because `JumpIfFalse` consumes a
  verifier-proven Bool. `if` emits an explicit jump over `else`; `while` emits a reachable
  header, false exit, stack-neutral body, and backedge.
- No fall-off-end: the final source expression is followed immediately by `Return`.
- Reachability: the compiler emits no bytecode after `Return`, and branch targets are
  patched only to emitted instruction boundaries reachable through verifier dataflow.
- Pair fields: the type checker uses the same allocation-site join idea as the verifier.
  A field can be read only when all possible pair sites agree on the field kind; field
  assignment must preserve that kind.

## Design bias

The heap uses object IDs instead of raw host pointers so movement can be represented as
reference rewriting instead of host pointer patching. An `ObjectId` is a move-sensitive
capability to the current storage location, not stable numeric identity. Language-level
object identity survives movement because the collector rewrites every mutator-visible
reference to the survivor's new `ObjectId` before bytecode execution resumes; stale copied
IDs outside the traced root set remain invalid and trap loudly.

## Moving collection

Collection is mark, forward, rewrite, install, validate:

1. Marking uses an explicit vector worklist. Roots are visited in provider order, pair
   fields are traced left before right, and no unordered container participates in tracing.
2. Compaction scans the slot vector in ascending order and copies marked objects into the
   lowest slots. This deterministic sliding pass builds a forwarding table indexed by old
   slot.
3. A moved survivor receives `new_slot | new_generation << 32`. If it moves into another
   slot, that destination slot's generation is advanced before the new ID is minted, so an
   old stale ID for the destination slot cannot alias the moved object. The old source slot
   is emptied, so the pre-move ID also traps.
4. Before installing the compacted slot vector, the collector rewrites all roots
   (`RootProvider` roots plus allocation extra roots) and every copied live pair field
   through the forwarding table while the old layout can still validate old IDs.
5. After installation, validation walks all roots and all live-object fields and checks that
   every object reference resolves through the current generation table.

Rejected alternative: a permanent handle-indirection table where ObjectIds never change.
That would preserve external numeric handles, but it would not exercise the root and heap
field rewriting invariant this phase is meant to protect. The current forwarding design
makes missed updates fail as stale IDs.

## Generational collection

The heap has two logical object generations on top of the same slot vector:

- New pair allocations are young.
- Any young object that survives one collection is promoted to old. This one-survival
  policy keeps the phase deterministic and makes promotion independent of wall-clock age or
  allocation rate.
- `Heap::collect()` is the major collector. It traces all objects reachable from mutator
  roots, compacts the whole heap, promotes young survivors, sweeps unreachable old and young
  objects, rewrites all roots and heap fields through the forwarding table, then validates.
- `Heap::collect_minor()` is the minor collector. It traces only young objects, with roots =
  mutator roots plus the deterministic remembered set of old objects that may reference
  young objects. Old objects are retained conservatively until the next major collection,
  but all roots and all retained heap fields are still rewritten through the forwarding
  table before mutator execution resumes.
- The remembered set is a `std::vector<ObjectId>`, not an unordered container. Barrier
  insertion is stable and duplicate-free by linear scan; collection-time pruning rebuilds it
  in slot order by keeping only valid old objects that still contain a young field.
- A dead old object in the remembered set can conservatively keep a young referent alive
  during a minor collection, but never forever: major collection traces only mutator roots,
  so the dead old graph is swept and remembered-set pruning drops the entry.
- `Heap::store_pair_field` is the only mutation hook exposed to bytecode. On every store of
  a young object into an old pair field it records the old object before publishing the
  value. The public heap API does not expose mutable pair fields directly, protecting the
  barrier-completeness invariant.

Rejected alternative: keep old objects fixed during minor collection and compact only young
slots. That would reduce forwarding work, but it would leave minor collection unable to
exercise the same root/heap-field rewrite discipline as major collection when young
survivors slide across holes. The current minor collector preserves old objects
conservatively but still uses one forwarding-table rewrite path for roots, heap fields, and
remembered-set entries.

## Protected Invariants

- Stack safety: the verifier checks each instruction's pop requirements before applying
  its push result, rather than checking only net stack effect.
- Local safety: locals have an explicit verifier state of uninitialized or known value kind;
  `LoadLocal` requires a known initialized kind on every incoming path.
- Root precision: `verify_with_stack_maps` emits per-pc stack object maps from the
  verifier's abstract states. Hand-written maps, when present, must match those generated
  maps, and VM-controlled collection points assert that runtime stack object tags agree
  with the generated map for the current pc.
- GC identity safety: dereferencing, marking, forwarding, or validating an invalid/stale
  `ObjectId` throws, exposing root-precision and missed-forwarding bugs instead of silently
  ignoring or aliasing them.
- Moving-collector safety: `RootProvider`/`RootVisitor` traces mutable root slots,
  allocation stress treats allocation operands and the new object as temporary roots, the
  forwarding pass rewrites all roots and live heap fields before mutator execution resumes,
  and VM collection points assert generated stack maps both before and after collection.
- Generational barrier safety: old-to-young stores are recorded in `Heap::store_pair_field`,
  minor collection traces young objects from mutator roots plus remembered old objects,
  remembered-set entries are rewritten/pruned after movement, and validation traps any
  valid old-to-young field absent from the remembered set.
- Source agreement: `compile_program` does not return bytecode for rejected source, and
  every returned function has passed `verify_with_stack_maps` with generated stack maps.

## Verifier join lattice

- Stack height is invariant at a merge; different heights reject immediately to protect
  stack-depth safety.
- Stack value kinds are `Int64`, `Bool`, `Object`, `Nil`, and `Poison`.
- Equal non-object kinds join to themselves; object kinds join to object with the union of
  possible allocation sites.
- Different kinds join to `Poison`, which cannot be consumed by any instruction or emitted
  into a stack map. This rejects ambiguous object/non-object roots instead of guessing.
- Local initialization joins by intersection: a local initialized on only one incoming path
  becomes uninitialized at the merge, so later `LoadLocal` rejects.
- Abstract pair fields join by allocation site and field kind. Mutating a field joins the
  previous abstract field with the stored value because one allocation site may represent
  multiple runtime objects.
- Rejected alternative: coercing differing kinds to a permissive `Any` value. That would let
  typed operations or stack maps treat maybe-object values imprecisely, violating type
  safety and future moving-collector root precision.
