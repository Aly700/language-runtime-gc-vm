# Invariants — Language Runtime

## VM

- Bytecode verification must prove stack depth is valid at every instruction.
- A bytecode instruction may only read locals proven initialized by the verifier.
- VM observable behavior must not depend on host pointer addresses.

## GC

- Every live object is reachable from an explicit root or another live object at collection start.
- The collector must never treat non-reference values as references.
- No object may be swept while reachable.
- If a moving collector is introduced, every root and heap reference must be updated before mutator execution resumes.
- Write barriers must run on every old-to-young reference store once generations exist.

## Testing

- GC stress mode must be deterministic and able to collect before or after any allocation.
- A test that passes with GC disabled is not sufficient for runtime correctness.
