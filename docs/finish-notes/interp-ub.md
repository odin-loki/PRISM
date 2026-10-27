# Concrete division and shift undefined behavior

Slice on `finish/interp-ub`, started at `136f174bb`.

`shift` and `binop` in `src/prism/stages/interp.cpp` already report these
operations as defect classes. Those classes were left as they are.

## Division by zero

`binop` throws `INT-DIV-ZERO` when the right operand of `/` or `%` is zero,
signed or unsigned, before it divides. `execute` catches that and stores the
class in `ExecResult.ub`. There is no value and no error string.

That result is not `CLEAN` and not `PROVED`. `execute` does not return a
finding status. Where a finding is built from `ub` (`execute_cex`, the
fuzzer, concolic), a non-empty class is `CRASH` with that class. `CLEAN` is
the no-trap row. A proof status is not produced here.

## Shift amount

`shift` throws `INT-SHIFT-UB` when the amount is negative or is greater than
or equal to the width of the promoted left operand (C11 6.5.7: the amount
must be strictly less than that width). Both `<<` and `>>` use this check,
including compound assignments.

A shift inside the range is still evaluated. `1 << 3` on a 32-bit `int` is
8. `1LL << 3` on `long long` is 8. An amount of 64 is out of range only for
a 64-bit left operand; an amount of 32 is out of range for a 32-bit `int`
or `unsigned`.

A signed `<<` of a negative left operand, or one whose result does not fit
in the result type, is still `INT-SHIFT-UB`. That check was already in
`shift` and was not applied to every shift.

## What the test calls

`prism::stages_detail::execute` is the in-process entry
`tests/cpp/test_concrete.cpp` uses. It does not run a compiler.
`prism::stages_detail::eval_src` evaluates one expression and throws the
same classes; the exception type is local to `interp.cpp`, so the test
observes the class through `execute`. `prism::concrete_execute` copies `ub`. When the expression has a value it
also copies the return code (`1 << 3` is `rc == 8`). When it does not, `rc`
stays 0; the class in `ub` is the result, not that zero.

## Tests

`tests/cpp/test_interp_ub.cpp`, one source line in `CMakeLists.txt`:

- `x / 0`, `x / y` and `x % y` with `y == 0`, including unsigned: `ub` is
  `INT-DIV-ZERO`, not `CLEAN`, not `PROVED`.
- Shift amount negative, equal to the width, or greater than the width, for
  `<<` and `>>`: `ub` is `INT-SHIFT-UB`.
- `return 1 << 3` on `int`: value 8, empty `ub`. `concrete_execute` reports
  `rc == 8`.

`prism_tests` was not run in this slice.
