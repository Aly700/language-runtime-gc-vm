# GC Stress Mode

The VM/heap support deterministic collection at these points:

- before every allocation
- after every allocation
- every N bytecode instructions

Future generational phases should add:

- after every write barrier

Each mode must be seed/replay friendly and must not depend on wall-clock timing.

Phase-1 implementation details:

- Configuration lives in `lang::gc::StressConfig`.
- `Heap` owns allocation stress and traces registered roots plus temporary allocation
  operands/the new object so collection cannot sweep values between VM pop and push.
- `VM` owns instruction-count stress. For `collect_every_n_instructions = N`, collection
  runs at bytecode boundaries after N, 2N, 3N, ... instructions have executed and before
  the next instruction starts.
