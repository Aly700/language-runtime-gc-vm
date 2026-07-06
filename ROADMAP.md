# Roadmap — Language Runtime: Bytecode VM + Precise Tracing GC

1. Lexer/parser/typed AST and type checker shell.
2. Bytecode compiler and stack VM with bump allocation.
3. Precise stack maps and root enumeration.
4. Tracing mark-sweep GC with barrier-ready object model.
5. Generational moving collector plus fuzzed GC timing.

## Phase 1 first tasks

- Make the bytecode verifier reject stack underflow and invalid local/root maps.
- Add allocation stress tests that trigger collection at deterministic instruction counts.
- Extend `Heap` with explicit root tracing APIs before making objects movable.
- Document each object-layout assumption beside the code that depends on it.

## Phase discipline

Do not optimize before correctness. Every phase should end with an executable deterministic test or replay artifact.
