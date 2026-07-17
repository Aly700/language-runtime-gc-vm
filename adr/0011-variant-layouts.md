# ADR 0011: Use nominal tagged layouts with exact active-case scanning

## Status

Accepted.

## Decision

Variants are nominal immutable tagged objects described by a deterministic per-module
layout table. Each layout has a unique source name and an ordered, non-empty case table;
each case has its source name, ordered payload signatures, and a reference bitmap derived
exactly from those signatures. `i64` and `bool` payloads are scalar and every other
payload is a reference. Layout indices are the runtime, verifier, and frontend identity:
same-shaped declarations remain incompatible, and registering every variant before
resolving payloads permits finite self and forward references.

A heap variant retains its validated module layout index, a raw case tag, the active
case's tagged payload values, and a complete copied bitmap table for every case. Its
logical storage width is exactly `2 + active_field_count`: one header slot, one raw tag
slot, and only the selected case's payload slots. Different cases may therefore have
different widths. The full bitmap table is retained so heap validity does not depend on
mutable or out-of-band module metadata, while the raw tag selects the sole bitmap whose
width and tags must agree with the active payload.

The shared object descriptor visitor is the only authority that selects strong variant
edges. It validates the raw tag range, selected bitmap length, active payload width, and
each selected slot's tag, then visits exactly the selected case's true bits in payload
order. Inactive case maps are metadata, not payload, and scalar active slots remain opaque
even when their bits resemble current, stale, or forwarded object IDs. Marking,
forwarding, remembered-set pruning, promotion-edge discovery, and post-collection
validation all use this same descriptor path.

Variant payloads are immutable after allocation. There is no heap setter and no bytecode
store opcode, so the mutator cannot create a post-publication old-to-young edge. If a
collection promotes a variant while one of its descriptor-selected references remains
young, the generic promotion-created-edge pass inserts the old owner into the remembered
set before boundary validation. This collector-owned insertion is exact, deterministic,
and does not count as a mutator write barrier.

Bytecode appends `AllocVariant(layout, case)`, operand-free `VariantTag`, and
`VariantGet(layout, case, field)`. `SignatureValue` preserves exact variant layout and
nullable state through calls, containers, captures, and recursive payloads. The verifier
proves layout/case/field bounds, selected initializer arity and types, receiver identity
and non-nil state, and exact scalar/reference stack maps. The VM still checks the runtime
tag before a selected-case read and traps with `variant case tag mismatch`; it does not
trust the statically expected case tag as a substitute for the runtime guard.

The frontend registers records, named pairs, and variants in one namespace before
resolving their contents. A constructor is known non-nil. Other variant-typed values are
nullable and require the ordinary `is_nil` refinement before matching. `match` is a
statement with immutable payload bindings, exactly one arm per declared case, and
source-order-independent exhaustive checking. The compiler emits dispatch in declared
case order, guards every payload read with a tag check, and preserves nominal identity at
all boundaries.

## Consequences

- Each object occupies only its active case's exact width while retaining self-contained
  metadata sufficient for precise descriptor validation.
- Scalars and inactive-case layouts cannot retain or rewrite accidental object-ID bit
  patterns.
- Immutable payloads remove a mutator barrier surface; promotion-created edges remain
  covered by the common descriptor-driven generational path.
- Nominal layout indices keep recursive signatures finite and make same-shaped variant
  declarations incompatible by construction.
- Exhaustive source matches and verifier-checked guarded reads agree with the VM's runtime
  mismatch trap rather than assuming frontend proofs can replace runtime validation.

## Rejected alternatives

A max-width union allocation was rejected because every object would reserve the largest
case and make compaction width depend on inactive payload capacity. Conservative scanning
of every active tagged slot, every case bitmap, or object-ID-looking scalar was rejected
because scalar bits and inactive cases are deliberately opaque. Mutable payloads were
rejected because they would require case-aware store opcodes, width stability rules, and a
new write-barrier funnel without adding expressive power beyond construction and match.

Structural variant typing was rejected because same-shaped declarations would lose source
identity and recursive equality would require a separate coinductive relation. Trusting a
statically expected tag at `VariantGet` was rejected because malformed raw modules and
heap corruption must trap deterministically instead of reading the wrong payload shape.
Per-opcode or per-collector case scans were rejected because they would split strong-edge
authority away from the single descriptor visitor.
