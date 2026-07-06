# Invariants — Language Runtime

## VM

- Bytecode verification must prove stack depth is valid at every instruction.
- A bytecode instruction may only read locals proven initialized by the verifier.
- A call instruction may only target an in-module function whose declared parameter kinds
  match the values proven on the caller stack.
- A return instruction may only return a value whose kind matches the current function's
  declared return kind.
- Every live VM frame's operand stack and locals are precise mutable roots. Moving
  collection must rewrite references in active and suspended frames before bytecode
  execution resumes.
- Call depth is bounded by an explicit VM limit and must trap deterministically before
  host stack exhaustion can matter.
- VM observable behavior must not depend on host pointer addresses.

## GC

- Every live object is reachable from an explicit root or another live object at collection start.
- The collector must never treat non-reference values as references.
- No object may be swept while reachable.
- If a moving collector is introduced, every root and heap reference must be updated before mutator execution resumes.
- Every live embedder handle is a precise mutable root slot; handle destruction removes that slot before the next collection, and the heap must outlive all handles.
- Write barriers must run on every old-to-young reference store once generations exist.

## Testing

- GC stress mode must be deterministic and able to collect before or after any allocation.
- A test that passes with GC disabled is not sufficient for runtime correctness.
