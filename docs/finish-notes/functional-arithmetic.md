# functional arithmetic

Branch `finish/cxx-functional-arithmetic`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::plus`, `std::minus`, `std::multiplies`, `std::divides`, `std::modulus`, and `std::negate` are on the existing header file `src/prism/pir/models/cxx/functional`. Each takes one type argument. The header is still a file, not a directory. The include guard is still `_GLIBCXX_FUNCTIONAL`. `#pragma GCC system_header`. Namespace `std`. `std::function`, both `std::invoke` overloads, `std::not_fn`, `std::identity`, `std::equal_to`, `std::not_equal_to`, `std::less`, and `std::greater` are unchanged.

PIR was not run. The solver was not run. This is not PROVED and not CLEAN.

## What the six objects do

They are function objects. They are not function pointers. A type that does not support the operator fails the template. There is no stand-in result and no default `T{}`.

The template parameter is the one type. `operator()` takes two `const _Tp&` arguments, except `negate`, which takes one. There is no `void` specialization and no default `void`, so `std::plus<>`, `std::minus<>`, `std::multiplies<>`, `std::divides<>`, `std::modulus<>`, and `std::negate<>` are not declared. A unit that names one fails to compile against this header and pir falls back to libstdc++.

`bool` and a non-integer use the operator, with two exceptions below. The fitting integer result is that operator's value.

A signed integer `plus`, `minus`, or `multiplies` whose result does not fit in the type calls `__prism_check` with class `INT-SIGNED-OVF` and message `std::plus result does not fit in T`, `std::minus result does not fit in T`, or `std::multiplies result does not fit in T`, then `__builtin_trap`. That arm does not return a wrapped result. An unsigned integer of those three whose result does not fit calls `__prism_check` with class `FUNC-CONTRACT` and the same message, then `__builtin_trap`. That arm does not return a wrapped result. The signed maximum is an unsigned shift of all-ones, and the minimum is `-1` minus that maximum, so the test itself does not overflow. The product test divides only on a side where the division stays in range, and it treats the minimum times `-1` before any division.

An integer `divides` or `modulus` with a zero right operand calls `__prism_check` with class `FUNC-CONTRACT` and message `std::divides divisor is zero` or `std::modulus divisor is zero`, then `__builtin_trap`. That arm does not return a quotient or a remainder. The same zero check covers `bool` (`false`) and, for `divides` only, a floating zero. A user-defined type uses `operator/` or `operator%` and is not compared with zero.

A signed `divides` of the type minimum by `-1` calls `__prism_check` with class `FUNC-CONTRACT` and message `std::divides of the minimum by -1`, then `__builtin_trap`, and does not return a quotient. A signed `modulus` of the type minimum by `-1` calls `__prism_check` with class `FUNC-CONTRACT` and message `std::modulus of the minimum by -1`, then `__builtin_trap`, and does not return a remainder. That remainder expression is undefined when the quotient does not fit.

`negate` of the most negative signed value calls `__prism_check` with class `INT-SIGNED-OVF` and message `std::negate of the most negative value`, then `__builtin_trap`. That arm does not negate it. Unsigned `negate` is the built-in negation, including the wrap of a non-zero value.

## How the names share the translation unit

`<bits/stl_function.h>` still defines these names for the rest of the library. This header renames `plus`, `minus`, `multiplies`, `divides`, `modulus`, and `negate` for that include, the same way it renames `equal_to`, `not_equal_to`, `less`, and `greater`, then defines the objects above. `ranges::less` and `ranges::equal_to` stay the ones from `ranges_cmp.h`. If `<bits/stl_function.h>` was already included, that rename is skipped and the libstdc++ objects stay, and this header does not define a second one. Logical and bitwise function objects stay the libstdc++ ones.

`bind`, `mem_fn`, `ref`, `cref`, `bind_front`, and `reference_wrapper` are still not declared.

