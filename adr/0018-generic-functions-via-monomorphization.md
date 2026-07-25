# ADR 0018: Frontend-Only Generic Function Monomorphization

## Status

Accepted for Iteration 41.

## Context

The compile boundary established by the pair, recursive named-type, closure, record,
variant, exception, and tail-call iterations is deliberately concrete. A
`SignatureValue` describes an exact finite runtime value shape or nominal layout
identity; verifier-generated stack maps then classify every operand and local without
conservative roots. The VM, heap, and collector have no type-variable semantics.

Parametric functions are useful at the source level, but carrying type variables across
that boundary would broaden every trusted runtime layer and make array representation,
closure capture maps, record descriptors, calls, and exception paths depend on runtime
generic metadata. Iteration 41 instead requires each used type application to become an
ordinary, independently checked concrete function before compilation.

## Decision

The source language accepts generic function declarations such as
`fn id<T>(value: T) -> T { value }`, with any positive number of comma-separated type
parameters. A type parameter is lexically available throughout that declaration's
parameter and return types, local and catch annotations, nested pair/array/map/function/
weak/ephemeron positions, explicit type arguments, and nested lambda signatures. It
shadows a nominal type of the same spelling inside the template.

Calls may provide a complete concrete tuple, as in `id<i64>(5)`, or omit it and request
first-order inference from argument types. Inference recursively matches pair, array,
map, function, weak, and ephemeron structure and exact named/record/variant identities.
Every occurrence of one parameter must infer the same concrete type. Inference does not
use expected return types, unfold nominal types, search overloads, or solve constraints.
A missing, nil-only, structurally conflicting, or otherwise ambiguous binding is a
positioned type error ending with an instruction to use explicit type arguments. A
generic template is not a first-class runtime function and a bare reference is rejected.

`TypeSpec::TypeParameter` exists only in the frontend AST. It carries declaration-order
identity and source spelling. A generic template is stored separately from ordinary
functions and is never emitted. At a direct call, the flow-sensitive checker resolves or
infers a complete concrete tuple and consults an insertion-ordered instantiation
registry. On first use it deep-clones the complete function AST, substitutes every type
position, routes the clone through the ordinary concrete type-resolution restrictions,
and then runs the existing flow-sensitive function checker. This second resolution step
is required: restrictions such as `map<K, V>`, `weak<T>`, and
`ephemeron<K, V>` are deliberately deferred when the restricted position is a direct
type parameter, then enforced after substitution.

Every concrete clone receives an ordinary direct function index and closure-layout
index. Calls, explicit tail calls, captures, exception handlers, array representation
selection, record/variant identity, map and ephemeron layouts, and stack-map generation
then use the unchanged compiler. `Program::generic_functions` remains frontend-only;
`Program::functions` presented to lowering contains only original non-generic functions
and checked concrete clones. No opcode, `Module` table, `SignatureValue`, verifier rule,
VM dispatch path, heap representation, root category, write barrier, or collector path
is added.

Concrete tuples use a canonical recursive encoding. Scalar types have fixed tags; pair,
array, function, map, weak, and ephemeron types encode their ordered children with
explicit boundaries; named, record, and variant types encode their resolved table index
and spelling. Source positions, allocation addresses, pointer identity, hash iteration,
and host state do not participate. The registry key is the generic declaration identity
plus the length-delimited tuple encoding. The internal clone name is the source name plus
`$mono$` and that same tuple encoding. Registry lookup is linear and deterministic;
equal keys share the same clone, index, code, and layouts.

Original non-generic functions retain indices `1..N` in their previous relative order.
Instances begin at `N+1`. The checker visits ordinary bodies in declaration order and
then the entry body, evaluates calls and arguments left-to-right, and checks a first-use
clone immediately. First use is therefore depth-first: if `outer<i64>` first requests
`inner<i64>`, those receive adjacent indices before checking resumes in the caller.
Lambdas are recollected only after the concrete function set closes, in the existing
post-order over ordinary functions, concrete instances, and entry. This preserves legacy
function and layout order while making generic closure order deterministic.

Registry lookup precedes recursion accounting. A call to an already registered key,
including an in-progress self or mutual cycle, closes on that function and consumes no
new instantiation depth. A genuinely new key requested beyond depth 32 is rejected with
the stable diagnostic:

`generic instantiation depth limit of 32 exceeded while instantiating '<name>';
possible polymorphic recursion`

This makes closed generic recursion finite while rejecting self or mutual polymorphic
growth loudly and deterministically instead of hanging or exhausting host resources.

Checking is instantiation-site sound. Every emitted clone passes the existing concrete
flow-sensitive checker and the module verifier, and every attached operand/local stack
map must round-trip through a second verifier pass. Definition processing still proves
declaration hygiene, duplicate-free parameters, and known nominal types. A never-used
template is not symbolically constraint-solved; an operation valid for only some `T`
values is accepted or rejected when those concrete values are requested.

Concrete lowering retains exact GC precision. For example, `singleton<i64>` emits a
scalar array and `singleton<pair<i64, i64>>` emits a reference array;
`getter<i64>` has a false capture bit while an object instantiation has a true bit; and
generic functions accepting distinct nominal records preserve each record's exact
layout and scalar/reference field bitmap. Scalars are never conservatively exposed as
object IDs. The isolated `generics` grammar runs 32 seeds under all 15 existing movement
schedules, compares canonical graph and output oracles, and runs 12 rejection mutants
per seed. Its exact corpus dump SHA-256 is
`8885efba70fb5788ae1486efd05453c46bf3a3e782bab73e801279b6778b350e`.

Generic named aliases (`type Box<T> = ...`) and generic record or variant declarations
are deferred. The existing named-type table is a finite nominal recursion graph and
record/variant identities are fixed declaration layouts. Supporting parameterized
nominal recursion requires a separate demand-instantiation graph, coinductive key
closure, and deterministic layout identity allocation. Adding partial syntax without
that proof would risk infinite expansion or conflating distinct layouts. Generic
functions can still use existing concrete named recursive, record, and variant types as
type arguments.

## Consequences

- Legacy source contains neither generic headers nor explicit generic calls, so its AST,
  bytecode, table ordering, fuzz streams, observables, and benchmark counters remain
  byte-identical.
- Code size grows once per distinct used concrete tuple. This is deterministic and
  bounded by source demand plus the depth guard; unused templates emit nothing.
- Diagnostics inside a concrete body retain template source positions, while tuple
  resolution, arity, inference, and recursion-limit errors point at the requesting call.
- The runtime has no generic reflection or shared polymorphic function value. All
  generated functions are ordinary direct functions and obey existing call, tail-call,
  closure, exception, verification, and collection rules.

## Rejected alternatives

- Runtime type variables or dictionary passing were rejected because they require new
  runtime metadata, verifier semantics, dynamic representation selection, and collector
  trust.
- Erasure with boxed scalar values was rejected because it destroys the exact scalar
  versus reference representation and root-map invariant.
- Substitution only during code generation was rejected because it would bypass the
  flow-sensitive checker and could reuse incompatible annotations across instances.
- Eager Cartesian expansion was rejected because the concrete type universe is
  demand-defined, unused combinations would perturb indices and tables, and recursive
  expansion would be unbounded.
- An unordered instantiation cache or mangling based on addresses or host hashes was
  rejected because generated indices and diagnostics must be reproducible.
- Inferring from expected return type was rejected because it introduces context-sensitive
  constraint solving where explicit type arguments already provide a deterministic
  fallback.
- Implementing generic named recursive types in this iteration was rejected because
  nominal layout instantiation is a separate recursion and identity problem, not a safe
  extension of function cloning.
