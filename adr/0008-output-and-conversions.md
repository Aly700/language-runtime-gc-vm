# ADR 0008: Deterministic bounded output and explicit string conversions

## Status

Accepted.

## Decision

Programs gain one observable output channel: a VM-owned `std::vector<uint8_t>`. A verified
execution clears the vector before dispatch, and the embedder reads it afterward through
`VM::output()`. The VM never writes stdout, uses no host stream, and adds no clock, thread,
address, or schedule metadata. `Print` consumes a verified `Str`, copies its immutable
bytes, appends exactly one newline, and leaves no stack result.

Output is bounded by the documented constant `VM::kMaxOutputBytes`, fixed at 1 MiB. Before
copying, `Print` checks the complete string-plus-newline append. An append that would cross
the cap traps at that opcode with the stable diagnostic `output buffer overflow`; it does
not partially append. Reset-on-execution makes VM reuse produce independent logs.

The output vector is not a root and stores no `Value` or `ObjectId`. `trace_roots` visits
only frames. Major/minor collection, forwarding, descriptors, remembered sets, barriers,
weak processing, and validation therefore cannot observe or mutate output. `Print` keeps
its string operand in the frame until copying completes, so an instruction-boundary
collection sees the ordinary precise string root.

Four append-only bytecodes define the boundary:

- `Print`: `str -> []` and append bytes plus newline.
- `I64ToStr`: `i64 -> str`, allocating the unique decimal representation.
- `StrToI64`: `str -> i64`, trapping on malformed or overflowing input.
- `BoolToStr`: `bool -> str`, allocating exactly `true` or `false`.

Each opcode has its own stable verifier rejection reason. `I64ToStr` uses a conversion that
handles `INT64_MIN` directly rather than negating it. `StrToI64` accepts exactly an optional
ASCII `-` followed by one or more ASCII digits; `+`, whitespace, leading zeroes, `-0`, and
values outside `[-9223372036854775808, 9223372036854775807]` trap with `invalid string for
i64 conversion`. A checked unsigned magnitude accumulator makes overflow independent of
host parsing libraries and locales.

The frontend surface is `print(str);`, `to_str(i64|bool)`, and `to_i64(str)`. `to_str` is
resolved statically to one opcode, and wrong argument types are positioned frontend
errors. String interpolation is deferred; concatenation plus `to_str` is the sole
composition mechanism in this iteration.

## Consequences

- Program output is a second fuzzer oracle independent of the returned canonical heap
  graph. Both observables must be byte-identical across all ten GC schedules.
- A dedicated pinned `output` grammar exercises the new surface without changing any of
  the eleven legacy generator streams.
- Output memory and execution time remain deterministically bounded.
- Conversion failures are loud runtime traps and cannot be confused with a valid numeric
  result.

## Rejected alternatives

Writing directly to stdout or accepting an embedder stream was rejected because external
buffering, errors, and interleaving would enter program semantics. An unbounded buffer was
rejected because fuzz programs could consume unbounded host memory. Returning a sentinel
from `StrToI64` was rejected because every int64 value is valid and a sentinel would either
lose a value or require a new error union. Locale-sensitive host formatting/parsing was
rejected because the accepted language and diagnostics must be platform-independent.
