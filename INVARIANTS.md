# Invariants — Language Runtime

## VM

- Bytecode verification must prove stack depth is valid at every instruction.
- A bytecode instruction may only read locals proven initialized by the verifier.
- A call instruction may only target an in-module function whose declared parameter kinds
  match the values proven on the caller stack.
- A return instruction may only return a value whose kind matches the current function's
  declared return kind.
- Every live VM frame's operand stack and locals are precise mutable roots. Moving
  collection must rewrite references in active and suspended frames before bytecode
  execution resumes.
- Call depth is bounded by an explicit VM limit and must trap deterministically before
  host stack exhaustion can matter.
- `AllocClosure`, `CallClosure`, and `LoadCapture` may execute only after the verifier has
  proved their module layout, structural function signature, capture arity/types, and
  closure-body capture index. A closure call uses the same frame stack and deterministic
  call-depth limit as a direct call.
- Every active closure body frame keeps its closure object in a precise mutable root slot.
  Moving collection must rewrite that frame-owned closure reference before `LoadCapture`
  can resume.
- `AllocMap`, `MapSet`, `MapGet`, `MapHas`, and `MapLen` may execute only after the
  verifier has proved the module layout index, restricted key type, complete structural
  key/value types, and receiver/operand agreement. Missing-key `MapGet` traps with the
  stable `map key not found` diagnostic.
- VM observable behavior must not depend on host pointer addresses.

## GC

- Every live object is reachable from an explicit root or another live object at collection start.
- The collector must never treat non-reference values as references.
- Every heap object has a descriptor derived from its header kind and length. The
  collector may inspect only descriptor-declared reference slots: `Pair` scans its two
  tagged `Value` fields, `RefArray` scans every tagged `Value` object-reference payload
  slot, and `ScalarArray` and `Str` scan zero payload slots.
- Reference-bearing variable-length payloads must be expressed through the same
  descriptor visitor as fixed-size objects. Marking, forwarding, remembered-set
  validation, and post-collection validation may not add one-off object-kind scans
  outside that descriptor path.
- `ScalarArray` payload elements are raw `i64` values, not tagged `Value`s. Even if a raw
  element's bit pattern equals a valid or stale `ObjectId`, marking, forwarding,
  remembered-set validation, and post-collection validation must not interpret it as a
  reference or rewrite it.
- `Str` payload elements are immutable raw bytes, not tagged `Value`s. Even if any byte
  sequence has the exact bit pattern of a live, dead, stale, or forwarded `ObjectId`,
  marking, forwarding, remembered-set validation, and post-collection validation must not
  interpret it as a reference or rewrite it. Strings expose no payload-store API and have
  no write-barrier path after construction.
- `Closure` payloads are immutable tagged capture slots plus a raw scalar function index.
  The module verifier derives each layout's capture bitmap from its ordered static capture
  types. The heap descriptor visits exactly bitmap-selected object slots; adjacent scalar
  captures remain opaque even when their payload bits equal live, dead, stale, or forwarded
  `ObjectId`s. Marking, forwarding, remembered-set maintenance, and validation may not
  inspect closure captures outside the descriptor visitor.
- `Map` payloads are mutable ordered `(key, value)` tagged-slot pairs. Each heap map retains
  its verified layout index plus `key-is-ref` and `value-is-ref` flags. The descriptor
  visitor visits exactly the flagged slots in insertion order: string keys and reference
  values are traced and forwarded, while scalar key/value slots remain opaque even when
  their payload bits equal a live, dead, stale, or forwarded `ObjectId`. Descriptor shape
  validation requires the entry count to match the payload and every slot tag to agree
  with its static reference flag; reference values may carry `Object` or `Nil`.
- Object IDs name object base slots only. Payload/reserved storage slots are never valid
  object headers, and variable-size compaction must advance by the descriptor storage
  width without allowing overlapping live objects.
- No object may be swept while reachable.
- If a moving collector is introduced, every root and heap reference must be updated before mutator execution resumes.
- Every live embedder handle is a precise mutable root slot; handle destruction removes that slot before the next collection, and the heap must outlive all handles.
- Write barriers must run on every old-to-young reference store once generations exist,
  including `Pair` field stores and `RefArray` element stores. Raw `ScalarArray` stores
  are not reference-publishing mutations and must not enter the remembered set; immutable
  strings cannot publish references at all.
- Every map insertion or update flows through `Heap::store_map_entry`. Before publishing
  an inserted young string key or a young reference value into an old map, that single
  funnel records the old owner in the remembered set. Updates barrier the replacement
  value and preserve the original key slot and insertion position. No mutable map payload
  is exposed outside this funnel, and remembered-set validation must reject every omitted
  old-map-to-young edge.
- A map's logical storage width is `1 + 2 * current_entry_count` and may grow only between
  collections. Every allocator, storage-layout validator, growth relocation, compaction
  cursor, and forwarding pass computes that same width from the current entry count. The
  collector asserts that an object's width is unchanged within one collection. If an
  append cannot claim the adjacent two logical slots, deterministic relocation rewrites
  all roots, descriptor-declared references, and remembered-set entries before the entry
  is published.
- Closure captures have no post-construction store path and therefore no mutator barrier.
  If collector promotion creates an old closure with a mapped young capture, the promotion
  path itself must insert the closure into the remembered set before collection-boundary
  validation. This GC-internal insertion must be exact, deterministic, and must not count
  as a mutator write-barrier hit.

## Frontend

- Structural `fn(T1, ..., Tn) -> R` types must survive the compile boundary in locals,
  parameters, returns, pair fields, and reference-array elements. Lambda captures are
  immutable creation-time snapshots in deterministic first-use order; later assignment to
  the source local cannot alter an existing closure.
- Every type-checked lambda and named-function value must lower through a verifier-accepted
  closure layout whose ordered capture types and derived bitmap agree exactly. Function
  arrays must use `RefArray`, and closure-valued stack slots must carry the existing precise
  object-root bit.

- `str` values compile to the distinct verifier `Str` kind and runtime `ObjectKind::Str`.
  Literals decode into the per-module constant pool; concat, equality, length, and byte
  indexing must preserve the compile-boundary agreement invariant and string stack slots
  must be marked as precise object roots.
- String indexing is read-only and returns an unsigned byte widened to `i64`. The frontend
  must reject indexed string assignment before bytecode generation.
- `map<K, V>` accepts only `i64`, `bool`, or `str` for `K`; the type checker and compile
  boundary enforce exactly the same restriction as the verifier. `map<K, V>()`, indexed
  get/set, `.has(key)`, and `.len` preserve complete nested `K`/`V` facts through function
  signatures, pair fields, arrays, captures, and map values. String keys compare by bytes,
  never by `ObjectId`, so lookup remains valid after moving collection.

- A source array type's element type determines its runtime representation at the compile
  boundary: `[i64]` and `[bool]` must emit only scalar `AllocArray`/`ArrayGet`/`ArraySet`
  operations, while `[pair<...>]`, named-pair arrays, and nested array elements must emit
  only `AllocRefArray`/`RefArrayGet`/`RefArraySet`.
- The frontend must never compile a `nil` or maybe-nil value into a RefArray element.
  Reference array literals enumerate every element, and sized reference array
  construction requires a non-nil initializer expression.
- Indexing a typed reference array must recover the declared element type in frontend and
  verifier metadata even though the runtime value is a coarse object reference.

## Testing

- GC stress mode must be deterministic and able to collect before or after any allocation.
- A test that passes with GC disabled is not sufficient for runtime correctness.
