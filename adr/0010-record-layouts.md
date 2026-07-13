# ADR 0010: Use nominal per-module layouts for fixed-width records

## Status

Accepted.

## Decision

Records use `ObjectKind::Record` as mutable fixed-width objects with ordered, statically
typed fields. Every module owns a record-layout table. A layout gives one source record
declaration its nominal name, ordered field signatures, and a reference bitmap derived
exactly from those signatures: `i64` and `bool` fields are scalar, and every other field
is a reference. Verification rejects an empty or duplicate layout name, a malformed or
out-of-range field signature, a field-count/bitmap mismatch, or any bitmap bit that
disagrees with the field's static representation.

Layout table indices are the runtime and verifier identities of record types. Two source
declarations remain distinct even when their field names and types are identical. This
nominal rule makes assignment, function boundaries, containers, captures, weak targets,
and recursive fields agree on one finite identity rather than repeatedly comparing or
unfolding shapes. All record declarations are registered before their fields are resolved,
so a field may refer to its own record or another declared record without creating an
infinite module encoding. As with named recursive pair types, record references are
nullable and field access requires the existing `is_nil` false-branch refinement.

A heap record retains its validated module layout index, copied reference bitmap, and one
tagged slot per field. Its logical storage width is permanently
`1 + field_count`: one header plus the complete declared payload. No field may be added,
removed, or reordered after allocation. Compaction snapshots and asserts this width just
as it does for every other descriptor-sized object, but record mutation cannot change it.

The shared object descriptor visitor is the only strong-edge path. Its Record arm visits
exactly the bitmap-selected fields in declaration order. Scalar fields are never presented
to marking, forwarding, remembered-set maintenance, or post-collection validation, even
when their raw `i64` or bool payload bits equal a live, dead, stale, or forwarded
`ObjectId`. Reference fields accept only `Object` or canonical `Nil`; scalar fields accept
only scalar tags. The heap validates that agreement at construction, mutation, and
collection boundaries.

All record field writes flow through `Heap::store_record_field`; bytecode cannot obtain a
mutable payload reference. For a statically reference-typed field, the funnel validates
the owner layout and field shape and records an old owner in the remembered set before
publishing a young target. Scalar stores do not run the barrier. If collection promotion
itself produces an old record whose mapped field still points to a young object, the
generic descriptor-driven promotion-edge pass records the owner before remembered-set
validation. That collector-internal insertion is exact and deterministic and does not
increment mutator barrier metrics.

Bytecode adds append-only `AllocRecord(layout)`, `RecordGet(layout, field)`, and
`RecordSet(layout, field)` operations. `SignatureValue` carries exact record layout
identity through interprocedural and container types. The verifier proves layout and field
bounds, initializer arity and declared-order types, receiver identity and non-nil state,
stored-value type, and exact stack maps. The compiler independently classifies every field
and asserts at its boundary that the emitted bitmap agrees with the type checker's scalar
versus reference decision.

## Consequences

- Record scanning is precise for arbitrary interleavings of mutable scalar and reference
  fields without adding a conservative or object-ID-pattern scan.
- Nominal layout indices make same-shape record declarations incompatible by construction
  and keep recursive signatures finite.
- Record allocation and compaction have a stable width known from the validated layout;
  mutation never requires growth relocation.
- Mutable reference fields use the ordinary old-to-young barrier discipline, while
  promotion-created edges remain covered by the same descriptor-driven collector path.
- Adding records changes no existing opcode value, legacy generator stream, benchmark
  counter, runtime `Value` tag, or one-bit stack-map representation.

## Rejected alternatives

Structural record typing was rejected because same-shape declarations would lose their
source identity, recursive equality would need a separate coinductive shape relation, and
layout-index checks at bytecode operations would no longer match source assignment rules.
A global process-wide layout registry was rejected because verification and execution must
depend only on immutable in-module metadata, not load order or shared mutable state.

Variable-width or extensible records were rejected because field addition would change
descriptor width during mutation, require relocation like map growth, and invalidate the
declared bitmap/field-index contract. Reusing pairs or reference arrays was rejected
because neither represents arbitrary mixed scalar/reference slots with nominal field
identity. Conservative scanning of every tagged field was rejected because scalar payload
bits are deliberately opaque and may equal valid `ObjectId` encodings. Per-opcode or
per-collector scans were rejected because they would create strong-edge paths outside the
single descriptor visitor and make barrier/forwarding validation incomplete.