This header does not include `<memory>` or `<type_traits>`. The two bits headers pull in `<bits/move.h>`, and that header includes `<type_traits>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/functional` was already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_functional.cpp` was already in `_prism_test_sources`. No new test file was added. `tests/cpp/test_qa_conformance.cpp` already lists `functional` in the public header array. That array was not edited. No conformance harness was added.

## Tests

`tests/cpp/test_cxx_functional.cpp` reads the header. It is a regular file. The `std::function`, `std::invoke`, `std::not_fn`, `std::identity`, and comparison checks are unchanged. The header says it does not include memory or type_traits, and the text has neither `#include <memory>` nor `#include <type_traits>`. After line comments are removed there are two includes, `<bits/stl_function.h>` and `<bits/ranges_cmp.h>`. The code names `struct plus`, `struct minus`, `struct multiplies`, `struct divides`, `struct modulus`, and `struct negate` once each, and it does not name `struct plus<void>` or the other five `void` forms, and it does not name those as function pointers. Each failing check is a literal class and message, then `__builtin_trap`, and the operator result is after that trap. The phrase `This is not PROVED and not CLEAN.` appears once. With line comments removed, the code still does not name `bind`, `mem_fn`, `reference_wrapper`, `bind_front`, `cref`, or `ref(`.

`tests/cxx_models/functional_trace.cpp` includes `<functional>`. Checked with clang++ 18.1.3 and libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. Compiled twice (`-isystem` the model directory, and without it). Stdout matched:

```
empty 0
dtor 0
bool 1
call 42 calls=1
move src=0 dst=1 calls=1
move call 42 calls=2
held 5
reassign 4 calls=3
copy 1 1 calls=3
copy call 42 calls=4
livedtor 4
null 0
invoke 42 calls=5
invoke ptr 42 calls=6
invoke add 42 calls=7
invoke ref 11 n=11 calls=8
invoke rv 10 calls=9
invoke void 10
invoke nx 42 calls=11
invoke slot 9
invoke held 5
not zero 1 calls=12
not copy 0 1 calls=14
not flag 1 0 calls=16
not ref 0 m=4 calls=17
not rv 0 calls=18
not both 1 calls=19
not nx 1 calls=20
not ptr 1 0 calls=22
not obj 1 0 calls=24
not bit 1 bit=0 calls=25
not hold
id lv 7 x=7
id same 8
id rv 8
id mo 3 3 moves=0
eq 1 0
ne 0 1
lt 1 0
gt 1 0
tr 1 1 1 0
ptr 1
obj 1 0 1 1
add 5 -5
sub 1
mul -12
div 3 1
divn -3 -1
neg -4 4 0
uadd 5
usub 3
umul 6
udiv 3 1
uneg 0 4294967295
ll 42
sc 5
fp 4.0
bdiv 1
objadd 5
quot 3 1
bit 2
land 0
```

`add 5 -5` is `plus<int>` copied as an object: `2 + 3` and `-2 + -3`. `sub 1` is `minus<int>` of `3 - 2`. `mul -12` is `multiplies<int>` of `-3 * 4`. `div 3 1` is `7 / 2` and `7 % 2`. `divn -3 -1` is `-7 / 2` and `-7 % 2`, toward zero. `neg -4 4 0` is `negate<int>` of `4`, `-4`, and `0`, copied as an object. `uadd 5`, `usub 3`, `umul 6`, and `udiv 3 1` are the unsigned forms of those operations. `uneg 0 4294967295` is unsigned `negate` of `0` and of `1`. `ll 42` is `plus<long long>` of `20` and `22`. `sc 5` is `plus<signed char>` of `2` and `3`. `fp 4.0` is `plus<double>` of `1.5` and `2.5`. `bdiv 1` is `divides<bool>` of `true` and `true`. `objadd 5` is `plus` of a type whose `operator+` adds the stored ints. `quot 3 1` is `divides` and `modulus` of a type whose `/` and `%` are the integer operators. `bit 2` is `bit_and<int>` of `6` and `3`. `land 0` is `logical_and<int>` of `1` and `0`. Those last two stay the libstdc++ objects. The earlier lines are the `std::function`, `std::invoke`, `std::not_fn`, `std::identity`, and comparison trace.

