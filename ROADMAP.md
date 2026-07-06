# Roadmap — Language Runtime: Bytecode VM + Precise Tracing GC

1. Lexer/parser/typed AST, type checker, and source-to-bytecode compiler.
2. Bytecode compiler and stack VM.
3. Precise stack maps and root enumeration.
4. Tracing and moving collection with barrier-ready object model.
5. Generational moving collector plus fuzzed GC timing.

## Current correctness boundary

Well-typed source must compile to bytecode that passes `verify_with_stack_maps`. A verifier
rejection of type-checked compiler output is a compiler bug.

## Phase 1 first tasks

- Make the bytecode verifier reject stack underflow and invalid local/root maps.
- Add allocation stress tests that trigger collection at deterministic instruction counts.
- Extend `Heap` with explicit root tracing APIs before making objects movable.
- Document each object-layout assumption beside the code that depends on it.

## Phase discipline

Do not optimize before correctness. Every phase should end with an executable deterministic test or replay artifact.
