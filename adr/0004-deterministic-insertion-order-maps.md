# ADR 0004: Use deterministic insertion-order maps without hashing

## Status

Accepted for insertion-order payload, layout, growth, and iteration semantics. The
no-hashing lookup decision is refined by
[ADR 0017](0017-deterministic-content-hashed-map-index.md); identity/address hashing
remains rejected.

## Decision

Maps use `ObjectKind::Map` as a mutable insertion-order association container. The payload
is a vector of tagged `(key, value)` entries. Lookup performs a linear scan in insertion
order; setting an existing key replaces only its value, while setting a new key appends one
entry. Keys are restricted to `i64`, `bool`, and `str`. Integer and bool keys compare by
tagged scalar value, and string keys compare their immutable bytes structurally.

As originally accepted, no hashing was used. In particular, an `ObjectId` may not be used
as an identity hash:
moving collection changes its slot/generation bits when forwarding the same language-level
object. Structural string equality plus insertion order makes lookup independent of those
move-sensitive bits and keeps iteration, tracing, fuzz observables, and traps deterministic.
The originally accepted cost was O(n) lookup and open addressing was deferred. Iteration
40's measured content-hash entry-index optimization now replaces that linear lookup under
ADR-0017 without changing any other decision in this ADR.

Every module carries a map-layout table. Each layout stores complete structural key/value
types and the exactly-derived `key-is-ref` and `value-is-ref` flags. A heap map retains its
validated layout identity and flags. The descriptor visitor validates entry count and slot
tags, visits string-key slots and reference-value slots, and never presents scalar slots to
marking, forwarding, remembered-set logic, or validation. Reference values may be `Nil`
under the existing named-type rules.

All insertion and update flows through `Heap::store_map_entry`. For an old owner, the
funnel records a young inserted string key or young reference value in the remembered set
before publishing the entry. Updates retain the original key position and barrier the new
value. There is no alternate mutable map payload API.

A map's logical storage width is `1 + 2 * entry_count`. Appends grow that width after
allocation. If the two adjacent logical slots are unavailable, the heap deterministically
relocates the map to a large-enough run and rewrites roots, descriptor-declared references,
and remembered-set entries through a forwarding table before publishing. Collection takes
one width snapshot per object and asserts that the width stays unchanged until that
collection finishes.

## Consequences

- Fresh byte-equal strings find entries even after stored key strings move during full
  compaction.
- Map tracing remains precise per slot, including scalar payloads whose bits equal stale or
  live object IDs.
- Map operations and nested map types remain deterministic across GC stress schedules and
  recover complete key/value facts through `SignatureValue`.
- Growing maps can move outside a collection, but only through the same precise forwarding
  discipline used by compaction; raw external `ObjectId` copies remain intentionally stale,
  while VM roots and `Handle`s are rewritten.

## Rejected alternatives

Identity-based hashing over `ObjectId` was rejected because collection changes the identity
bits and would make lookup movement-dependent. Host-pointer hashing was rejected because it
would leak address nondeterminism and bypass forwarding invariants. Open addressing and
structural hash caching were deferred because they add resize policy, tombstones, cached
reference-bearing state, and new determinism obligations before measurements justify them.
Sorting entries by key was rejected because it discards source-visible insertion order and
adds comparison semantics not required by the language.

## Iteration semantics

Iteration 28 makes the order guaranteed here directly observable through
`for key, value in map`. The compiler walks positional entries from index zero to the
entry-count snapshot, so updates preserve their original position and new entries are never
silently appended to an in-progress traversal. Existing-value updates remain visible when
their position is reached; inserting a new key grows the entry count and deterministically
traps the lowered loop. ADR-0007 specifies the complete mutation semantics.
