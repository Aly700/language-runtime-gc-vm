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
- `lang::gc::Heap` implements a mark-sweep baseline over generation-tagged object IDs.
  The low bits identify the storage slot and the high bits identify that slot's current
  generation, so a swept ID cannot alias a later object in the same slot.
- Pair field mutation is routed through `Heap::store_pair_field`. That method is the
  single hook point where a future generational write barrier must run before the new
  field value is published.
- Deterministic GC stress is configured with explicit counters: before every allocation,
  after every allocation, and every N bytecode instructions. No stress trigger depends on
  wall-clock time, randomness, threads, or host addresses.

## Design bias

The initial heap uses object IDs instead of raw host pointers so future moving collection can be introduced without changing language-level identity.

## Protected Invariants

- Stack safety: the verifier checks each instruction's pop requirements before applying
  its push result, rather than checking only net stack effect.
- Local safety: locals have an explicit verifier state of uninitialized or known value kind;
  `LoadLocal` requires a known initialized kind on every incoming path.
- Root precision: `verify_with_stack_maps` emits per-pc stack object maps from the
  verifier's abstract states. Hand-written maps, when present, must match those generated
  maps, and VM-controlled collection points assert that runtime stack object tags agree
  with the generated map for the current pc.
- GC identity safety: dereferencing or marking an invalid/stale `ObjectId` throws, exposing
  root-precision bugs instead of silently ignoring or aliasing them.
- Moving-collector readiness: `RootProvider`/`RootVisitor` traces mutable root slots and
  allocation stress treats allocation operands and the new object as temporary roots.

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
