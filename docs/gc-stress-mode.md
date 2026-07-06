# GC Stress Mode

The VM/heap support deterministic collection at these points:

- before every allocation
- after every allocation
- after every write barrier
- every N bytecode instructions as a major collection
- every N bytecode instructions as a minor collection

Each mode must be seed/replay friendly and must not depend on wall-clock timing.

Implementation details:

- Configuration lives in `lang::gc::StressConfig`.
- `Heap` owns allocation stress and traces registered roots plus temporary allocation
  operands/the new object so collection cannot sweep values between VM pop and push.
- `Heap` also owns after-barrier stress. When
  `collect_minor_after_every_write_barrier = true`, a minor collection runs immediately
  after a store that actually records an old object for a young reference.
- `VM` owns instruction-count stress. For `collect_every_n_instructions = N`, major
  collection runs at bytecode boundaries after N, 2N, 3N, ... instructions have executed
  and before the next instruction starts.
- For `collect_minor_every_n_instructions = N`, minor collection runs at those same
  deterministic verified bytecode boundaries.
- VM-controlled collection points assert, in debug builds, that the generated verifier stack
  map for the current pc agrees with the runtime stack's object tags before collecting.
- Every stress-triggered major or minor collection runs the same root/field/reference and
  remembered-set validation as explicit collection. The after-barrier mode specifically
  protects the invariant that old-to-young stores cannot bypass `Heap::store_pair_field`.

## Differential GC-timing fuzzing

`lang_iteration5_fuzz` mechanically searches deterministic bytecode programs against a
fixed roster of GC schedules. The generator is seeded by an explicit splitmix64-style
`uint64_t` PRNG in the test binary; it does not use wall-clock time, host randomness,
threads, or unordered iteration. Each generated program is a structured counted loop that
allocates shared pairs, promotes roots with `Collect`, creates young loop objects, forms a
self-cycle with `SetLeft`, stores young objects into an old pair field through
`SetLeft`/`SetRight`, drops filler allocations to create compaction holes, and returns an
object reachable through locals held across collections. The generator tracks its abstract
operand stack and locals while emitting bytecode, then asserts the existing verifier
accepts the generated `Function`; a rejected generated program is treated as a generator
bug, not as a skipped fuzz case.

The CTest corpus runs seeds `1..64` across these schedules:

- `no_stress`
- `before_every_alloc`
- `after_every_alloc`
- `major_every_1`, `major_every_3`, `major_every_7`
- `minor_every_1`, `minor_every_4`
- `minor_after_every_barrier`
- `combined` (`before_every_alloc`, `after_every_alloc`, `major_every_7`,
  `minor_every_4`, and `minor_after_every_barrier`)

The oracle executes the same generated program under every schedule and compares the
observable return value to `no_stress`. Scalar returns compare by tag and value. Object
returns compare a canonical deep graph serialization from the returned object: traversal
assigns schedule-local node numbers in left-before-right order, records pair fields, and
preserves sharing and cycles through repeated node references instead of comparing
`ObjectId` values. Any divergence, verifier rejection, unexpected trap, stale id, or GC
validator failure prints the seed, schedule, full bytecode listing, and a one-line replay
command.

Replay one finding with:

```bash
./build/lang_iteration5_fuzz --seed <uint64> --schedule <schedule-name>
```

The test also pins the full generated program listing for seed `17`, so accidental
generator nondeterminism or drift fails before the corpus run.
