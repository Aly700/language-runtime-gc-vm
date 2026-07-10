# ADR 0002: Represent strings as immutable opaque byte objects

## Status

Accepted.

## Decision

Strings use `ObjectKind::Str` with a descriptor byte length and an immutable raw-byte
payload. Their logical heap width is one header slot plus the payload rounded up to the
existing `std::int64_t` storage-slot granularity. The descriptor visitor has an explicit
`Str` arm that visits zero reference fields, so it remains the collector's only authority
for payload scanning.

String literals are decoded into one deterministic per-module constant pool and loaded by
`PushStr`. The verifier uses a distinct `ValueKind::Str`/`AbstractKind::Str` for operation
type safety, but stack maps mark string slots with the existing object-reference bit.
`StrConcat` allocates, `StrEq` compares bytes structurally, `StrLen` reports byte length,
and `StrIndex` returns an unsigned byte widened to `i64` with deterministic bounds traps.

## Consequences

- ObjectId-shaped byte patterns are opaque to marking, forwarding, remembered-set
  validation, and post-collection validation.
- Strings move and forward by base slot exactly like every other variable-sized object.
- The heap exposes no post-construction string payload setter, so strings have no write
  barrier or remembered-set path.
- There is no substring, runtime interning, Unicode interpretation, or string mutation in
  this iteration.
- Constant-pool indexes are verifier-checked before execution with the stable
  `BadStringConstantIndex` reason.

## Rejected alternatives

Reference-bearing ropes were rejected because they would add payload reference scanning,
barrier obligations, and multiple representations before substring or incremental concat
is required. Mutable byte arrays were rejected because they would violate the fixed
immutability boundary and create an unnecessary store API.
