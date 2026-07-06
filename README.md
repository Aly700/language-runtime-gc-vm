# Language Runtime: Bytecode VM + Precise Tracing GC

## Scope

A small statically typed language runtime whose hardest correctness boundary is precise garbage collection: stack maps, roots, tri-color marking, barriers, and eventually a moving generational collector must agree on the exact object graph.

## Stack

C++20, CMake, small bytecode VM, mark-sweep GC baseline, moving/generational collector reserved for later phases.

## Build and smoke test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Phase map

1. Lexer/parser/typed AST and type checker shell.
2. Bytecode compiler and stack VM with bump allocation.
3. Precise stack maps and root enumeration.
4. Tracing mark-sweep GC with barrier-ready object model.
5. Generational moving collector plus fuzzed GC timing.

