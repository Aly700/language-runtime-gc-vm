# Architecture — Language Runtime

## Pipeline

```text
source -> lexer/parser -> typed AST -> bytecode -> verifier -> VM -> heap/GC
```

## Current scaffold

- `lang::Function` stores bytecode and future stack maps.
- `lang::verify` is the bytecode safety gate. It abstract-interprets the linear bytecode,
  proving stack depth before every instruction, `LoadLocal` initialization, and value
  kinds for stack-map validation. This protects the VM invariants for stack depth and
  initialized local reads.
- `lang::VM` owns stack/locals and registers itself as a `lang::gc::RootProvider`.
  Root tracing visits mutable `Value` slots, not copied root values, so a future moving
  collector can update roots before mutator execution resumes.
- `lang::gc::Heap` implements a mark-sweep baseline over generation-tagged object IDs.
  The low bits identify the storage slot and the high bits identify that slot's current
  generation, so a swept ID cannot alias a later object in the same slot.
- Deterministic GC stress is configured with explicit counters: before every allocation,
  after every allocation, and every N bytecode instructions. No stress trigger depends on
  wall-clock time, randomness, threads, or host addresses.

## Design bias

The initial heap uses object IDs instead of raw host pointers so future moving collection can be introduced without changing language-level identity.

## Protected Invariants

- Stack safety: the verifier checks each instruction's pop requirements before applying
  its push result, rather than checking only net stack effect.
- Local safety: locals have an explicit verifier state of uninitialized or known value kind;
  `LoadLocal` requires a known initialized kind.
- Root precision: stack maps, when present, must match the verifier's abstract object slots
  before the instruction at the same pc.
- GC identity safety: dereferencing or marking an invalid/stale `ObjectId` throws, exposing
  root-precision bugs instead of silently ignoring or aliasing them.
- Moving-collector readiness: `RootProvider`/`RootVisitor` traces mutable root slots and
  allocation stress treats allocation operands and the new object as temporary roots.
