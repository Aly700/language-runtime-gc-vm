# Invariants — Language Runtime

## VM

- Bytecode verification must prove stack depth is valid at every instruction.
- A bytecode instruction may only read locals proven initialized by the verifier.
- A call instruction may only target an in-module function whose declared parameter kinds
  match the values proven on the caller stack.
- A return instruction may only return a value whose kind matches the current function's
  declared return kind.
- A `TailCall` is a terminal direct transition. The verifier must prove that its target
  needs no captures, its complete return signature exactly matches the current function,
  and its operand stack contains exactly the conforming outgoing arguments with no other
  value. It has no ordinary or exceptional successor.
- Every live VM frame's operand stack and locals are precise mutable roots. Moving
  collection must rewrite references in active and suspended frames before bytecode
  execution resumes.
- Generated per-pc stack maps carry exact reference bits for both operand-stack slots and
  ordinary local slots. Definite initialization remains separate: a loop header may mark
  a local reference-capable because it is `Nil` on entry and an object on a backedge while
  still rejecting `LoadLocal` until every incoming path initializes it. The VM asserts
  both operand and local bits at every active-frame instruction boundary.
- At a TailCall pc, the stack map is deliberately a dying-frame boundary map: outgoing
  argument bits remain exact roots and every local bit is false. Before the first map
  assertion or collection at that boundary, the VM must overwrite all dying locals with
  canonical `nil` and clear the frame closure. No stale slot from the old invocation may
  be visible to atomic collection, incremental marking, or incremental compaction.
- Call depth is bounded by an explicit VM limit and must trap deterministically before
  host stack exhaustion can matter. `Call` and `CallClosure` consume that depth;
  verifier-accepted `TailCall` replaces the current frame and does not.
- Tail transfer may install the callee only after any boundary collection has rewritten
  the outgoing argument slots. Reuse must leave an empty operand stack, a zero pc, a
  callee-sized `nil`-initialized local vector with parameters in locals `0..N-1`, and no
  inherited closure or handler state before callee execution.
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
- `MapKeyAt` and `MapValueAt` are append-only positional accessors used by compiler-lowered
  map iteration. The verifier requires a typed map receiver plus an `i64` index and derives
  the exact result from the map layout. Runtime out-of-bounds access traps with the stable
  `map entry index out of bounds` diagnostic; lowered iteration deliberately reaches that
  boundary when the entry count grows after its entry snapshot.
- `AllocRecord`, `RecordGet`, and `RecordSet` may execute only after the verifier has
  proved the module record-layout identity, field bounds, declared-order initializer
  types, receiver identity and non-nil state, and stored-value type. Record signatures
  retain exact nominal layout identity through calls, containers, captures, and recursive
  fields, and their stack/local slots carry exact reference bits.
- `AllocVariant`, `VariantTag`, and `VariantGet` may execute only after the verifier has
  proved exact nominal layout identity, case and field bounds, selected-case payload
  types, receiver non-nil state, and scalar/reference stack maps. `VariantGet` also checks
  the runtime raw tag and traps with `variant case tag mismatch` rather than trusting the
  statically expected case.
- `Throw` consumes only a proven non-nil nominal variant. Verified handler ranges add
  exceptional dataflow edges whose target stack is exactly that one reference; handler
  locals are joined from exceptional predecessors and ordinary flow cannot enter a handler.
- A TailCall inside an active same-frame try range is invalid. An exception from a
  tail-called callee therefore unwinds the reused callee frame and may be caught only by
  an eligible suspended outer frame.
- Runtime traps are never translated into language exceptions. During iterative unwind,
  the in-flight exception is exactly one mutable `pending_exception_` root. Moving
  collection rewrites it and every surviving frame root before another frame is removed
  or a handler resumes.
- VM observable behavior must not depend on host pointer addresses.
- The VM output buffer is execution-local copied byte state, never a heap root. Its
  contents are a pure function of the verified module and its inputs and must be
  byte-identical across every GC stress schedule. Collection, forwarding, barriers, and
  validation never read or rewrite output bytes.

## GC

- Incremental major marking consumes deterministic integer budgets measured in complete
  descriptor object scans. No marking trigger or budget depends on wall-clock time,
  threads, host addresses, payload bytes, or field count.
