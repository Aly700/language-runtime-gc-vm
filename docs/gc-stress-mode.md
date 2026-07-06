# GC Stress Mode

Future phases should support deterministic collection at these points:

- before every allocation
- after every allocation
- every N bytecode instructions
- after every write barrier

Each mode must be seed/replay friendly and must not depend on wall-clock timing.