On the model binary, each trap argv aborts with the check below and prints no `fake=`. Undefined-behavior sanitizer does not report a wrapped operation on those paths.

| argv | class | message |
| --- | --- | --- |
| `plus-ovf`, `plus-min`, `plus-ll`, `plus-sc` | `INT-SIGNED-OVF` | `std::plus result does not fit in T` |
| `plus-uovf` | `FUNC-CONTRACT` | `std::plus result does not fit in T` |
| `minus-ovf`, `minus-pos` | `INT-SIGNED-OVF` | `std::minus result does not fit in T` |
| `minus-uovf` | `FUNC-CONTRACT` | `std::minus result does not fit in T` |
| `mul-min`, `mul-max`, `mul-neg`, `mul-sc` | `INT-SIGNED-OVF` | `std::multiplies result does not fit in T` |
| `mul-uovf` | `FUNC-CONTRACT` | `std::multiplies result does not fit in T` |
| `div-zero`, `div-uzero`, `div-fzero`, `div-bzero` | `FUNC-CONTRACT` | `std::divides divisor is zero` |
| `div-min`, `div-sc` | `FUNC-CONTRACT` | `std::divides of the minimum by -1` |
| `mod-zero`, `mod-uzero`, `mod-bzero` | `FUNC-CONTRACT` | `std::modulus divisor is zero` |
| `mod-min` | `FUNC-CONTRACT` | `std::modulus of the minimum by -1` |
| `neg-min`, `neg-sc` | `INT-SIGNED-OVF` | `std::negate of the most negative value` |

`plus-ovf` is `INT_MAX + 1`. `plus-min` is `INT_MIN + -1`. `plus-ll` is `LLONG_MAX + 1`. `plus-sc` is `signed char` `127 + 1`. `plus-uovf` is `UINT_MAX + 1`. `minus-ovf` is `INT_MIN - 1`. `minus-pos` is `INT_MAX - -1`. `minus-uovf` is `0u - 1u`. `mul-min` is `INT_MIN * -1`. `mul-max` is `INT_MAX * 2`. `mul-neg` is `INT_MIN * 2`. `mul-sc` is `signed char` `-2 * -64`. `mul-uovf` is `UINT_MAX * 2`. `div-zero` is `1 / 0`. `div-uzero` is `1u / 0u`. `div-fzero` is `1.0 / 0.0`. `div-bzero` is `true / false`. `div-min` is `INT_MIN / -1`. `div-sc` is the `signed char` minimum divided by `-1`. `mod-zero` is `1 % 0`. `mod-uzero` is `1u % 0u`. `mod-bzero` is `true % false`. `mod-min` is `INT_MIN % -1`. `neg-min` is `negate<int>` of `INT_MIN`. `neg-sc` is `negate<signed char>` of the minimum. `argv` `empty`, `nullfn`, and `nullnot` still abort on the same `__prism_check` paths and print no `fake=`.

Against this header and against libstdc++, `plus<int>`, `minus`, `multiplies`, `divides`, `modulus`, `negate`, `plus<unsigned>`, `plus` of a type that only provides `+`, `divides` and `modulus` of a type that provides `/` and `%`, and `bit_and<int>` compile. `std::plus<>` and `std::negate<>` compile against libstdc++ and fail against this header. A type with no `+` fails `plus` on both. A type with `+` and no `-` fails a unit that also calls `minus` on both. `std::bind`, `std::mem_fn`, `std::ref`, and `std::invoke` of a functor, of a member pointer, and of `std::function` still fail to compile against this header and still compile with libstdc++. Without a usable clang++ the compile cases are `NOTRUN`. The header-text case does not need a compiler. cmake, ninja, and `prism_tests` were not run.
