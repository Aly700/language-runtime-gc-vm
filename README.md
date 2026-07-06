# Language Runtime: Bytecode VM + Precise Tracing GC

## Scope

A small statically typed language runtime whose hardest correctness boundary is precise garbage collection: stack maps, roots, tri-color marking, barriers, and eventually a moving generational collector must agree on the exact object graph.

## Stack

C++20, CMake, recursive-descent frontend, verified bytecode VM, precise stack maps, and deterministic moving/generational GC stress modes.

## Build and smoke test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Phase map

1. Frontend surface: lexer/parser, typed AST, type checker, and bytecode compiler.
2. Bytecode compiler and stack VM.
3. Precise stack maps and root enumeration.
4. Tracing and moving collection with barrier-ready object model.
5. Generational moving collector plus fuzzed GC timing.

## Start here

Start with `INVARIANTS.md` and `ARCHITECTURE.md`. The frontend entry point is `lang::frontend::compile_program`; every returned function has already passed `verify_with_stack_maps`.
