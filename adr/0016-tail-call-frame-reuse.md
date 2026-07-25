# ADR 0016: Verifier-Checked Tail Calls with Frame Reuse

## Status

Accepted for Iteration 39.

## Decision

The source language exposes tail transfer explicitly as
`return tail f(arg1, ..., argN);`. It is a terminating statement inside a named
function or lambda; the ordinary base path still ends in the function's existing final
expression. `return` is reserved, while `tail` remains an ordinary identifier everywhere
except immediately after `return`, preserving legacy programs that use `tail` as a name.
The frontend accepts only a call to a directly named function. It does not infer tail
position, optimize `Call; Return`, or tail-call through a function value.

`TailCall` is appended after all existing opcodes and carries a direct in-module function
index. The verifier treats it as a terminal, return-shaped transition. It rejects an
out-of-range target or a closure body that needs captures, requires exact equality between
the callee's and caller's complete return signatures, requires the operand stack to
contain exactly the callee arguments and nothing else, and checks every argument against
the corresponding complete parameter signature. A valid TailCall has no ordinary or
exceptional successor, so later instructions follow the existing unreachable-code rule.
The append-only stable rejection reasons are `TailCallInTryRegion`,
`BadTailCallTarget`, `TailCallReturnTypeMismatch`, `BadTailCallArity`,
`TailCallStackShapeMismatch`, and `BadTailCallArgKind`.

The stack map at a TailCall pc is a transfer-boundary map. Its operand bits describe the
outgoing arguments in stack order, including every reference-valued argument as a precise
mutable root. Every local bit is false even if a local was reference-capable earlier in
the function. Before the VM's first map assertion or any instruction-boundary GC work at
that pc, it overwrites every local with canonical `nil` and clears the frame-owned closure
slot. The collector can therefore observe the outgoing arguments, but it cannot retain or
forward a stale local or capture container from the dying invocation. Supplied bytecode
maps must match this same special boundary shape.

After boundary collection has rewritten any moved argument IDs, the VM reuses the current
`Frame`. It removes the arguments from the old operand stack, changes the function index,
sets the pc to zero, replaces the local vector with a callee-sized vector of `nil`, copies
parameters into locals `0..N-1`, and leaves the closure slot empty. It neither pushes a
frame nor consults the call-depth limit. The next dispatch iteration checks the ordinary
callee-entry stack map. Consequently self and mutual explicit tail recursion use constant
VM frame depth, while `Call` and `CallClosure` retain the existing deterministic
call-depth trap.

A TailCall pc inside an active try range in the same frame is rejected. Keeping that
handler after reusing its owning frame would require a separate continuation record and
would make handler ownership ambiguous. A callee reached by TailCall therefore propagates
a language exception directly to the next suspended frame. During that unwind,
`pending_exception_` remains the single precise moving root from Iteration 35. Suspended
frame stacks, locals, and closures continue through the existing all-frame root walk;
Handles, embedder root providers, allocation extra roots, incremental marking worklists,
and partial-compaction forwarding are unchanged.

Tail transfer adds no host-stack recursion and no new scheduling input. Scrubbing,
boundary collection, argument copying, and frame installation use deterministic vector
and source order. There is no wall clock, thread, random choice, host address, hash-table
iteration, or implicit optimization decision. Existing bytecode contains no TailCall, so
its instruction counts, collection boundaries, output, graphs, pinned fuzz streams, and
benchmark counters are unchanged. The isolated `tailcalls` source corpus adds 32 seeds
run under all 15 existing schedules; its pinned dump SHA-256 is
`72e0af127c314f7bfa4ceb3961170b452fc490e5ea9f31fcfb6643cddebeaf61`.

## Rejected alternatives

- Implicitly converting `Call; Return` was rejected because it hides the proof boundary,
  changes bytecode meaning through peephole context, and cannot express the special root
  map explicitly.
- Tail calls through closures were rejected because the callee closure would need a
  second handoff root and a distinct verified opcode contract. Direct named calls cover
  self and mutual recursion without broadening the crux.
- Teaching all-frame root tracing to interpret public stack maps was rejected because a
  suspended ordinary caller is intentionally between verifier-visible states after its
  arguments are removed and before its result is pushed.
- Copying arguments to a temporary root vector, popping the frame, and pushing a callee
  was rejected because it creates another root owner, routes near the ordinary depth
  check, and is not in-place frame reuse.
- Retaining dying locals until after collection was rejected because it would make dead
  objects observable as roots and could change weak, ephemeron, and movement outcomes.
- Preserving same-frame try handlers across a TailCall was rejected because handler
  lifetime would outlive the frame state that defines its protected region.
