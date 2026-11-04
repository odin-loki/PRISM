# functional bitwise

Branch `finish/cxx-functional-bitwise`, base `c164ba71c67bf51330687627679f49368a361928`.

`std::bit_and`, `std::bit_or`, and `std::bit_xor` are on the existing header file `src/prism/pir/models/cxx/functional`. Each takes one type argument. The header is still a file, not a directory. The include guard is still `_GLIBCXX_FUNCTIONAL`. `#pragma GCC system_header`. Namespace `std`. `std::function`, both `std::invoke` overloads, `std::not_fn`, `std::identity`, `std::equal_to`, `std::not_equal_to`, `std::less`, `std::greater`, `std::plus`, `std::minus`, `std::multiplies`, `std::divides`, `std::modulus`, `std::negate`, `std::logical_and`, `std::logical_or`, and `std::logical_not` are unchanged.

PIR was not run. The solver was not run. This is not PROVED and not CLEAN.

## What the three objects do

They are function objects. They are not function pointers. `bit_and` returns the `&` of its arguments. `bit_or` returns the `|` of its arguments. `bit_xor` returns the `^` of its arguments. The result type is that operator (`decltype(auto)`). A type that does not support the operator fails the template. There is no stand-in integer and no default `T{}`.

The template parameter is the one type. `operator()` takes two `const _Tp&` arguments. There is no `void` specialization and no default `void`, so `std::bit_and<>`, `std::bit_or<>`, and `std::bit_xor<>` are not declared. A unit that names one fails to compile against this header and pir falls back to libstdc++.

`bool`, integers, and a user-defined type whose operator returns that same type use that operator. On `bool`, the built-in operator promotes to `int`, and this header returns that `int`. libstdc++'s one-type form returns `bool`, so the stored type differs and `static_cast<int>` of either value is the same integer. A user-defined operator that returns another type returns that type. libstdc++'s one-type form returns the argument type, so a call that reads a member of a different result type compiles against this header and fails against libstdc++.

## How the names share the translation unit

`<bits/stl_function.h>` still defines these names for the rest of the library. This header renames `bit_and`, `bit_or`, and `bit_xor` for that include, the same way it renames `plus` and `logical_and`, then defines the objects above. `ranges::less` and `ranges::equal_to` stay the ones from `ranges_cmp.h`. `bit_not` stays the libstdc++ object. If `<bits/stl_function.h>` was already included, that rename is skipped and the libstdc++ objects stay, and this header does not define a second one.

`bind`, `mem_fn`, `ref`, `cref`, `bind_front`, and `reference_wrapper` are still not declared.

This header does not include `<memory>` or `<type_traits>`. The two bits headers pull in `<bits/move.h>`, and that header includes `<type_traits>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/functional` was already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_functional.cpp` was already in `_prism_test_sources`. No new test file was added. `tests/cpp/test_qa_conformance.cpp` already lists `functional` in the public header array. That array was not edited. No conformance harness was added. `src/prism/bmc.cpp` and `src/prism/stage.cpp` were not edited.

## Tests

`tests/cpp/test_cxx_functional.cpp` reads the header. It is a regular file. The `std::function`, `std::invoke`, `std::not_fn`, `std::identity`, comparison, arithmetic, and logical checks are unchanged, except the shared binary `decltype(auto)` signature is now counted five times: `logical_and`, `logical_or`, `bit_and`, `bit_or`, and `bit_xor`. The unary `logical_not` signature is still one. The header says it does not include memory or type_traits, and the text has neither `#include <memory>` nor `#include <type_traits>`. After line comments are removed there are two includes, `<bits/stl_function.h>` and `<bits/ranges_cmp.h>`. The code names `struct bit_and`, `struct bit_or`, and `struct bit_xor` once each, and it does not name `struct bit_and<void>` or the other two `void` forms, and it does not name those as function pointers. Each body returns `__a & __b`, `__a | __b`, or `__a ^ __b`, with `decltype(auto)` and a requires clause on that operator. The phrase `This is not PROVED and not CLEAN.` appears once. With line comments removed, the code still does not name `bind`, `mem_fn`, `reference_wrapper`, `bind_front`, `cref`, or `ref(`.

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
landc 0 1
lor 1 0
lnot 1 0
lbool 0 0 0
blog 0 1 1
band 2 1
bor 7 3
bxor 5 0
bbool 0 0 0
ubit 2 7 5
bobj 2 7 5
bandonly 2
bnot -1
```

`band 2 1` is `bit_and<int>` copied as an object: `6 & 3` and `7 & 1`. `bor 7 3` is `bit_or<int>` copied: `6 | 3` and `1 | 2`. `bxor 5 0` is `bit_xor<int>` copied: `6 ^ 3` and `6 ^ 6`. `bbool 0 0 0` is `bit_and<bool>` of `true` and `false`, `bit_or<bool>` of `false` and `false`, and `bit_xor<bool>` of `true` and `true`. `ubit 2 7 5` is the unsigned forms of `6` and `3`. `bobj 2 7 5` is `bit_and`, `bit_or`, and `bit_xor` of a type whose operators return that type: `6 & 3`, `6 | 3`, and `6 ^ 3`. `bandonly 2` is `bit_and` of a type that only provides `&`. `bnot -1` is `bit_not<int>` of `0`, still the libstdc++ object. `bit 2` is `bit_and<int>` of `6` and `3`. The earlier lines are the `std::function`, `std::invoke`, `std::not_fn`, `std::identity`, comparison, arithmetic, and logical trace. `plus` of `INT_MAX` and `1` still prints `CHECK INT-SIGNED-OVF: std::plus result does not fit in T` and aborts before a result. An empty `std::function` call still prints the `CXX-STD-FUNCTION-NULL` check and aborts before a result.

Against this header and against libstdc++, `bit_and<int>`, `bit_or<int>`, `bit_xor<int>`, the `bool` and `unsigned` forms, `bit_and` of a type that only provides `&`, all three of a type whose operators return that type, `bit_not<int>`, and `logical_and<int>` compile. `std::bit_and<>`, `std::bit_or<>`, and `std::bit_xor<>` compile against libstdc++ and fail against this header. A type whose `&`, `|`, and `^` return another type compiles against this header, and the call's `.v` is that result; the same unit fails against libstdc++. A type with no `&`, `|`, or `^` fails all three on both. A type with `&` and no `|` fails a unit that also calls `bit_or` on both. The header-text case does not need a compiler. cmake, ninja, and `prism_tests` were not run.
