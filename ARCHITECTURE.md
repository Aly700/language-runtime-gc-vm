# Architecture — Language Runtime

## Pipeline

```text
source -> lexer -> parser -> AST -> type checker -> compiler -> verifier -> VM -> heap/GC
```

## Frontend surface

`lang::frontend::compile_program` is the narrow public entry point. It returns either a
`lang::Module` with verifier-generated per-function stack maps attached, or deterministic
diagnostics with byte offsets plus 1-based line/column positions. For compatibility with
single-entry tests, the result also exposes a copy of the entry `lang::Function`.

Frontend implementation files are split by pipeline stage while keeping
`include/lang/frontend/type_checker.hpp` as the only public frontend API:
`src/frontend/lexer.*` owns tokenization, `parser.*` owns AST definitions and parsing,
`type_checker_internal.*` owns flow-sensitive typing and diagnostics,
`compiler.*` owns bytecode emission plus the verifier-agreement assertion, and the thin
`type_checker.cpp` wires those stages into `compile_program`.

The source language is intentionally small:

- Types are `i64`, `bool`, opaque `pair`, and finite parametric `pair<T, U>`.
- Top-level `let name: type = expression;` declarations introduce initialized locals.
- Assignment supports locals and `pair` fields (`left` and `right`).
- Expressions include i64/bool literals, variables, `+`, `<`, `pair(left, right)`, field
  access, and parentheses.
- `if condition { ... } else { ... }` and `while condition { ... }` are statement forms.
- Function declarations use `fn name(a: i64, b: pair<i64, pair>) -> pair<i64, bool> { ... }`.
  Function bodies have the same shape as the top level: statements followed by a final
  expression.
- Calls are expressions and recursion is allowed.
- A program ends with a final expression, which becomes the VM result.

Minimal cuts: there are no nil literals, strings, expression-valued blocks, user-defined
types, named recursive types, first-class functions, or block-local `let` declarations.
Bare `pair` is an opaque pair leaf type: it proves only "object" at function boundaries,
so field reads through bare pair parameters/returns are rejected as unknown. Inside a
function, the type checker still tracks pair field types flow-sensitively by allocation
site, so old local pair-field reads and cyclic structure construction remain expressible.
`pair<T, U>` carries field types in declarations and function signatures; construction,
assignment, calls, and returns must conform structurally to those declared fields.

Rejected alternative: add named recursive pair types. That would make self-referential
shapes statically expressible, but it would require a larger type language and recursive
subtyping rules. The current design keeps cyclic runtime structures expressible by storing
through opaque `pair` leaves while giving signatures enough finite structure for safe
interprocedural field reads.

## Runtime scaffold

- `lang::Module` stores a designated entry function plus all callable `lang::Function`
  bodies. Each function carries a `FunctionSignature` of coarse parameter/return kinds,
  optional detailed `SignatureValue` pair-field types, bytecode, local count, and optional
  stack maps.
- `lang::verify` is the bytecode safety gate. It runs a per-function worklist dataflow
  analysis over reachable bytecode, proving stack depth before every instruction,
  `LoadLocal` initialization over all incoming paths, branch target validity, call
  argument kinds/counts against the module signature table, return kind against the
  current function signature, and value kinds for generated stack maps. This protects the
  VM invariants for stack depth, initialized local reads, and function boundaries across
  loops and merge points.
- Verifier rejection is structured. The compatibility APIs `verify` and
  `verify_with_stack_maps` still return only bool/optional results, while
  `verify_with_diagnostics` returns the same stack-map result plus deterministic
  `VerifierDiagnostic` entries carrying function index, optional pc, stable
  `VerifierReason`, and message text. The reason-code catalog is documented next to
  `VerifierReason` in `include/lang/bytecode.hpp`; consumers print the first diagnostic
  before throwing or asserting.
- `lang::VM` owns a vector of call frames and registers itself as a `lang::gc::RootProvider`.
  Each frame owns its own operand stack and locals. Root tracing visits mutable `Value`
  slots in every live frame, not copied root values, so moving collection can update
  active and suspended frames before mutator execution resumes.
- `lang::gc::Heap` implements deterministic major and minor mark-compact collection over
  generation-tagged object IDs. The low bits identify the storage slot and the high bits
  identify that slot's current generation, so a swept or moved ID cannot alias a later
  object in the same slot.