- During an incremental cycle, no black object may have a descriptor-declared edge to a
  white object. Every mutable reference-publishing funnel shades a white target before a
  black owner publishes it; newly allocated objects enter the grey worklist. The same
  descriptor visitor is the sole strong-edge authority for stepping and validation.
- Final incremental-marking completion re-traces current precise roots and derives the
  same live set as atomic stop-the-world major marking at that boundary. Ephemeron
  fixpoint and weak/ephemeron death decisions complete before any incremental movement;
  a combined schedule may then transfer that fixed live set directly into compaction.
- Every collector-owned object ID must participate in movement. Map-growth relocation
  forwards an active incremental grey worklist together with roots, descriptor edges,
  remembered entries, WeakRef/ephemeron registries, and weak intern-table
  canonicals.

- Incremental marking and incremental compaction are mutually exclusive. Compaction
  starts only after final major liveness, ephemeron activation, dead weak-target clearing,
  dead-owner pruning, and dead-header sweep have completed deterministically.
- An incremental compaction budget counts complete survivor relocation units in ascending
  source-slot order. The saved full source ID, descriptor width, full destination ID, and
  destination slot are immutable; no clock, thread, random choice, byte count, field count,
  address, or unordered iteration may influence a step.
- During incremental compaction, checked dereference accepts a physically current full ID
  or an exact saved source ID with installed forwarding. Slot-only forwarding is
  forbidden. A genuinely stale generation must trap before, during, and after the phase,
  and source IDs must become stale after final canonical rewriting.
- Each relocation copies the complete object before clearing an overlapping source,
  installs the predetermined destination generation before publishing forwarding,
  partially rewrites every precise root and collector owner registry, records
  promotion-created old-to-young edges, clears no additional liveness, and runs all
  collection validators before the mutator resumes.
- Fixed-width mutations during compaction must update the authoritative moved or unmoved
  owner through checked dereference. Reference values are canonicalized before the
  barrier-before-publish funnels. Allocations, new map entries, map growth, and explicit
  collections must finish compaction first with every reference operand temporarily
  rooted. `intern` follows the same rule even on a possible hit: it finishes compaction
  with the source as an extra root before consulting the table.
- The final incremental-compaction graph must equal an independently recomputed atomic
  slide of the post-liveness source snapshot, including generations, descriptor-selected
  fields, scalar payloads, WeakRef and ephemeron fields/registries, the weak intern table,
  remembered-set pruning, handles, VM roots, and explicit roots. Shadow mutations update
  source fields directly; they may not copy production destination objects.

- Every live object is reachable from an explicit root or another live object at collection start.
- The collector must never treat non-reference values as references.
- Every reference relationship belongs to exactly one of two exhaustive edge categories.
  Descriptor visitors define **all strong edges**: `Pair`, `RefArray`, `Closure`, `Map`,
  and `Record` expose exactly their statically declared reference slots. Collector-owned
  WeakRef targets, ephemeron weak keys, and intern-table canonicals remain in the existing
  **weak-edge category** and are processed only by their deterministic post-mark rules.
  No third reference path may mark, retain, forward, clear, or validate an object ID.
- Reference-bearing variable-length strong payloads must use the same descriptor visitor
  as fixed-size strong payloads. Marking, strong forwarding, remembered-set validation,
  and strong post-collection validation may not add one-off object-kind scans outside
  that path. Weak-category references are the sole extension: they are processed only
  through their registry/table-driven post-mark phase and never through a descriptor
  visitor.
- `WeakRef` is a fixed-width object with one collector-owned target slot. Its descriptor
  visits zero strong fields, so neither the WeakRef object nor its registry entry keeps
  the target alive. The mutator initializes a non-nil object target exactly once through
  `AllocWeak`; no target setter, store opcode, or write-barrier path exists. After
  construction, only the collector may forward or canonically clear the slot.
- The weak registry contains every live WeakRef owner exactly once in ascending heap-slot
  order and no other object. Allocation inserts in slot order; movement rewrites owner
  IDs; collection prunes dead owners. `validate_weak_targets` proves registry exactness
  and that every target is canonical `Nil` or a current generation-valid object ID.
- Weak processing runs after liveness and the forwarding table are fixed and before the
  moved heap is installed. Live owners are visited in registry/old-slot order: targets
  with forwarding entries are rewritten and targets without them are cleared to `Nil`.
  A cleared slot is never matched against later allocation and stays cleared across slot
  reuse.
