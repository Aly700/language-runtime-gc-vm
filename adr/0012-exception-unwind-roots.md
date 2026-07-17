# ADR-0012: Typed Variant Exceptions and Precise Unwind Roots

## Status

Accepted for Iteration 35.

## Decision

Language exceptions are proven non-nil nominal variant objects. `throw` transfers the
object to the nearest active `catch` whose declared variant layout is identical; other
exceptions continue through caller frames. Existing runtime failures—including bounds,
nil access, conversion, missing-key, depth, verifier, descriptor, and output failures—stay
uncatchable deterministic traps.

Functions carry verified half-open exception-handler ranges. Append-only `TryBegin`,
`TryEnd`, and `Throw` opcodes make lexical boundaries explicit. The verifier gives every
throwing instruction an exceptional successor with an empty operand stack plus exactly
one non-nil exception reference, joins locals at handler entry, and conservatively gives
calls an exceptional edge to an enclosing handler. Ordinary execution jumps over handler
entries.

The VM unwinds its explicit frame vector iteratively. While no handler owns the value,
`pending_exception_` is one precise mutable root. Collection during teardown visits and
forwards that slot together with every surviving frame stack, local vector, and closure.
On transfer, the forwarded value moves to the handler-entry stack and the pending slot is
cleared. Uncaught exceptions remove all frames, clear the slot, and trap with
`uncaught exception <VariantName>`.

## Rejected alternatives

- Catching C++ exceptions would conflate language control flow with runtime traps.
- Untyped or scalar throws would require a new boxed representation and runtime type tags.
- Conservative frame or payload scanning would violate precise-root and scalar-opacity
  invariants.
- Native C++ stack unwinding would hide frame teardown from the moving collector.