- Pair field types are verification-time metadata only. They do not change `Value`,
  object layout, stack-map object/non-object bits, root tracing, write barriers,
  forwarding, movement, or heap validation.
- Pair field mutation is routed through `Heap::store_pair_field`. That method is the
  single hook point where the generational old-to-young write barrier runs before the new
  field value is published.
- Deterministic GC stress is configured with explicit counters: before every allocation,
  after every allocation, after every barrier-triggering old-to-young store, every N
  major-collection bytecode instructions, and every N minor-collection bytecode
  instructions. Every stress collection runs the same post-collection reference validation
  as explicit collections. No stress trigger depends on wall-clock time, randomness,
  threads, or host addresses.

## Functions and frames

Function 0 is the compiled top-level entry. Source `fn` declarations are emitted as
functions 1..N, and `Call` operands are direct callee indexes. Parameters occupy locals
0..N-1 in the callee and are initialized when the VM pushes the frame.

The verifier is modular at function boundaries: it does not import caller allocation-site
state into the callee and does not export callee allocation-site state back to the caller.
Only the module signature table crosses a call boundary. This is the soundness boundary:
calls prove parameter count/kind compatibility and, when present, detailed
`pair<T, U>` field-kind compatibility. A typed pair parameter enters the callee as an
object with signature-derived field facts; a typed pair return pushes an object with the
callee's declared return field facts. A bare pair parameter/return remains opaque, so
field reads still reject unless allocation-site facts are established inside the function.

The VM uses per-frame operand stacks rather than a single shared stack segment. This keeps
root rewriting direct: `trace_roots` walks every frame and visits each stack/local slot by
mutable reference. On `Call`, arguments are popped from the caller and copied into callee
locals; the caller's pc is advanced to the return pc before suspension. Suspended caller
stacks therefore do not necessarily match a public stack map until the callee returns and
the result has been pushed, so runtime stack-map assertions are enforced for the active
frame at instruction boundaries. The design does not assert suspended-frame maps because
their in-call continuation stack is between verifier-visible states.

Call depth is checked by an explicit `VM::set_max_call_depth` limit before pushing a frame,
so recursion traps deterministically with a VM error instead of depending on the host C++
stack.

## Source/Verifier agreement

Well-typed source must compile to a module accepted by `verify_with_stack_maps`. A verifier
rejection of type-checked compiler output is a compiler bug, not a user error.
`compile_program` enforces this by running module-level `verify_with_stack_maps`, asserting
success, then attaching the generated maps to every emitted function and asserting the maps
round-trip through the verifier.

Compiler accommodations for verifier strictness:

- Local initialization: every source `let` has an initializer and compiles to
  `initializer; StoreLocal` before any `LoadLocal` for that local can be emitted.
- Stack discipline: statements are stack-neutral. Field assignment emits receiver then
  value then `SetLeft`/`SetRight`, which consumes both and pushes nothing.
- Bool literals: the bytecode has no Bool constant opcode, so `true` and `false` compile to
  constant i64 comparisons whose result kind is proven `Bool`.
- Branches and loops: conditions must type-check as `bool` because `JumpIfFalse` consumes a
  verifier-proven Bool. `if` emits an explicit jump over `else`; `while` emits a reachable
  header, false exit, stack-neutral body, and backedge.
- No fall-off-end: the final source expression is followed immediately by `Return`.
- Reachability: the compiler emits no bytecode after `Return`, and branch targets are
  patched only to emitted instruction boundaries reachable through verifier dataflow.
- Function boundaries: calls emit arguments left-to-right followed by `Call <callee>`.
  Function signatures initialize parameter locals in the verifier and VM, and `Return`
  must match the declared return kind plus any detailed pair field types.
- Pair fields: the type checker uses the same allocation-site join idea as the verifier
  and also attaches finite `pair<T, U>` details to function signatures. A field can be read
  only when all possible allocation sites and signature facts agree on the field kind;
  field assignment through typed pairs must preserve the declared field kind.

## Design bias

