# Adapters slice

Landed on `finish/adapters` from `e59a8b3df`. Owned files:
`src/prism/adapters.cpp`, `tests/cpp/test_adapters.cpp`, this note.
libFuzzer stays in `src/prism/stages/fuse.cpp`.

## cppcheck

A child that does not start is `NOTRUN` with `cppcheck unusable: failed to start <path>`, plus the runner's start-failure text (`RunOut::err`) when there is one. A process that starts and answers like doctest/Catch2 stays `cppcheck at <path> is not cppcheck (not a proof)`.

## ESBMC / CBMC / Dafny

Fake tool output, no real ESBMC or Dafny binary.

- ESBMC plain `VERIFICATION SUCCESSFUL` is `PROVED`.
- ESBMC `INDUCTION` is `PROVED-UNBOUNDED`. It is not folded into `PROVED`.
- ESBMC `UNWINDING ASSERTION` without `--k-induction` (PRISM does not pass that flag) is `BOUNDED`, including when the log also says `INDUCTION`.
- CBMC `VERIFICATION SUCCESSFUL` stays `BOUNDED`, including when the log says `INDUCTION` or unbounded. That mapping was left as it is.
- Dafny still follows the process exit code: 0 is `PROVED`, anything else is `FAILED`. Those words do not change it.

## pbsd

`extra.ported` is the `tools/verify` module names C++ does not import.
`extra.invoked` is the restaged checker class names that fired, not those module names.
`--allow-exec` off still returns `exec_notrun` for a present tree.
When the tree is present, `--allow-exec` is on, and neither a restaged class nor a missing heavy binary produced a row, the stage writes a `NOTRUN` that the scanners did not run. It does not return an empty list.
