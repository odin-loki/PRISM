# Work in progress kept as patches

## pir-targets.patch

The unfinished `pir-targets` branch (commit 2fe3827, based on c1287eb, the #78 head). It checks each unit with its real
`--target` data layout (ILP32 and big-endian) instead of LP64 only. It touches the memory model, the libc models, the
translator and the stage, and adds `tests/cpp/test_pir_targets.cpp`. The agent stopped before building it, so it has
never been compiled, tested or gated. It is not merged.

What main does instead: a unit whose compile_commands.json entry builds for a target that is not LP64 is `NOTRUN`
with "target data layout not supported (checked only for LP64 little-endian)". That covers an ILP32 `--target` or
`-target`, a 32-bit cross compiler such as `arm-linux-gnueabi-gcc`, and `-m32` or `-mabi=ilp32`. 32-bit x86 is
still checked when `--data-model ILP32` asks for it. A big-endian target is `NOTRUN` too (#78). The test is
`pir: a unit built for a target that is not LP64 is not checked` in `tests/cpp/test_pir_header_fns.cpp`.

To continue: `git am docs/wip/pir-targets.patch` on a branch from main (expect conflicts with integration-wave3's
`stage.cpp` prepare/check split). Then build it, pass its tests and run the full soundness gate before it replaces
the stopgap.
