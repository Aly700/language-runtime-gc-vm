# ADR 0022: Labeled Loops and Core Built-in Lowering

## Status

Accepted for Iteration 46.

## Context

Iteration 30 introduced terminating `break;` and `continue;` statements through
frontend flow contexts and unresolved ordinary jumps. The nearest enclosing loop
owned each transition. A `while` continue reached its condition header; a range,
array, or map `for-in` continue reached that loop's hidden increment preamble.
This target is semantic, not an optimization detail: incrementing preserves trip
counts, and a map backedge must revisit the entry-count mutation check.

That nearest-loop-only surface made nested algorithms awkward. Adding labels
requires more than parsing a name: flow facts must reach the selected loop's
fixed point or exit, compiler fixups must survive intervening loops, scoped
for-in locals must not leak through a multi-level transition, and verifier maps
must remain exact at the selected target.

The language also had the primitive operations needed for common integer
selection and byte-string search, but callers had to spell out their control
flow. Most requested built-ins can be synthesized without extending the VM.
`abs`, however, cannot: the instruction set has integer constants, addition, and
comparison, but no negation or subtraction. Repeated addition toward zero is
unboundedly expensive and cannot honestly implement the signed minimum
boundary.

## Decision

### Labeled syntax

A label is an ordinary identifier followed by `:` directly before a `while` or
`for-in` statement:

```text
outer: while condition {
  scan: for item in items {
    break outer;
  }
}
```

`break;` and `continue;` retain their iteration-30 nearest-loop behavior.
`break name;` and `continue name;` select the lexically enclosing active loop
whose label is `name`.

The parser recognizes a label prefix only in statement position and accepts it
only before `while` or `for`. A stable
`loop label must precede 'while' or 'for'` diagnostic rejects other uses. The
new labeled wrapper disables record-literal lookahead only while parsing its
loop condition or iterable. This resolves `for x in xs { inner: ... }` without
changing the pinned unlabeled loop grammar path.

### Label namespace and diagnostics

Labels are callable-local. Entry code, each declared function, and each lambda
have separate inventories. Lookup never crosses a function or lambda boundary.

Active labels must be unique. A nested loop may not shadow an active label:

```text
outer: while true {
  outer: while true { } // rejected
}
```

The same name may be reused by sibling or later loops after the first lexical
extent ends. This rule avoids ambiguous targets while retaining harmless local
reuse.

Before checking a callable, the frontend collects all loop-label declarations
in that callable, excluding nested callable bodies. This makes two rejection
classes stable:

- a name absent from the callable inventory reports
  `unknown loop label 'name'`; and
- a name present in the callable but absent from the active context stack
  reports
  `loop label 'name' does not lexically enclose this break` or the corresponding
  `continue` form.

An active duplicate reports
`loop label 'name' duplicates an active loop label`. Labeled control outside a
loop is rejected by the same unknown/non-enclosing distinction. Existing
unlabeled out-of-loop diagnostics are unchanged.

### Multi-level flow and jump patching

Each type-checker loop context carries its optional label plus break and
continue state sets. Labeled control searches contexts from innermost to
outermost and appends its incoming `FlowState` to the selected context.
The current source path terminates.

The iteration-30 joins remain conservative:

- body fallthrough and selected continue states join into the selected loop's
  next header state;
- selected break states join at that loop's exit with ordinary exhaustion; and
- literal-true `while` contributes no fabricated condition-false exit.

When a transition crosses an inner for-in scope, that loop's immutable
iteration locals are deactivated in the state delivered to the outer context.
Match-binding cleanup applies to every active loop context as before.
Speculative fixed-point checks retain deterministic state-vector behavior, and
all joins continue to use the existing intersection lattice.

The compiler mirrors the checker. Each `LoopPatchContext` carries its optional
label. A labeled control jump is appended directly to the selected context, so
an inner loop leaves an outer-target fixup unresolved until the outer lowering
knows its header, increment, or exit pc. In particular:

- labeled `continue` to a `while` reaches that while's header;
- labeled `continue` to any for-in form reaches that loop's increment preamble;
- a map continue therefore returns through its mutation check; and
- labeled `break` reaches the first instruction after the selected loop.

The bytecode remains ordinary `Jump`. The existing verifier performs every
stack-height, initialization, abstract-kind, reachability, and stack-map join.
Tests identify the actual two-depth outer break and continue targets, assert
empty operand stacks and exact local root bits, and execute with major/minor
collection at every instruction.

### Integer built-ins

The source forms are:

```text
abs(value)
min(left, right)
max(left, right)
```

They accept only `i64`; arity and argument diagnostics are positioned and
stable. Arguments are evaluated exactly once, left to right.

`min` and `max` are pure frontend lowering. The compiler stores the two results
in scalar temporary locals, compares them with `LessI64`, and selects one with
existing jumps. Equality may select either operand because both values are
identical.

