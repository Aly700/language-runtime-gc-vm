# ADR 0009: Copying substrings and byte-wise string ordering

## Status

Accepted.

## Decision

`StrSub` consumes `str, i64, i64` and produces a fresh `ObjectKind::Str` containing the
half-open byte range `[lo, hi)`. It traps deterministically with `string substring bounds
out of range` when `lo < 0`, `hi` exceeds the source byte length, or `lo > hi`. `lo == hi`
is valid and allocates a fresh empty string. A full-range substring is structurally equal
to its source but has a different object identity.

Substring uses copy semantics: there is no sharing, view, or interning. Views would add a
second object kind referencing foreign payloads and complicate the opacity invariant for
zero measured need. Keeping one immutable opaque byte representation preserves the
existing descriptor rule that `Str` visits zero references and avoids new lifetime,
forwarding, barrier, and collector obligations.

Because `StrSub` allocates, the source operand stays on the VM frame through allocation and
is marked as the only reference bit among the three input slots. The heap helper mirrors
`StrConcat`: it accepts a copied source `Value`, registers that value as an extra precise
root if collection runs before allocation, reacquires the forwarded byte span afterward,
and roots the newly allocated result if collection runs after allocation. Bounds are
validated before entering that allocation boundary, and loud assertions defend the
verified/runtime agreement.

`StrLt` consumes two strings and produces a bool using unsigned byte-wise lexicographic
comparison. The first differing byte decides; if one payload is a proper prefix, the
shorter payload is less. Equal strings are not less, and `0x00 < 0xff` regardless of host
`char` signedness.

The frontend derives its entire string ordering surface from `StrLt`:

- `s1 < s2` lowers to `StrLt(s1, s2)`.
- `s1 > s2` lowers to `StrLt(s2, s1)`.
- `s1 <= s2` lowers to `!StrLt(s2, s1)`.
- `s1 >= s2` lowers to `!StrLt(s1, s2)`.

Existing `StrEq` remains the structural equality primitive for `==` and `!=`. The
verifier gives `StrSub` and `StrLt` append-only stable rejection reasons, and the frontend
reports positioned errors for mixed ordering operands and invalid `sub` receiver, arity,
or argument types.

## Consequences

- No new heap kind, descriptor edge, collector phase, write barrier, or mutable string API
  is introduced.
- Substring cost is linear in the copied byte count and independent of source lifetime.
- Ordering is deterministic for arbitrary byte strings, including embedded zero and bytes
  above `0x7f`.
- The isolated pinned `strings2` grammar compares both canonical heap graphs and output
  bytes across all ten GC schedules while leaving every legacy generator byte-identical.

## Rejected alternatives

Substring views were rejected because they require a reference-bearing representation or
an additional owner/payload lifetime scheme, weakening the single opaque-string invariant
without measured demand. Interning was rejected because identity canonicalization is not
part of string semantics. Adding four ordering bytecodes was rejected because operand
reversal plus deterministic boolean inversion expresses the complete source surface with
one verified comparison primitive.