The heap uses object IDs instead of raw host pointers so movement can be represented as
reference rewriting instead of host pointer patching. An `ObjectId` is a move-sensitive
capability to the current storage location, not stable numeric identity. Language-level
object identity survives movement because the collector rewrites every mutator-visible
reference to the survivor's new `ObjectId` before bytecode execution resumes; stale copied
IDs outside the traced root set remain invalid and trap loudly. External C++ code that
needs to keep a heap reference across collection must hold a `lang::gc::Handle`; raw
`ObjectId` or `Value` copies outside a root slot are intentionally not movement-safe.

## Embedder root handles

`lang::gc::Handle` is the narrow C++ embedder API for holding a heap reference across
moving collection. A handle is created only by `Heap::make_handle(Value)` or
`Heap::make_handle(ObjectId)`, validates object IDs before registration, owns one mutable
`Value` slot, and registers that slot in a heap-owned `std::vector<Value*>`. The vector is
traced in registration order after VM/explicit `RootProvider` roots and before temporary
allocation roots, so marking, forwarding, and validation remain deterministic without
pointer-keyed hash containers.

Handles are move-only. Move construction and move assignment transfer the existing root
registration to the destination slot and leave the source handle unusable, so copying
cannot accidentally duplicate a long-lived root. Destruction deregisters the slot; a
destroyed handle is not traced by later collections. `Handle::value()` returns the current
rewritten `Value`, and `Handle::object()` returns the current rewritten `ObjectId`.

The lifetime rule is explicit: a `Heap` must outlive all of its handles. The heap carries a
shared lifetime token for use-after-move/teardown diagnostics, and `Heap::~Heap` aborts if
any handle root slots are still registered. This makes lifetime violations deterministic
under asserts instead of leaving a dangling registration that a later collection could
trace. Handles are roots only; they are not old objects and never participate in the
remembered set.

Rejected alternative: copyable handles with shared ownership of a root table entry. That
would make `Handle h2 = h1` cheap, but it hides whether a copy is meant to extend object
lifetime and makes deregistration depend on reference counts rather than lexical root
ownership. Move-only handles keep C++ root lifetimes visible.

## Moving collection

Collection is mark, forward, rewrite, install, validate:

1. Marking uses an explicit vector worklist. Roots are visited in provider order, pair
   fields are traced left before right, and no unordered container participates in tracing.
2. Compaction scans the slot vector in ascending order and copies marked objects into the
   lowest slots. This deterministic sliding pass builds a forwarding table indexed by old
   slot.
3. A moved survivor receives `new_slot | new_generation << 32`. If it moves into another
   slot, that destination slot's generation is advanced before the new ID is minted, so an
   old stale ID for the destination slot cannot alias the moved object. The old source slot
   is emptied, so the pre-move ID also traps.
4. Before installing the compacted slot vector, the collector rewrites all roots
   (`RootProvider` roots from every VM frame, heap handle slots, plus allocation extra
   roots) and every copied live pair field through the forwarding table while the old
   layout can still validate old IDs.
5. After installation, validation walks all roots and all live-object fields and checks that
   every object reference resolves through the current generation table.

Rejected alternative: a permanent handle-indirection table where ObjectIds never change.
That would preserve external numeric handles, but it would not exercise the root and heap
field rewriting invariant this phase is meant to protect. The current forwarding design
makes missed updates fail as stale IDs.

## Generational collection

The heap has two logical object generations on top of the same slot vector:

- New pair allocations are young.
- Any young object that survives one collection is promoted to old. This one-survival
  policy keeps the phase deterministic and makes promotion independent of wall-clock age or
  allocation rate.
- `Heap::collect()` is the major collector. It traces all objects reachable from mutator
  roots, compacts the whole heap, promotes young survivors, sweeps unreachable old and young
  objects, rewrites all roots and heap fields through the forwarding table, then validates.
- `Heap::collect_minor()` is the minor collector. It traces only young objects, with roots =
  mutator roots plus the deterministic remembered set of old objects that may reference
  young objects. Old objects are retained conservatively until the next major collection,
  but all roots and all retained heap fields are still rewritten through the forwarding
  table before mutator execution resumes.
- The remembered set is a `std::vector<ObjectId>`, not an unordered container. Barrier
  insertion is stable and duplicate-free by linear scan; collection-time pruning rebuilds it
  in slot order by keeping only valid old objects that still contain a young field.
- A dead old object in the remembered set can conservatively keep a young referent alive
  during a minor collection, but never forever: major collection traces only mutator roots,
  so the dead old graph is swept and remembered-set pruning drops the entry.
