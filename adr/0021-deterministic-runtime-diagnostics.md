# ADR 0021: Deterministic Runtime Diagnostics

## Status

Accepted for Iteration 45.

## Context

Before this iteration, runtime failures exposed deterministic exception text, and several
VM-generated messages included the active function index and pc. That was not a complete
reproduction record. Heap-thrown bounds and missing-key failures carried no frame
context, suspended callers were unavailable to the embedder, source line/column data
ended at frontend diagnostics, and iterative typed-exception unwind destroyed every
frame before the host observed an uncaught exception.

Changing exception text would break a mature observable contract. Storing frames,
`Value`s, or `ObjectId`s after failure would also be unsound for the moving collector:
such data could dangle after forwarding or become an accidental liveness edge.

## Decision

### Append-only module debug data

Each `Function` appends two verifier-inert fields: one debug name and one source position
per emitted pc. These fields are module data owned by `VerifiedModule`, but they are not
bytecode. `Instruction`, `OpCode`, verifier transfer, stack maps, and module acceptance
are unchanged.

The frontend appends a `(line,column)` entry whenever it appends an instruction. Restoring
position scopes make nested lowering deterministic: expression instructions use their
expression token, binary terminal operations use the operator token, and compiler-created
control flow uses its enclosing statement. The table length equals code length and table
order is pc order, so compiling identical source produces identical names and position
sequences.

Function 0 is named `<entry>`. Ordinary declarations use their source names. Lambdas use
`<lambda@LINE:COLUMN>`. A monomorphized generic uses its existing deterministic
`name$mono$<canonical-type-tuple>` name. Generic cloning already preserves AST positions,
so every concrete clone maps its pcs to the generic declaration/body, not the first call
that requested the clone. Equal tuples share one name/table; different tuples have
different names and corresponding template positions.

Hand-built modules may leave either field empty. Such modules still report function index
and pc. Debug metadata is intentionally not a verifier input: absent, short, or extra
tables cannot change whether executable code is safe.

### Plain-data side channel

`VM::last_trap_trace()` returns a const reference to an optional `RuntimeTrace`. The value
is cleared before every execution, remains empty after success or a caught language
exception, and is populated only for a terminal runtime trap or uncaught typed exception.
Every trace owns copied host data:

- failure kind;
- optional exception variant name;
- innermost-to-outermost frames;
- function index and pc for every frame;
- optional copied function name; and
- optional copied source line/column.

The public diagnostic types live in a dependency-light header that cannot name `Value`,
`ObjectId`, `Heap`, or collector state. Coordinate leaves have structural compile-time
assertions. `VM::trace_roots` never visits a diagnostic trace. Capturing a trace therefore
cannot dangle after movement, retain an object, affect marking, enter a remembered set, or
extend liveness.

### Pc semantics

The VM snapshots the active function and pc before stack-map assertion, instruction stress,
or opcode dispatch. That coordinate is the innermost failure site even when a call-depth
check fails after the caller has otherwise prepared a call.

Ordinary `Call` and `CallClosure` advance the stored caller pc before suspension.
Suspended frames therefore report `stored_pc - 1`, the call instruction, rather than the
post-call continuation. Frames are stored outermost-first and copied to the trace in
reverse order.

`TailCall` reuses and overwrites its frame. A later failure reports only the current
function in that slot; the tail caller is not reconstructable and is deliberately absent.
This is diagnostic history truncation by design, matching Iteration 39's constant-space
contract.

### Capture boundaries

Every ordinary host/runtime failure reaches the VM dispatch catch-all. The catch-all
copies the trace before completing active incremental phases, then rethrows the original
exception unchanged.

Typed exception unwind needs an earlier snapshot. At `Throw`, the VM copies a candidate
trace before removing any frames. Finding a matching handler discards the candidate.
Reaching the outer boundary installs it as `UncaughtException`, copies the exact nominal
variant name, clears `pending_exception_`, and throws the existing
`uncaught exception NAME` text. The outer catch sees the installed trace and does not
replace it with an empty post-unwind snapshot.

## Determinism argument

Existing invariants make a program's terminal trap independent of the fifteen GC stress
schedules. Debug names and tables depend only on deterministic frontend traversal and
monomorphization order. Frame order, active pc, suspended call-site normalization, and
variant layout names depend only on verified control flow. Trace capture iterates vectors
in fixed reverse frame order and reads no clock, address, object identity, hash table,
thread, random state, or collection counter.

Consequently two executions of the same trapping module under different schedules
produce equal failure kind, variant, frame count/order, names, pcs, and source positions.
The Iteration 45 matrix checks ten trap families through all fifteen schedules, and the
shared fuzz outcome compares the trace beside its canonical graph and output oracles.

## Consequences

- Existing exception messages, output bytes, verifier reasons, return values, opcodes,
  stack maps, metrics, and GC behavior are unchanged.
- Embedders can reproduce source failures without parsing exception text.
- Raw modules retain a useful index+pc fallback without manufacturing source metadata.
- A trace remains valid after the passed module goes out of scope because all names and
  positions are copied.
- Successful fuzz executions still have two substantive observables: canonical result
  graph/value and output bytes. Trapping outcomes now add trace equality as a third
  schedule oracle.

## Rejected alternatives

- Putting positions inside `Instruction` was rejected because it changes bytecode
  representation rather than adding side data.
- A module-level parallel vector was rejected because omissions in hand-built modules
  make function/debug alignment ambiguous.
- Reconstructing positions from the frontend AST after failure was rejected because the
  VM does not own the AST and lowering/monomorphization are not one-to-one.
- Formatting the trace into existing exception text was rejected because exception bytes
  are an existing observable.
- Retaining frames, exceptions, `Value`s, handles, or `ObjectId`s was rejected because
  diagnostics must not become roots or moving identities.
- Preserving logical TailCall history was rejected because it would add an unbounded
  shadow stack and violate the purpose of frame reuse.