- The collector-owned intern table stores only `(FNV-1a content hash, canonical Str ID)`
  weak entries. It is never traced as a root, descriptor edge, remembered-set edge, or
  ephemeron value. Entries are unique by immutable bytes and remain in strict canonical
  base-slot order. Lookup checks the fixed string-domain content hash and then structural
  bytes; no ID, slot, generation, address, library hash, or process seed influences a
  match.
- Intern weak processing runs beside WeakRef processing after liveness is fixed. A
  canonical with forwarding survives under its forwarded ID; a canonical without
  forwarding causes the complete entry to be evicted. Major, minor, map-growth, atomic
  sliding, and incremental sliding must preserve this rule. During incremental marking,
  a hit publishes a weak canonical as a strong result and therefore enqueues it on the
  ordinary grey worklist.
- `validate_intern_table` must run after insertion and at every collection, relocation,
  incremental-step, and finalization boundary. It proves current generation-valid `Str`
  targets, strict slot order, recomputed FNV/content agreement, and structural uniqueness.
  An uncleared entry may return its still-physical weak-only canonical between collections;
  once returned, that value is an ordinary precise strong root.
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
- The ordered map entry vector is the sole authority for payload, positional access,
  tracing order, insertion-order iteration, and mutation-during-iteration entry-count
  checks. Its deterministic lookup index stores only encoded entry-vector positions:
  never a `Value`, `ObjectId`, pointer, handle, or owner. Index buckets are scalar metadata
  outside both reference-edge categories; they may not mark, retain, forward, clear,
  barrier, or validate a reference, and they do not change logical heap width.
- Map hashes are fixed 64-bit FNV-1a over a domain byte plus an explicit content encoding:
  eight little-endian `uint64_t` bytes for `i64`, canonical `0`/`1` for `bool`, and
  immutable bytes for `str`. No object identity, slot, generation, address, host
  endianness, library hash, random seed, clock, or process state may influence a bucket.
  Hash collisions always fall back to the existing tagged/content equality.
- Every map lookup index must be the exact deterministic half-load, power-of-two
  open-addressing rebuild of its ordered entries. The coherence validator runs after
  every map mutation and at every heap-layout/collection validation boundary, proving
  capacity, unique and complete entry indices, probe reachability, and exact buckets.
  Atomic/incremental movement and map-growth relocation copy the scalar index unchanged;
  forwarded immutable string IDs cannot change their content-derived buckets.
- `Record` payloads are mutable ordered tagged field slots. Each heap record retains its
  validated module layout index and the exactly-derived per-field reference bitmap. The
  descriptor visits only bitmap-selected fields in declaration order; scalar fields stay
  opaque even when their payload bits equal a live, dead, stale, or forwarded `ObjectId`.
  Shape validation requires the field count and bitmap length to agree and every slot tag
  to match its static reference bit; reference fields may carry `Object` or `Nil`.
- `Variant` payloads are immutable active-case tagged slots. Each heap variant retains its
  validated nominal layout index, raw case tag, active fields, and a full copied per-case
  bitmap table. Its descriptor validates the tag and selected width, then visits only true
  bits in the selected case; inactive cases and scalar active fields expose no edges.
- Object IDs name object base slots only. Payload/reserved storage slots are never valid
  object headers, and variable-size compaction must advance by the descriptor storage
  width without allowing overlapping live objects.
- No object may be swept while reachable.
- If a moving collector is introduced, every root and heap reference must be updated before mutator execution resumes.
- Every live embedder handle is a precise mutable root slot; handle destruction removes that slot before the next collection, and the heap must outlive all handles.
- Write barriers must run on every old-to-young reference store once generations exist,
  including `Pair` field stores, `RefArray` element stores, and statically reference-typed
  `Record` field stores. Raw `ScalarArray` and scalar-record-field stores are not
  reference-publishing mutations and must not enter the remembered set; immutable strings
  cannot publish references at all.
- Every map insertion or update flows through `Heap::store_map_entry`. Before publishing
  an inserted young string key or a young reference value into an old map, that single
  funnel records the old owner in the remembered set. Updates barrier the replacement
  value and preserve the original key slot and insertion position. No mutable map payload
  is exposed outside this funnel. A new insertion prepares any resized index before
  appending, publishes the ordered entry and its encoded index exactly once, then validates
  their coherence; an update changes no bucket. Remembered-set validation must reject
  every omitted old-map-to-young edge.
