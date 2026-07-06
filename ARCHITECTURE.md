# Architecture — Language Runtime

## Pipeline

```text
source -> lexer/parser -> typed AST -> bytecode -> verifier -> VM -> heap/GC
```

## Current scaffold

- `lang::Function` stores bytecode and future stack maps.
- `lang::verify` is the bytecode safety gate. It runs a worklist dataflow analysis over
  reachable bytecode, proving stack depth before every instruction, `LoadLocal`
  initialization over all incoming paths, branch target validity, and value kinds for
  generated stack maps. This protects the VM invariants for stack depth and initialized
  local reads across loops and merge points.
- `lang::VM` owns stack/locals and registers itself as a `lang::gc::RootProvider`.
  Root tracing visits mutable `Value` slots, not copied root values, so a future moving
  collector can update roots before mutator execution resumes.
- `lang::gc::Heap` implements a deterministic mark-compact collector over
  generation-tagged object IDs. The low bits identify the storage slot and the high bits
  identify that slot's current generation, so a swept or moved ID cannot alias a later
  object in the same slot.
- Pair field mutation is routed through `Heap::store_pair_field`. That method is the
  single hook point where a future generational write barrier must run before the new
  field value is published.
- Deterministic GC stress is configured with explicit counters: before every allocation,
  after every allocation, and every N bytecode instructions. Every stress collection runs
  the same post-collection reference validation as explicit collections. No stress trigger
  depends on wall-clock time, randomness, threads, or host addresses.

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

When generations arrive, this compactor is the structural basis for a copying young
generation: the forwarding table becomes from-space to to-space metadata, the rewrite pass
updates roots and remembered old-to-young fields, and `Heap::store_pair_field` remains the
barrier hook that records old-to-young stores before publishing them.

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
