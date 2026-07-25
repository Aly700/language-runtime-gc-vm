# ADR-0019: Generic named types via frontend monomorphization

## Status

Accepted — 2026-07-25

## Context

ADR-0018 made generic functions frontend templates and required every type
parameter to disappear before bytecode lowering. It deliberately deferred generic
named aliases, records, and variants because recursive declarations need an
identity before their bodies can be resolved, while records and variants also
need a concrete runtime layout.

The language already has the required concrete machinery:

- named aliases close recursive pair conformance coinductively and use `nil` as
  the empty recursive value;
- records register a fixed field layout and an exact reference bitmap, and all
  field mutation goes through the existing write-barrier path;
- variants register a tagged set of fixed payload layouts with an exact bitmap
  per case, and matching is checked exhaustively against that concrete variant;
- the Iteration 41 registry provides canonical concrete-tuple keys,
  deterministic depth-first first-use order, sharing, and a depth-32 guard for
  non-closing polymorphic recursion.

Putting type variables into the module format or using conservative layouts
would break the central verifier/collector contract. Generic type declarations
therefore need to become ordinary concrete declarations before lowering, just
as generic functions become ordinary concrete functions.

## Decision

The frontend accepts the following declaration forms:

```text
type List<T> = pair<T, List<T>>;
record Node<T> { value: T, next: Node<T> }
variant Option<T> { None(), Some(T) }
```

Generic aliases, records, and variants are template-only AST declarations.
They are registered in their own source-ordered tables and are absent from the
ordinary declaration tables consumed by the compiler. A generic application
may appear anywhere an existing type may appear, including ordinary and
generic function signatures, locals, fields, payloads, element positions, and
another generic type argument.

Each demand instantiation is keyed by declaration identity plus the canonical
ordered tuple of fully concrete type arguments. The registry is shared with the
Iteration 41 instantiation context:

1. Resolve type arguments from left to right and reject unbound variables,
   non-concrete arguments, or an arity mismatch.
2. Look up the canonical key before applying the depth guard.
3. On first use, reserve an ordinary concrete declaration identity and table
   index before resolving the cloned body, fields, or cases.
4. Substitute the type parameters completely, then resolve and validate the
   resulting declaration with the existing concrete rules.
5. Materialize completed declarations in deterministic depth-first first-use
   order. Equal keys share the reserved declaration and table entry.

Reserving the identity first is what closes structural recursion through the
same key. For example, resolving `List<i64>` encounters `List<i64>` again and
reuses the in-progress alias. `Node<str>` and `Tree<i64>` close the same way.
The depth-32 limit is checked only when an application would allocate a new
key. A genuinely growing chain such as:

```text
type Grow<T> = pair<T, Grow<Grow<T>>>;
```

therefore reaches the existing stable diagnostic:

```text
generic instantiation depth limit of 32 exceeded while instantiating
'Grow'; possible polymorphic recursion
```

A concrete generic alias must obey the existing named-type rule and resolve to
a pair. Each concrete record instance registers an ordinary record layout whose
reference bitmap is derived from its substituted field types. Each concrete
variant instance registers ordinary case layouts whose reference bitmaps are
selected by runtime tag. Thus `Node<i64>` leaves `value` opaque while
`Node<object-type>` traces it, and `Option<i64>.Some` and
`Option<object-type>.Some` have different exact case maps.

Concrete record and variant instances retain ordinary nominal identity:
`Node<i64>` and `Node<str>` cannot be assigned to one another. Construction,
field access and mutation, recursive conformance, variant payload typing,
non-nil refinement, and match exhaustiveness all use the existing concrete
checker paths.

The compiler receives only ordinary concrete declarations. This iteration adds
no opcode, bytecode field, verifier state, VM behavior, heap representation,
barrier path, or collector rule. Module declaration/layout tables grow only as
they would for equivalent hand-written concrete declarations.

## Consequences

- Generic type declarations compose with generic functions and with every
  existing concrete type constructor without adding runtime generic metadata.
- Layout precision is per instantiation and per variant tag; scalar payload
  bits are never traced, while object payloads are traced and forwarded under
  marking and compaction.
- Determinism depends on source-order template registration, canonical key
  rendering, lookup-before-guard recursion closure, and depth-first first use.
  Those properties are pinned by a separate `generic-types` corpus.
- An unused generic declaration emits no module entry and cannot change
  bytecode or layout numbering.
- Diagnostics distinguish an unknown type, a non-generic type application, a
  bare or wrong-arity generic application, duplicate/unbound parameters,
  non-closing recursion, payload/field mismatch, and non-exhaustive match.

## Rejected alternatives

- **Runtime type arguments or generic opcodes.** This would expose type
  variables to the module, verifier, VM, and collector and duplicate concrete
  checking at runtime.
- **One erased record or variant layout per template.** A union/conservative
  bitmap would trace scalar bits as object IDs and violate precise GC.
- **Eagerly enumerate instantiations.** The concrete type universe is open and
  recursive; demand instantiation is finite for every accepted program and
  preserves deterministic first-use order.
- **Resolve a recursive body before reserving identity.** Same-key recursion
  would be mistaken for divergence and could not use the existing coinductive
  named-type machinery.
- **Treat every recursive application as closing.** Growing keys such as
  `Grow<Grow<T>>` are not the same concrete instance and would make
  instantiation non-terminating.
- **Structurally merge concrete record or variant instances.** That would
  discard the language's existing nominal layout and exhaustiveness boundary.