- A map's logical storage width is `1 + 2 * current_entry_count` and may grow only between
  collections. Every allocator, storage-layout validator, growth relocation, compaction
  cursor, and forwarding pass computes that same width from the current entry count. The
  collector asserts that an object's width is unchanged within one collection. If an
  append cannot claim the adjacent two logical slots, deterministic relocation rewrites
  all roots, descriptor-declared references, and remembered-set entries before the entry
  is published. The host-side lookup-index capacity is not part of this width and cannot
  affect allocation, movement order, or compaction byte accounting.
- Every record mutation flows through `Heap::store_record_field`. The funnel validates the
  owner's immutable layout identity, field bounds, bitmap shape, and stored tag; before
  publishing a young object in a reference field of an old record it records the owner in
  the remembered set. No mutable record payload is exposed outside this funnel, and
  remembered-set validation must reject every omitted old-record-to-young edge.
- A record's logical storage width is permanently `1 + field_count`. Allocation,
  storage-layout validation, compaction cursors, and forwarding all derive the same width
  from the retained layout payload and assert that it does not change during collection.
  Field mutation may replace a slot but can never add, remove, or reorder one.
- A variant's logical storage width is exactly `2 + active_field_count`: header, raw tag,
  and selected payload. Allocation, storage validation, compaction, and forwarding derive
  that same case-dependent width. Payloads have no post-construction mutation path.
- Closure captures and variant payloads have no post-construction store path and therefore
  no mutator barrier. If collector promotion creates an old closure, variant, or record
  with a descriptor-selected young edge, the generic descriptor-driven promotion path
  itself
  must insert the owner into the remembered set before collection-boundary validation.
  This GC-internal insertion must be exact, deterministic, and must not count as a mutator
  write-barrier hit. Record mutator-created edges still require the ordinary store funnel.
- Weak edges never enter the remembered set and never run a write barrier. Minor
  collection finds old WeakRef owners through the exact weak registry, not through
  strong-edge remembered-set scanning. A young weak target survives only if a strong
  minor root marks it; survivors are promoted/forwarded and dead young targets clear.
- `Ephemeron` descriptors expose zero strong fields. Their exact slot-ordered registry
  owns an immutable weak key and a conditional value. A value is marked only when its key
  is independently live; registry scans and descriptor draining repeat to a deterministic
  fixpoint. Each productive pass marks a new object, so finite monotone marking terminates.
- Ephemeron fixpoint marking precedes movement. Active keys/reference values forward;
  inactive entries clear to canonical nil/nil and never resurrect. Minor collection
  treats old keys as independently live and young keys as live only when marked.
- Every ephemeron value mutation flows through `Heap::store_ephemeron_value`, which
  validates scalar/reference shape and performs the old-to-young barrier before publish.

## Frontend

- Top-level `record Name { field: T, ... }` declarations introduce nominal object types.
  All declarations are registered before fields are resolved so self and forward
  references remain finite. Two records with identical field lists are distinct types;
  complete nominal identity must survive parameters, returns, arrays, maps, closure
  captures, weak targets, and field reads/writes.
- Record construction must supply every field exactly once in declared order. A constructed
  record is proven non-nil, while record-typed parameters, recursive fields, and other
  nullable values require the existing `is_nil(local)` false-branch refinement before
  field access. All record writes preserve the selected field's declared type.
- At the compile boundary, `i64` and `bool` record fields must emit scalar bitmap bits and
  every other field must emit reference bits. The compiler asserts that this emitted
  bitmap exactly matches the type checker's classification before module verification.
- Top-level variants introduce nominal nullable types in the same declaration namespace.
  Constructors are non-nil; exhaustive matches require a refined non-nil scrutinee, cover
  every case exactly once, and expose immutable arm-local bindings. Per-case bitmap bits
  use the same `i64`/`bool` scalar and all-other-types reference rule.
- `throw e;` requires a proven non-nil nominal variant. `try { ... } catch (e: V) { ... }`
  catches exact nominal variant layout `V`; the catch binding is non-nil, immutable, and
  scoped to its handler body. Unmatched exceptions propagate.
- `return tail f(args);` is the sole explicit tail syntax. It is terminal, valid only
  inside a function or lambda and outside an active try body, and `f` must be a directly
  named function whose argument and complete return types match. `return` is reserved;
  `tail` remains a contextual identifier so legacy uses of that name are unchanged.
