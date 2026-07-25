# Language Runtime: Verified Bytecode and Precise Moving GC

Wave 3 is complete, and source-level generic functions now monomorphize entirely in the
frontend. Every used type tuple becomes ordinary verifier-checked bytecode, so explicit
tail-call frame reuse, records, sum types and match, exceptions, ephemerons, deterministic
incremental marking, and deterministic incremental compaction all retain the same precise
moving-collector contract.

This repository is a correctness-first implementation of a small statically typed
language. Source passes through a lexer, recursive-descent parser, flow-sensitive type
checker, bytecode compiler, verifier, and stack VM. The runtime uses a precise moving,
two-generation mark-compact collector with deterministic stress scheduling.

The public source entry point, `lang::frontend::compile_program`, returns executable code
only as an immutable `lang::VerifiedModule`. The verifier has already proved stack depth,
local initialization, call signatures, return types, branch joins, and exact per-PC root
maps before the VM accepts that product. The complete pipeline and object model are in
[ARCHITECTURE.md](ARCHITECTURE.md); the properties that changes must preserve are in
[INVARIANTS.md](INVARIANTS.md).

## Architectural crux

### Verified-module discipline

Compiler output and hand-built modules pass through the same module verifier. A
`VerifiedModule` owns immutable accepted bytecode and its matching verification result;
execution reuses that proof without weakening instruction-boundary assertions. Raw
`Module` and `Function` compatibility entry points reverify at execution. See
[ARCHITECTURE.md](ARCHITECTURE.md#runtime-scaffold) and
[ADR 0001](adr/0001-architecture-crux.md).

### Stack maps

Every reachable program counter has exact operand-stack and local reference bits.
Definite initialization is tracked separately from root capability, so control-flow joins
cannot make an unreadable local readable or turn a maybe-object slot into a scalar guess.
The VM traces mutable slots in every active and suspended frame and rewrites them before
execution resumes. See [INVARIANTS.md](INVARIANTS.md#vm) and
[ARCHITECTURE.md](ARCHITECTURE.md#protected-invariants).

### Generic function monomorphization

`fn id<T>(value: T) -> T { value }` can be called explicitly as `id<i64>(5)` or
with unambiguous argument-based inference as `id(5)`. The frontend deterministically
deep-clones each first-used concrete tuple, shares equal tuples, checks every clone with
the existing flow-sensitive checker, and gives it an ordinary direct function index.
The verifier, VM, heap, and collector never see a type variable. A stable depth-32 guard
rejects unbounded polymorphic recursion while equal self and mutual recursion close on
the existing instance. See
[ADR 0018](adr/0018-generic-functions-via-monomorphization.md).

### Tail-call frame reuse

`return tail f(args);` is an explicit terminal transfer to a directly named function.
The verifier requires an argument-only stack and exact caller/callee return agreement.
At that pc, outgoing arguments are the precise roots; every dying local and the frame
closure is cleared before scheduled GC work, after which the callee is installed in the
same frame. Self and mutual tail recursion therefore do not consume call depth, while
ordinary calls retain the deterministic depth trap. Tail calls inside an active try
region are rejected. See [ADR 0016](adr/0016-tail-call-frame-reuse.md).

### Capture maps

Closures snapshot captured locals by value in deterministic first-use order. Each closure
layout carries a verifier-derived reference bitmap; scalar captures remain opaque even if
their bits resemble an object ID, while mapped captures are traced and forwarded. See
[ADR 0003](adr/0003-closure-capture-maps.md).

### Record layouts

Top-level records are nominal fixed-width objects. Each module layout records the ordered
field signatures and an exact per-field reference bitmap; the shared descriptor visits
only mapped fields, so interleaved scalar payload bits remain opaque. All field mutation
uses one pre-publication barrier funnel. See
[ADR 0010](adr/0010-record-layouts.md).

### Variant layouts

Top-level variants are nominal immutable tagged objects. Every case has its own exact
payload width and reference bitmap; the retained raw tag selects the sole bitmap scanned
by the shared descriptor. Exhaustive matches introduce immutable bindings, and guarded
payload reads preserve exact stack maps across movement. See
[ADR 0011](adr/0011-variant-layouts.md).

Typed exceptions use nominal variants: `throw error;` transfers a proven non-nil variant
through explicit VM frames to `try { ... } catch (error: Error) { ... }`. Catch matching
uses exact nominal layout identity, the in-flight object remains a precise moving-GC root,
and existing runtime traps remain uncatchable. See
[ADR 0012](adr/0012-exception-unwind-roots.md).

### Descriptor-driven precision

One object descriptor visitor defines every strong heap edge. It visits pair fields,
reference-array elements, mapped closure captures, statically reference-typed map slots,
bitmap-selected record fields, and active-case bitmap-selected variant payloads; scalar
arrays, scalar record/variant fields, inactive cases, and immutable string bytes expose no
edges. Marking, forwarding, remembered-set validation, and
post-collection validation use that same authority. See [INVARIANTS.md](INVARIANTS.md#gc) and
[ADR 0002](adr/0002-immutable-string-representation.md).

### Barriers

All reference-publishing mutations pass through heap-owned funnels for pair fields,
reference-array elements, record fields, and map entries. An old-to-young store records
its owner before publication. Immutable strings and closures have no mutator barrier path;
promotion records descriptor-declared closure, record, or variant edges created by the
collector itself. See
[ARCHITECTURE.md](ARCHITECTURE.md#generational-collection) and
[ADR 0004](adr/0004-deterministic-insertion-order-maps.md).

### Deterministic map lookup

Maps keep their ordered entry vector as the sole payload and insertion-order iteration
authority. Lookup uses a private half-load open-addressing index whose buckets contain
only entry positions. Fixed FNV-1a hashes consume explicit `i64`/`bool` encodings or
immutable string bytes; object IDs, addresses, library hashes, and per-process seeds never
participate. The index therefore survives atomic/incremental movement without forwarding,
adds no reference edge, and cannot change the ADR-0007 mutation-during-iteration trap.
Every mutation and collection validates exact index/vector coherence. See
[ADR 0017](adr/0017-deterministic-content-hashed-map-index.md).

### Weak phase

Weak targets are the sole non-descriptor edge category. An exact slot-ordered registry
locates every live `WeakRef`; after liveness and forwarding are fixed, the collector
forwards surviving targets and clears dead targets to canonical `nil`. Weak edges never
mark, run barriers, or enter the remembered set. See
[ADR 0005](adr/0005-weak-references.md).

Ephemerons use an exact slot-ordered registry with an immutable weak key and a
key-conditional value. Source exposes `ephemeron<K, V>`, `ephemeron(key, value)`,
`.key()`, `.value()`, and `.set_value(value)`. Reference getter results are nil-able and
use the ordinary `is_nil` refinement. See [ADR 0013](adr/0013-ephemeron-fixpoint.md).

### Incremental marking

Major marking can be split into deterministic object-scan budgets at instruction
boundaries. The existing barrier-before-publish funnels enforce no-black-to-white with an
incremental-update barrier; weak clearing and ephemeron fixpoint completion remain at the
final liveness boundary. A final current-root remark guarantees
stop-the-world-equivalent liveness.
See [ADR 0014](adr/0014-incremental-marking.md).

### Incremental compaction

Major survivors slide in deterministic source order under integer object budgets at VM
instruction boundaries. An exact full-ID read barrier resolves moved source identities
without weakening stale-generation traps; precise roots are rewritten after every move.
All ordinary validators remain live at every boundary, and an independent shadow atomic
slide proves the final heap graph. See
[ADR 0015](adr/0015-incremental-compaction.md).

## Language at a glance

```text
type List = pair<i64, List>;

fn sum(xs: List) -> i64 {
  let total: i64 = 0;
  if is_nil(xs) {
    total = 0;
  } else {
    total = xs.left + sum(xs.right);
  }
  total
}

let empty: List = nil;
let xs: List = pair(20, empty);
xs = pair(22, xs);
let words: [str] = ["gc", "vm", "gc"];
let counts: map<str, i64> = map<str, i64>();
for word in words {
  if counts.has(word) {
    counts[word] = counts[word] + 1;
  } else {
    counts[word] = 1;
  }
}
print("sum=" + to_str(sum(xs)));
counts["gc"]
```

The language includes:

- `i64`, `bool`, immutable byte `str`, typed and opaque pairs, nullable named recursive
  pair types, nominal recursive records with ordered mutable fields, nominal recursive
  variants with immutable tagged cases and exhaustive matching, scalar/reference
  arrays, insertion-order maps, weak references, and structural function types;
- named functions, recursion, first-class function values, returned lambdas, and
  immutable capture snapshots; generic function templates support explicit or inferred
  concrete type arguments, while explicit direct `return tail f(args);` supports
  constant-frame self and mutual recursion;
- local, pair-field, record-field, array-element, and map-entry assignment;
- `if`/`else`, `while`, array/map/range `for-in`, and nearest-loop `break`/`continue`;
- string concat, equality, byte indexing, byte length, copying `sub`, unsigned byte-wise
  ordering, and explicit `to_str`/`to_i64` conversions;
- deterministic bounded `print(str)` output captured by the VM rather than written to a
  host stream.

Eight executable programs live in [examples](examples): a named recursive linked list,
a capture-snapshot accumulator factory, insertion-order word frequencies, a higher-order
array pipeline, string/conversion tools, a recursive record showcase, and a recursive
variant showcase, plus a generic monomorphization showcase. Each `.lang` file
has a byte-pinned `.expected` output. `lang_examples` compiles every file and runs it under
both no stress and maximum combined major/minor/allocation/barrier stress.

Weak clearing is not presented as a source example because the language has no explicit
collection primitive and [ADR 0005](adr/0005-weak-references.md) makes clearing
schedule-dependent. Dedicated weak-reference tests and the weak fuzz grammar pin both
forwarding and clearing at controlled collection boundaries.

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The acceptance gate contains 44 CTest targets.

Run only the executable documentation:

```bash
ctest --test-dir build -R '^lang_examples$' --output-on-failure
```

The project has no standalone source interpreter or REPL. Tests and embedders load source
text through `compile_program` and execute the returned `VerifiedModule`.

## Fuzzing and invariant protection

The suite has 20 isolated deterministic positive corpora. The new `generics` stream is
isolated, and all 19 legacy source/bytecode streams remain byte-identical. Shared source
grammars run under the same 15 schedules:

- `no_stress`
- `before_every_alloc`
- `after_every_alloc`
- `major_every_1`, `major_every_3`, `major_every_7`
- `minor_every_1`, `minor_every_4`
- `minor_after_every_barrier`
- `incremental_1`, `incremental_3_1`
- `combined`
- `incremental_compact_1`, `incremental_compact_3_1`
- `combined_mark_compact`

The two independent observables are the canonical returned value/ID-free deep heap graph
and the exact VM output bytes. The weak grammar additionally uses schedule-specific
liveness expectations because clearing is intentionally observable. Generated source and
bytecode are constructive and deterministic; each grammar has its own corpus dump and an
embedded pinned representative snapshot.

Negative testing applies 73 positioned mutation forms across fixed seed sets, for 1,462
frontend rejection checks. The operators cover type errors, undefined values, arity and
return mismatches, missing nil refinement, invalid container operations, closure misuse,
map key/value errors, weak-target errors, loop-control errors, conversion errors, and
substring/ordering and nominal-record errors, plus invalid tail position/targets and
generic inference, concrete-instantiation, and polymorphic-recursion failures.

Run every corpus through CTest with the build command above. Run the fuzz executables
directly to see their per-corpus summaries:

```bash
./build/lang_iteration5_fuzz
./build/lang_iteration10_source_fuzz
./build/lang_iteration25_maps
./build/lang_iteration26_weak_source_fuzz
./build/lang_iteration28_for_in
./build/lang_iteration29_output
./build/lang_iteration31_strings2
./build/lang_iteration33_records_fuzz
./build/lang_iteration34_variants_fuzz
./build/lang_iteration35_exceptions_fuzz
./build/lang_iteration36_ephemerons_fuzz
./build/lang_iteration38_incremental_compaction_fuzz
./build/lang_iteration39_tail_calls_fuzz
./build/lang_iteration41_generics_fuzz
```

### Replay by grammar

Use one of the schedule names above.

| corpus | seeds | positive replay | mutant replay |
| --- | ---: | --- | --- |
| bytecode `single` | 64 | `./build/lang_iteration5_fuzz --seed N --schedule NAME` | none |
| bytecode `calls` | 64 | `./build/lang_iteration5_fuzz --grammar calls --seed N --schedule NAME` | none |
| bytecode `arrays` | 64 | `./build/lang_iteration5_fuzz --grammar arrays --seed N --schedule NAME` | none |
| source `legacy` | 48 | `./build/lang_iteration10_source_fuzz --seed N --schedule NAME` | `./build/lang_iteration10_source_fuzz --seed N --mutant 0..2` |
| source `recursive` | 48 | `./build/lang_iteration10_source_fuzz --grammar recursive --seed N --schedule NAME` | `./build/lang_iteration10_source_fuzz --grammar recursive --seed N --mutant 0..3` |
| source `array` | 48 | `./build/lang_iteration10_source_fuzz --grammar array --seed N --schedule NAME` | `./build/lang_iteration10_source_fuzz --grammar array --seed N --mutant 0..3` |
| source `strings` | 48 | `./build/lang_iteration10_source_fuzz --grammar strings --seed N --schedule NAME` | `./build/lang_iteration10_source_fuzz --grammar strings --seed N --mutant 0..4` |
| source `closures` | 48 | `./build/lang_iteration10_source_fuzz --grammar closures --seed N --schedule NAME` | `./build/lang_iteration10_source_fuzz --grammar closures --seed N --mutant 0..4` |
| source `maps` | 10 | `./build/lang_iteration25_maps --grammar maps --seed N --schedule NAME` | `./build/lang_iteration25_maps --grammar maps --seed N --mutant 0..5` |
| source `weak` | 32 | `./build/lang_iteration26_weak_source_fuzz --grammar weak --seed N --schedule NAME` | `./build/lang_iteration26_weak_source_fuzz --grammar weak --seed N --mutant 0..3` |
| source `loops` | 10 | `./build/lang_iteration28_for_in --replay --seed N --schedule NAME` | `./build/lang_iteration28_for_in --replay --seed N --mutant 0..6` |
| source `output` | 32 | `./build/lang_iteration29_output --grammar output --seed N --schedule NAME` | `./build/lang_iteration29_output --grammar output --seed N --mutant 0..3` |
| source `strings2` | 32 | `./build/lang_iteration31_strings2 --grammar strings2 --seed N --schedule NAME` | `./build/lang_iteration31_strings2 --grammar strings2 --seed N --mutant 0..3` |
| source `records` | 32 | `./build/lang_iteration33_records_fuzz --grammar records --seed N --schedule NAME` | `./build/lang_iteration33_records_fuzz --grammar records --seed N --mutant 0..6` |
| source `variants` | 32 | `./build/lang_iteration34_variants_fuzz --grammar variants --seed N --schedule NAME` | `./build/lang_iteration34_variants_fuzz --grammar variants --seed N --mutant 0..8` |
| source `exceptions` | 32 | deterministic fifteen-schedule sweep in `lang_iteration35_exceptions_fuzz` | typed rejection cases in `lang_iteration35_exceptions` |
| source `ephemerons` | 32 | `./build/lang_iteration36_ephemerons_fuzz --seed N --schedule INDEX` | six fixed type mutants |
| source `incremental_compaction` | 32 | `./build/lang_iteration38_incremental_compaction_fuzz --grammar incremental_compaction --seed N --schedule NAME` | `./build/lang_iteration38_incremental_compaction_fuzz --grammar incremental_compaction --seed N --mutant 0..3` |
| source `tailcalls` | 32 | `./build/lang_iteration39_tail_calls_fuzz --grammar tailcalls --seed N --schedule NAME` | `./build/lang_iteration39_tail_calls_fuzz --grammar tailcalls --seed N --mutant 0..3` |
| source `generics` | 32 | `./build/lang_iteration41_generics_fuzz --grammar generics --seed N --schedule NAME` | `./build/lang_iteration41_generics_fuzz --grammar generics --seed N --mutant 0..11` |

For example:

```bash
./build/lang_iteration25_maps --grammar maps --seed 7 --mutant 4
./build/lang_iteration31_strings2 --grammar strings2 --seed 12 --schedule combined
```

Dump every deterministic corpus for byte-identity checks:

```bash
./build/lang_iteration5_fuzz --dump-corpus single
./build/lang_iteration5_fuzz --dump-corpus calls
./build/lang_iteration5_fuzz --dump-corpus arrays
./build/lang_iteration10_source_fuzz --dump-corpus legacy
./build/lang_iteration10_source_fuzz --dump-corpus recursive
./build/lang_iteration10_source_fuzz --dump-corpus array
./build/lang_iteration10_source_fuzz --dump-corpus strings
./build/lang_iteration10_source_fuzz --dump-corpus closures
./build/lang_iteration25_maps --dump-corpus maps
./build/lang_iteration26_weak_source_fuzz --dump-corpus weak
./build/lang_iteration28_for_in --dump-corpus loops
./build/lang_iteration29_output --dump-corpus output
./build/lang_iteration31_strings2 --dump-corpus strings2
./build/lang_iteration33_records_fuzz --dump-corpus records
./build/lang_iteration34_variants_fuzz --dump-corpus variants
./build/lang_iteration35_exceptions_fuzz --dump-corpus exceptions
./build/lang_iteration36_ephemerons_fuzz --dump-corpus ephemerons
./build/lang_iteration38_incremental_compaction_fuzz --dump-corpus incremental_compaction
./build/lang_iteration39_tail_calls_fuzz --dump-corpus tailcalls
./build/lang_iteration41_generics_fuzz --dump-corpus generics
```

The isolated `tailcalls` dump is pinned at SHA-256
`72e0af127c314f7bfa4ceb3961170b452fc490e5ea9f31fcfb6643cddebeaf61`.
It compares non-empty canonical graph and output oracles across all 15 schedules; four
mutants prove the tail-position, direct-target, and return-signature gates are active.

The isolated `generics` dump is pinned at SHA-256
`8885efba70fb5788ae1486efd05453c46bf3a3e782bab73e801279b6778b350e`.
Its 32 seeds compare non-empty canonical graph and output oracles across all 15 schedules,
reverify every concrete function set, and run 12 positioned rejection mutants per seed.

The generator design, oracle, schedules, and replay behavior are detailed in
[docs/gc-stress-mode.md](docs/gc-stress-mode.md).

## Benchmarks

`lang_bench` separates deterministic work counters from machine-specific wall-clock
measurements:

```bash
./build/lang_bench --counters-only
./build/lang_bench --repetitions 7
./build/lang_bench --bench map_lookup_heavy --repetitions 7
./build/lang_bench --smoke --repetitions 1
```

The current documented object-kind capture used assertions-enabled Debug builds on the
recorded Apple Clang/macOS host. After the accepted first-fit interval sweep, seven-run
medians were 13.844 ms (`string_heavy`), 111.861 ms (`closure_heavy`), 84.943 ms
(`map_heavy`), 1.440 ms (`weak_heavy`), and 8.625 ms (`mixed_graph`). These values are
informational, not test thresholds.

The measured optimization target was `map_heavy`: storage-occupancy header examinations
fell from 306,009,624 to 8,002,102 and its median fell from 1,580.233 ms to 84.943 ms, a
94.6% reduction. Every deterministic counter outside the three allocator-search work
counters remained byte-identical. The full protocol, original workload counters, host
context, and before/after tables are in
[docs/perf-baseline.md](docs/perf-baseline.md) and
[ADR 0006](adr/0006-performance-capstone.md).

Iteration 40 added `map_lookup_heavy` only after the existing workloads were shown to
under-measure steady lookup. The isolated workload measured linear scanning at
58.1%–65.7% of total time. The deterministic content-hash index reduced candidate
comparisons from 982,336 to 8,069 (99.2%) and its seven-run median from 278.967 ms to
153.575 ms (44.9%, 1.816x). On the broader `map_heavy` workload, comparisons fell from
41,809 to 951 and the same-host median moved from 133.031 ms to 121.033 ms (9.0%).
Timings remain informational; the exact counters, host context, and corpus proof are in
[docs/perf-baseline.md](docs/perf-baseline.md).

## Limitations and deferred work

- Strings are immutable byte sequences, not Unicode text. There is no mutation,
  interning, interpolation, rope, or substring-view representation; `sub` copies.
- Maps have deterministic expected-O(1) content-hashed lookup and insertion-order
  iteration, but no deletion. Tombstones, shrink policy, and structural hash caching are
  deferred.
- Closure captures are immutable snapshots. There are no mutable capture cells or
  recursion through a self-capture.
- Tail calls are explicit and direct-only. There is no implicit `Call; Return`
  optimization or tail-call opcode for function values and closures.
- Generic functions are monomorphized and direct-call-only as templates. Generic named
  aliases, records, and variants are deferred until nominal recursive layout
  instantiation has its own finite deterministic identity proof.
- Weak-keyed maps, finalizers, resurrection, mutable weak targets, and user-visible
  finalization ordering are not implemented.
- Loop control is unlabeled. Labeled loops and labeled `break`/`continue` require a label
  namespace and explicit target-resolution rules.
- Blocks are statements rather than expressions, and `let` declarations are limited to a
  function or program body rather than nested blocks.
- The repository exposes an embeddable compiler/runtime API and tests, not a command-line
  interpreter, package system, debugger, JIT, or native-code backend.

These cuts keep the verifier/collector agreement explicit. New features must extend the
relevant invariant, verifier proof, deterministic replay surface, and ADR rather than
bypassing them.
