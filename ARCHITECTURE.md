# Architecture — Language Runtime

## Pipeline

```text
source -> lexer/parser -> typed AST -> bytecode -> verifier -> VM -> heap/GC
```

## Current scaffold

- `lang::Function` stores bytecode and future stack maps.
- `lang::verify` is the bytecode safety gate.
- `lang::VM` owns stack/locals and exposes roots to the heap.
- `lang::gc::Heap` implements a mark-sweep baseline over stable object IDs.

## Design bias

The initial heap uses object IDs instead of raw host pointers so future moving collection can be introduced without changing language-level identity.