- Generic function, named-alias, record, and variant declarations are frontend templates,
  not module functions or declarations. `TypeSpec::TypeParameter` must be eliminated by
  a complete concrete substitution before function indexing, bytecode signature
  emission, named-type/layout emission, verifier invocation, or VM execution. A generic
  function template cannot be used as a first-class value, and a generic type template
  cannot be used without its exact concrete arity.
- An omitted generic type tuple is inferred only by deterministic first-order matching
  from argument types. Every type parameter must receive one unambiguous equal concrete
  binding; missing, nil-only, or conflicting evidence is rejected with an explicit-type-
  argument fallback. Inference never depends on an expected return type or nominal
  unfolding.
- Every first-use generic clone must pass the same concrete type-resolution restrictions
  and flow-sensitive checker as an ordinary declaration. This includes restrictions
  deferred at the template for direct `map<K, V>`, `weak<T>`, and
  `ephemeron<K, V>` positions, the pair-body rule for named aliases, exact record field
  layout derivation, and exact per-case variant payload layout derivation. Every function
  clone then participates in the ordinary compiler/verifier stack-map agreement round
  trip.
- Generic instantiation keys are canonical concrete type tuples plus declaration
  identity. Lookup and allocation are insertion ordered: original non-generic functions
  and declarations keep their indices, new instances are checked depth-first in
  left-to-right first-use order, and an equal key shares one identity, body, and layout.
  A type instance reserves its ordinary concrete identity before its substituted body is
  resolved. Registry lookup must therefore close existing same-key structural recursion
  before the deterministic depth-32 new-key limit is applied; genuinely growing keys
  must reach the stable polymorphic-recursion diagnostic.
- Concrete instantiation must preserve representation precision rather than erase or box:
  scalar and reference array opcodes, closure capture bits, complete pair signatures, and
  named recursive identities plus nominal record/variant layout identities are derived
  independently for each concrete clone. Every record field and every tagged variant
  payload bit is exact for that instantiation. No type variable or conservative root may
  cross the compile boundary.
- Structural `fn(T1, ..., Tn) -> R` types must survive the compile boundary in locals,
  parameters, returns, pair fields, record fields, and reference-array elements. Lambda
  captures are immutable creation-time snapshots in deterministic first-use order; later
  assignment to the source local cannot alter an existing closure.
- Every type-checked lambda and named-function value must lower through a verifier-accepted
  closure layout whose ordered capture types and derived bitmap agree exactly. Function
  arrays must use `RefArray`, and closure-valued stack slots must carry the existing precise
  object-root bit.

- `str` values compile to the distinct verifier `Str` kind and runtime `ObjectKind::Str`.
  Literals decode into the per-module constant pool; concat, equality, length, and byte
  indexing must preserve the compile-boundary agreement invariant and string stack slots
  must be marked as precise object roots.
- `StrSub` copies a half-open byte range into a fresh `Str`; its source remains a precise
  mutable root until the allocating operation completes. `StrLt` is unsigned byte-wise
  lexicographic ordering, and every source string ordering operator lowers only through
  `StrLt` plus deterministic boolean inversion.
- `intern(e)` accepts exactly a proven non-nil `str` and lowers only to append-only
  `StrIntern`. The verifier proves `str -> str`; its operand stack slot is a precise root
  across the possible canonical-copy allocation. While a canonical is strongly live,
  every equal call must return that exact object. The table itself must never extend
  liveness.
- String indexing is read-only and returns an unsigned byte widened to `i64`. The frontend
  must reject indexed string assignment before bytecode generation.
- `print(e)` accepts exactly `str` and appends its bytes plus one newline to the bounded VM
  output log. `to_str` accepts exactly `i64` or `bool`; `to_i64` accepts exactly `str` and
  traps on every non-canonical or out-of-range spelling. Frontend overload resolution and
  verifier opcode types must agree.
- `map<K, V>` accepts only `i64`, `bool`, or `str` for `K`; the type checker and compile
  boundary enforce exactly the same restriction as the verifier. `map<K, V>()`, indexed
  get/set, `.has(key)`, and `.len` preserve complete nested `K`/`V` facts through function
  signatures, pair fields, arrays, captures, and map values. String keys hash and compare
  by immutable bytes, never by `ObjectId`, so lookup remains valid after moving
  collection.
