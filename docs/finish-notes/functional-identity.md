# functional identity and comparisons

Branch `finish/cxx-functional-identity`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`std::identity`, `std::equal_to`, `std::not_equal_to`, `std::less`, and
`std::greater` are on the existing header file
`src/prism/pir/models/cxx/functional`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_FUNCTIONAL`.
`#pragma GCC system_header`. Namespace `std`. `std::function`, both
`std::invoke` overloads, and `std::not_fn` are unchanged.

PIR was not run. The solver was not run. This is not PROVED and not CLEAN.

## What the five objects do

They are function objects. They are not function pointers. A type that does
not support the operator fails the template. There is no stand-in `bool` and
no default `T{}`.

`identity` returns its argument as `static_cast<_Tp&&>(__t)`. That is the
same object. It does not copy through a default `T{}`. A type with a deleted
copy and no default constructor is still returned. `is_transparent` is
`__is_transparent`.

`equal_to` returns `a == b`. `not_equal_to` returns `!(a == b)`. `less`
returns `a < b`. `greater` returns `b < a`. The result is that comparison.
They do not invent a third value. Each has a typed class template and a
`void` specialization. The `void` specialization is transparent
(`std::less<>`, `std::greater<>`, `std::equal_to<>`, `std::not_equal_to<>`)
and forwards both arguments. The typed call takes `const _Tp&`. A missing
`==` or `<` fails the `requires` clause. `not_equal_to` does not call `!=`.
`greater` does not call `>`. A type that only provides `==` can be passed to
`not_equal_to`. A type that only provides `<` can be passed to `greater`.

## How the names share the translation unit

`<bits/stl_function.h>` and `<bits/ranges_cmp.h>` still define these names
for the rest of the library. This header renames `equal_to`,
`not_equal_to`, `less`, and `greater` for the first include, and `identity`
for the second, then defines the objects above. `ranges::less` and
`ranges::equal_to` stay the ones from `ranges_cmp.h`. If either bits header
was already included, that rename is skipped and the libstdc++ objects stay.
The rest of `<bits/stl_function.h>` (arithmetic function objects and the
transparent-trait helpers) stays so a later model include does not define
those names twice. Those extra objects are not a new model.

`bind`, `mem_fn`, `ref`, `cref`, `bind_front`, and `reference_wrapper` are
still not declared.

This header does not include `<memory>` or `<type_traits>`. The two bits
headers pull in `<bits/move.h>`, and that header includes `<type_traits>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/functional` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_functional.cpp` was
already in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already
lists `functional` in the public header array. That array was not edited.
No conformance harness was added. `src/prism/bmc.cpp` and `src/prism/stage.cpp`
were not edited.

## Tests

`tests/cpp/test_cxx_functional.cpp` reads the header. It is a regular file.
The `std::function`, `std::invoke`, and `std::not_fn` checks are unchanged.
The header says it does not include memory or type_traits, and the text has
neither `#include <memory>` nor `#include <type_traits>`. After line comments
are removed there are two includes, `<bits/stl_function.h>` and
`<bits/ranges_cmp.h>`. The code names `struct identity`, `struct equal_to`,
`struct not_equal_to`, `struct less`, and `struct greater`, and it does not
name those as function pointers. `identity` returns
`static_cast<_Tp&&>(__t)` and that struct does not contain `{}`. The
comparison bodies are `return __a == __b;`, `return !(__a == __b);`,
`return __a < __b;`, and `return __b < __a;`. They do not return `a > b`,
`a != b`, `true`, `false`, `0`, or `{}`. The phrase
`This is not PROVED and not CLEAN.` appears once. With line comments
removed, the code still does not name `bind`, `mem_fn`, `reference_wrapper`,
`bind_front`, `cref`, or `ref(`.

`tests/cxx_models/functional_trace.cpp` includes `<functional>`. Checked
with clang++ 18.1.3 and libstdc++, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. Compiled
twice (`-isystem` the model directory, and without it). Stdout matched:

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
```

`id lv 7 x=7` then `id same 8` is an lvalue returned as a reference: writing
through it writes the caller's `int`. `id rv 8` is `identity` of a prvalue
whose type has no default constructor. `id mo 3 3 moves=0` is an lvalue of
a move-only type: the object is not copied and not moved. `eq 1 0` is
`equal_to<int>` copied as an object, true for equal ints and false
otherwise. `ne 0 1` is `not_equal_to<int>`. `lt 1 0` is `less<int>`.
`gt 1 0` is `greater<int>` copied as an object. `tr 1 1 1 0` is transparent
`less<>` on `int` and `double`, `greater<>` on two `double`s,
`not_equal_to<>` on two `double`s, and `equal_to<>` on unequal `int`s.
`ptr 1` is `less<int*>` on two pointers into the same array. `obj 1 0 1 1`
is `equal_to`, `not_equal_to`, `less`, and `greater` on a type whose `==`,
`!=`, `<`, and `>` agree with `a == b`, `!(a == b)`, `a < b`, and `b < a`.
The earlier lines are the `std::function`, `std::invoke`, and `std::not_fn`
trace. `argv` `empty`, `nullfn`, and `nullnot` still abort on the same
`__prism_check` paths and print no `fake=`.

Against this header and against libstdc++, `identity` of a type with no
default constructor, `equal_to<int>`, `not_equal_to` of a type that only
provides `==`, `less` of a type that only provides `<`, and transparent
`less<>` of `int` and `double` compile. `greater` of a type that only
provides `<` compiles against this header and fails against libstdc++,
because libstdc++ calls `operator>`. A type with no `==` fails both
`equal_to` and `not_equal_to`. A type with no `<` fails both `less` and
`greater`. `std::bind`, `std::mem_fn`, `std::ref`, and `std::invoke` of a
functor, of a member pointer, and of `std::function` still fail to compile
against this header and still compile with libstdc++. Without a usable
clang++ the compile cases are `NOTRUN`. The header-text case does not need
a compiler. cmake, ninja, and `prism_tests` were not run.
