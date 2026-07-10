# ADR 0003: Represent closures with immutable captures and precise capture maps

## Status

Accepted.

## Decision

Closures use `ObjectKind::Closure` and variable-width heap storage consisting of a raw
function index followed by immutable tagged capture slots. Source lambdas capture enclosing
locals by value in deterministic first-use order. Named top-level functions are represented
as zero-capture closures when used as values.

Every module owns a closure-layout table. A layout contains the target function's structural
`fn(T1, ..., Tn) -> R` type, ordered capture types, and a reference bitmap derived exactly
from those capture types. Verification proves layout bounds, function-signature agreement,
capture arity/type agreement, closure-call argument types, and `LoadCapture` scope/indexes.
The heap receives only that validated layout metadata and retains a heap-side capture-map
copy with the object descriptor so closures remain precisely scannable across moving
collections and later module executions.

The closure descriptor visits exactly bitmap-selected capture slots. Scalar captures are
opaque to marking, forwarding, remembered-set validation, and post-collection validation,
even when their bits equal an `ObjectId`; adjacent mapped reference captures are traced and
rewritten normally. This descriptor visitor remains the only object-payload scan path.

## Consequences

- Closure values use the existing runtime object tag and stack-map root bit while retaining
  a distinct structural verifier type.
- `CallClosure` pushes the same VM frame representation as direct `Call`, including the same
  deterministic depth limit, plus one precise frame-owned closure root for `LoadCapture`.
- Captures cannot be mutated after allocation, so there is no closure store opcode and no
  mutator write barrier.
- If promotion creates an old closure with a mapped young capture, the collector records the
  owner in the remembered set before boundary validation. This insertion is deterministic,
  exact, and excluded from mutator barrier metrics.
- Function-typed arrays use `RefArray`; function types can also appear recursively inside
  pair fields, parameters, returns, and other function signatures.

## Rejected alternatives

Environment pairs or generic reference arrays were rejected because they cannot express
per-slot scalar/reference precision without one-off collector scans. Mutable capture cells
were rejected because they add aliasing, store opcodes, and barrier obligations that are not
needed for snapshot semantics. Conservative scanning of every tagged capture was rejected
because scalar payloads can equal valid or stale `ObjectId` bit patterns. Host function
pointers were rejected because they would make behavior depend on addresses and bypass the
verified in-module function table.