- `for x in array`, `for k, v in map`, and `for i in lo..hi` are statement-only forms
  lowered to index-based control flow and ordinary compiler-managed locals. Loop bindings
  are immutable and lexically scoped; hidden names contain characters the lexer cannot
  produce. Array and map sources must be proven non-nil before lowering, using the same
  `is_nil` refinement boundary as existing object operations. Container references remain
  precise local roots across every loop boundary.
- Unlabeled `break` and `continue` bind only the nearest enclosing while/for-in and lower
  exclusively to existing jumps. Break targets the post-loop pc; for-in continue must
  execute the hidden index increment before returning to the header, and map continue must
  re-enter through the mutation check. Every target retains verifier-exact operand and
  local root bits.
- Array iteration snapshots length but loads each element through the live forwarded
  container, so element writes are visible. Map iteration snapshots entry count, observes
  ADR-0004/ADR-0007 insertion order directly from the ordered entry vector, never from the
  lookup index, sees existing-value updates, and traps if a new key grows the map. Range
  bounds are evaluated once left-to-right and `[lo, hi)` executes zero times when
  `hi <= lo`. Compiler and verifier must agree on every hidden local and boundary map.

- A source array type's element type determines its runtime representation at the compile
  boundary: `[i64]` and `[bool]` must emit only scalar `AllocArray`/`ArrayGet`/`ArraySet`
  operations, while `[pair<...>]`, named-pair arrays, record arrays, and nested array
  elements must emit only `AllocRefArray`/`RefArrayGet`/`RefArraySet`.
- The frontend must never compile a `nil` or maybe-nil value into a RefArray element.
  Reference array literals enumerate every element, and sized reference array
  construction requires a non-nil initializer expression.
- Indexing a typed reference array must recover the declared element type in frontend and
  verifier metadata even though the runtime value is a coarse object reference.
- `weak<T>` accepts only object types (`pair`/named pair, record, array, map, `str`, or
  `fn`).
  `weak(x)` requires a proven non-nil object and lowers to `AllocWeak`; `.get()` lowers to
  `WeakGet` and produces nil-able `T`. Every object operation, argument, store, or return
  requires the existing `is_nil(local)` false-branch refinement first. `WeakIsAlive` is
  intentionally absent so `is_nil` remains the single refinement mechanism.
- `ephemeron<K, V>` requires object-typed `K`; construction requires a proven non-nil key
  and non-nil initial value. Structural `K,V` identity survives calls, containers, and
  captures. `.key()` and reference `.value()` produce nil-able results requiring
  `is_nil` refinement, while `.set_value(v)` lowers only to `EphemeronSetValue` and the
  heap's barrier-before-publish funnel.

## Testing

- GC stress mode must be deterministic and able to collect before or after any allocation.
- A test that passes with GC disabled is not sufficient for runtime correctness.
- Differential fuzz executions compare two independent observables across schedules: the
  canonical returned heap graph/value and the VM output byte log. Both must match exactly.
- The isolated 32-seed `tailcalls` source corpus runs both observables under all 15
  schedules and has its own pinned source/outcome snapshots and corpus dump. Its mutants
  must prove that syntax, direct-target, tail-position, and return-signature gates are
  non-vacuous without changing any legacy corpus stream.
- The isolated 32-seed `interning` source corpus runs both observables under all 15
  schedules and pins its representative source, representative outcome, and full dump.
  Its returned graphs must expose canonical sharing, while positioned i64/bool/pair/nil
  mutants prove the builtin's exact type gate without changing any of the 21 legacy
  corpus streams.
- The isolated 32-seed `generics` source corpus runs both observables under all 15
  schedules, reverifies every concrete function set, and has independent source,
  graph/output, and exact corpus pins. Its 12 mutants cover inference, explicit arity,
  first-class misuse, instantiation-site body restrictions, invalid concrete container
  parameters, and polymorphic recursion without changing any of the 19 legacy streams.
- The isolated 32-seed `generic-types` source corpus runs both observables under all 15
  schedules, reverifies every concrete module, and pins its representative source,
  graph/output result, and complete corpus. It must exercise scalar and object instances
  of aliases, recursive records, recursive variants, nesting, generic functions, exact
  record/case maps, and barriered record mutation. Its 12 positioned mutants cover
  unbound parameters, arity, non-generic application, non-closing recursion, nominal
  separation, payload typing, and non-exhaustive generic-variant matching without
  changing any of the 20 legacy streams.
