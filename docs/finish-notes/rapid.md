# rapid / muttest

Concrete interpreter first. A body it cannot score used to come back as a raw
parse error, or as `install gcc or clang` when gcc was simply absent from
`PATH`, even though this stage never started a compiler. A mutant that could
not be scored was not a killed mutant, but the Law 9 hold on the compiled
fallback was dropped instead of written down.

## What this slice does

- Muttest and rapid, when the interpreter cannot score a body and
  `--allow-exec` is off: one `NOTRUN` row, not a missing row. `extra.install`
  is the exec hint, `extra.reason` is `executes-scanned-code`, `extra.exec`
  is `NOTRUN`. The message is `cannot evaluate {fn}: ` plus that hold and the
  interpreter text (a `ParseFail` is no longer the whole message).
- The compiler is not invoked on that path, and the row does not say
  `install gcc or clang`. That hint is only for an error that records a
  compiler lookup that failed (`no gcc/clang on PATH`).
- Survived-mutant evidence, and the rapid counterexample evidence, is
  `strip` then at most 400 characters.
- With `--allow-exec`, the same interpreter failure compiles a small
  `main` with `gcc` or `clang` (`-O0 -std=c11`) and runs that binary through
  `run_argv` + `sandbox::wrap_argv` + `limits_for`. No new dependencies.
  Compile failure, timeout, crash, or a short stdout is `ERROR` (or `NOTRUN`
  when the compiler is absent). It is not `CLEAN` and not `PROVED`.
- A mutant the samples actually catch is still `CLEAN`, and that is not a
  proof. This stage does not emit `PROVED`.

## Not in this slice

Python's last-resort tiny interpreter (`_interpret`, `_exec_block`,
`_eval_c_expr`) is not ported. `interp.cpp` is the first scorer. Shrinking a
counterexample still uses that interpreter, so a body only the gcc harness
could execute is reported from the original sample and is not shrunk.