- `Heap::store_pair_field` is the only mutation hook exposed to bytecode. On every store of
  a young object into an old pair field it records the old object before publishing the
  value. The public heap API does not expose mutable pair fields directly, protecting the
  barrier-completeness invariant.

Rejected alternative: keep old objects fixed during minor collection and compact only young
slots. That would reduce forwarding work, but it would leave minor collection unable to
exercise the same root/heap-field rewrite discipline as major collection when young
survivors slide across holes. The current minor collector preserves old objects
conservatively but still uses one forwarding-table rewrite path for roots, heap fields, and
remembered-set entries.

## Protected Invariants

- Stack safety: the verifier checks each instruction's pop requirements before applying
  its push result, rather than checking only net stack effect.
- Local safety: locals have an explicit verifier state of uninitialized or known value kind;
  `LoadLocal` requires a known initialized kind on every incoming path.
- Root precision: `verify_with_stack_maps` emits per-function per-pc stack object maps from
  the verifier's abstract states. Hand-written maps, when present, must match those
  generated maps, and VM-controlled collection points assert that the active frame's
  runtime stack object tags agree with the generated map for the current function and pc.
- Frame-root precision: every live frame's locals and operand stack slots are traced as
  mutable roots. A collection triggered in a deep callee rewrites references held by
  suspended callers before the callee or caller can resume.
- GC identity safety: dereferencing, marking, forwarding, or validating an invalid/stale
  `ObjectId` throws, exposing root-precision and missed-forwarding bugs instead of silently
  ignoring or aliasing them.
- Moving-collector safety: `RootProvider`/`RootVisitor` traces mutable root slots,
  heap handles expose mutable embedder root slots, allocation stress treats allocation
  operands and the new object as temporary roots, the forwarding pass rewrites all roots
  and live heap fields before mutator execution resumes, and VM collection points assert
  generated stack maps both before and after collection.
- Handle-root safety: every live `lang::gc::Handle` is a deterministic heap root slot
  traced and rewritten by major collection, minor collection, stress collection, and
  post-collection validation; destroying or moving a handle removes or transfers exactly
  that slot registration.
- Generational barrier safety: old-to-young stores are recorded in `Heap::store_pair_field`,
  minor collection traces young objects from mutator roots plus remembered old objects,
  remembered-set entries are rewritten/pruned after movement, and validation traps any
  valid old-to-young field absent from the remembered set.
- Signature safety: call sites are checked against the callee signature only; returns are
  checked against the current function signature. This keeps bytecode verification
  modular and rejects out-of-range calls, wrong arity, wrong argument kind, wrong
  pair-field kind, and wrong return kind.
- GC neutrality: pair-field types never participate in runtime stack maps or heap layout.
  Stack maps still record only object/non-object, and the VM/heap continue to trace,
  barrier, forward, and validate by runtime `Value` tags.
- Source agreement: `compile_program` does not return bytecode for rejected source, and
  every returned module has passed `verify_with_stack_maps` with generated stack maps.

## Verifier join lattice

- Stack height is invariant at a merge; different heights reject immediately to protect
  stack-depth safety.
- Stack value kinds are `Int64`, `Bool`, `Object`, `Nil`, and `Poison`.
- Equal non-object kinds join to themselves; object kinds join to object with the union of
  possible allocation sites, an opaque-object bit, and optional signature-derived pair
  fields.
- Different kinds join to `Poison`, which cannot be consumed by any instruction or emitted
  into a stack map. This rejects ambiguous object/non-object roots instead of guessing.
- Local initialization joins by intersection: a local initialized on only one incoming path
  becomes uninitialized at the merge, so later `LoadLocal` rejects.
- Abstract pair fields join by allocation site and field kind. Mutating an allocation-site
  field joins the previous abstract field with the stored value because one allocation site
  may represent multiple runtime objects. Mutating through a typed signature object first
  checks the stored value against the declared field kind.
- Field reads merge signature-derived fields with allocation-site fields. If any possible
  receiver is opaque, or if the merged field kind becomes poison, the read rejects rather
  than guessing a root/non-root kind.
- Rejected alternative: coercing differing kinds to a permissive `Any` value. That would let
  typed operations or stack maps treat maybe-object values imprecisely, violating type
  safety and future moving-collector root precision.
