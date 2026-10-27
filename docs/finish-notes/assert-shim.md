# Assert shim does not return

Slice `finish/assert-noreturn`. `src/prism/pir/models/libc/shim/assert.h` defines `assert` as a macro. The failure arm is a call to `__assert_fail`. There is no function body, and the macro does not call `abort()`.

A failed assert does not return and is not `CLEAN`. The macro was left as written. One comment line records that. PIR already treats `__assert_fail` as noreturn in `src/prism/pir/translate.cpp`: the call is a `FUNC-CONTRACT` check (`assertion can fail`) and the path ends. That is the opposite of modelling the failure as `abort()`, whose out-of-memory path is not a defect.

`tests/cpp/test_assert_shim.cpp` reads the header as text and does not compile it. The case requires a call to `__assert_fail` in the `assert` macro. It fails if that macro contains `return`, or if a function body calls `__assert_fail` and then returns, including a function that falls off its closing brace. Replacing the macro with a returning function fails the case.

No proof was added. `prism_tests` was not executed in this slice.