`abs` emits one append-only `I64Abs` opcode. The verifier requires one `i64`
operand through the append-only `I64AbsRequiresI64` reason and produces `i64`.
At runtime, nonnegative values pass through and negative values are negated
only after excluding `INT64_MIN`. The signed minimum traps deterministically at
the opcode pc with:

```text
absolute value overflow
```

The guard occurs before negation, so execution does not invoke signed-overflow
undefined behavior. This single opcode is the narrow honest boundary; adding a
general subtraction/negation family was outside the requested scope.

### String search built-ins

Strings remain immutable byte sequences. The new methods are:

```text
s.contains(sub)     // bool
s.index_of(sub)     // i64
s.starts_with(sub)  // bool
s.ends_with(sub)    // bool
```

The receiver is evaluated once before the argument, which is also evaluated
once. Both are stored in compiler-owned string locals before later work. The
verifier consequently marks exactly those locals as roots across every
allocating `StrSub` pc and moving collection.

All four methods lower to existing instructions:

- `contains` and `index_of` advance a scalar candidate byte index while
  `index + sub.len <= s.len`, compare `s.sub(index, index + sub.len)` with the
  argument through `StrEq`, and stop at the first match;
- `starts_with` length-guards one prefix `StrSub`/`StrEq`; and
- `ends_with` length-guards, advances a scalar start until
  `start + sub.len == s.len`, then performs one `StrSub`/`StrEq`.

The emitted bytecode size is fixed; the candidate loop is runtime control flow,
not unrolled code. Empty substrings match at byte index zero. Indices are byte
offsets, consistent with string length, indexing, ordering, and substring
semantics.

`index_of` returns `-1` when absent. Absence is a normal total search outcome
that callers commonly branch on, whereas malformed `StrToI64` input is an
invalid conversion with no value. Manufacturing a conversion or bounds trap
for ordinary absence would give misleading failure text and violate honest
lowering. This distinction is therefore consistent with the conversion API's
trap policy rather than an exception to it.

## Determinism and precision argument

Label inventories use source-order AST traversal and membership only; target
resolution uses the ordered lexical context stack. Jump patching uses
source-order vectors. Built-in lowering evaluates operands in source order,
uses deterministic scalar loops, and reads immutable string bytes. No path
consults wall time, addresses, randomized hashing, thread state, or collection
timing.

Compiler temporary locals enter the ordinary verifier dataflow. At candidate
substring allocations, the stack map contains one receiver root on the operand
stack and exactly the saved receiver/argument roots in locals. Labeled
multi-level targets likewise use verifier-derived maps, not handcrafted root
metadata. Every one of the fifteen schedules therefore observes the same
result graph, output, trap, and runtime trace.

## Corpus and compatibility consequences

- The existing `loops` grammar and all other legacy generators are untouched.
- The additive `ergonomics` grammar has 32 seeds. Every seed exercises labeled
  loops at depths two and three plus all seven built-ins, compares non-empty
  graph and output oracles under all fifteen schedules, and runs eighteen
  positioned rejection mutants.
- Its pinned source, representative outcome, and corpus FNV-1a values are
  `3513356585459432607`, `11394261262610471186`, and
  `7085578191262596976`.
- All 22 pre-iteration-46 corpus dumps and all established benchmark counters
  remain byte-identical.

## Invariant impact

`INVARIANTS.md` is intentionally untouched.

No heap representation, object descriptor, collector edge, root-map format,
barrier, weak processing rule, forwarding path, schedule, or determinism
invariant changed. `I64Abs` is scalar-only. Every synthesized string temporary
and labeled branch target is covered by the existing verifier/stack-map
invariants. Iteration 46 exercises those invariants at new frontend-generated
control-flow shapes; it does not amend them.

## Rejected alternatives

- Dynamically searching labels at runtime was rejected because labels are
  lexical control flow and ordinary verified jumps are sufficient.
- Allowing active label shadowing was rejected because identical visible names
  would make targets easy to misread and add no capability.
- Treating every inactive declaration as unknown was rejected because stable
  non-enclosing diagnostics materially improve nested-control errors.
- Adding labeled-loop opcodes was rejected because it would duplicate frontend
  target resolution in the VM.
- Lowering `abs` by repeatedly adding one toward zero was rejected as
  catastrophically slow and incapable of an honest signed-minimum boundary.
- Adding general negate/subtract opcodes was rejected as broader than the
  feature needs.
- Adding `StrFind` was rejected because fixed-size lowering with existing
  `StrSub`, `StrEq`, arithmetic, and jumps is sound and keeps the runtime
  surface smaller. Candidate substring allocation is an accepted simplicity
  trade-off for this iteration.
- Trapping when `index_of` finds no match was rejected because absence is a
  normal query result, not malformed input.
