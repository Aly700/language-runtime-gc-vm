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
