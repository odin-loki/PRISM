# forward_list slice

Branch `finish/cxx-forward-list`, base `66d1b3373ddf27f0a3187c35d2d19bd533a599cc`.
`std::forward_list` is a header file, `src/prism/pir/models/cxx/forward_list`,
the same shape as `list`. A directory of that name would make
`#include <forward_list>` fail with EISDIR. The include guard is
`_PRISM_MODEL_FORWARD_LIST`, and the file defines `_GLIBCXX_FORWARD_LIST` so
libstdc++'s `<forward_list>` is not parsed as well.

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. This slice adds
`src/prism/pir/models/cxx/forward_list` to `PRISM_PIR_MODELS` and adds
`forward_list` to the header-name array in
`tests/cpp/test_qa_conformance.cpp`. The iostream and `deque/deque` skips in
that test are unchanged. No doctest driver was added; `forward_list_trace.cpp`
is the differential program, compiled by hand the way `list_trace.cpp` is
compiled by `tests/cpp/test_cxx_list.cpp`.

## What the model covers

A singly linked list. `before_begin()` is the sentinel stored in the
container (it owns itself, so it is not dereferenceable). `begin()` is the
sentinel's successor. `end()` is a null iterator. One heap node per element,
allocated with `std::allocator` of the node (`operator new` / `operator
delete`).

These operations deallocate exactly the nodes they drop, so a pointer,
reference, or iterator to a removed element is a real use-after-free:

- `erase_after` (one element, and the range overload)
- `pop_front` (`erase_after` of the sentinel)
- `clear` and the destructor
- `remove`, `remove_if`, `std::erase`, `std::erase_if`
- `unique`
- `resize` when it shortens the list
- `assign` and copy / initializer-list assignment when they drop a tail
  (a non-assignable `value_type` clears first)
- move assignment, which clears the target and then steals

`remove` / `remove_if` / `unique` hook the dropped nodes at the front of a
side list and destroy that list at the end of the call, which is reverse
order of removal. That side list is an implementation detail. It is not
`splice_after`.

`insert_after` and `emplace_after` / `push_front` / `emplace_front` allocate
a new node and do not invalidate iterators, pointers, or references to
elements already in the list. A multi-element `insert_after` builds a
temporary and transfers it in one step, so a throw leaves the destination
unchanged. `reverse` and `sort` only relink. `swap` exchanges the successor
of each sentinel and keeps element iterators attached to the same nodes.
Move construction steals the chain. Self-move assignment leaves the list
empty, as libstdc++'s `forward_list` does.

`max_size()` is `size_t(-1) / sizeof` of a layout that matches libstdc++'s
`_Fwd_list_node` (one pointer plus `__aligned_buffer`). On LP64 that node is
16 bytes for both `int` and `char`, so `max_size()` is `1152921504606846975`.
The model node is one pointer larger (`_M_owner`, the same idea as the list
model) so a foreign iterator fails `__glibcxx_assert`. The condition text
does not mention `size()` or `nullptr`, so PIR classifies it `FUNC-CONTRACT`.
`front` / `pop_front` require `!this->empty()`.

`sort` is libstdc++'s iterative bottom-up mergesort. Equivalent elements keep
their order (`[2,1,2,3,1]` with ids becomes `1:2 1:4 2:1 2:3 3:5`). `reverse`
of `[1, 2, 3, 2]` is `[2, 3, 2, 1]`. Neither constructs elements. The node
type is not instantiated in the class body, so
`struct Rec { std::forward_list<Rec> kids; };` works.

Also present: the usual constructors (including the deduction guide),
`assign`, `swap`, forward iterators with asserts on `*` / `->` and on `++`
of `end()`, `operator==`, `operator<=>`, non-member `swap`, `std::erase`,
`std::erase_if`.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- every `splice_after` overload, including same-list. Moving a node out of
  another `forward_list` would free it later with this list's allocator, which
  aliases ownership across allocators. Only `std::allocator<T>` is modelled,
  so the overload is omitted rather than guessed.
- every `merge` overload, for the same reason
- any allocator other than `std::allocator<T>`
- `std::pmr::forward_list` (the alias is declared and the class
  `static_assert`s on `polymorphic_allocator`, same as `pmr::vector`)

## Tests

Differential, clang++ 18, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`. `tests/cxx_models/forward_list_trace.cpp`
compiled twice (`-isystem` the model directory, and without it). 40 stdout
lines, identical, including construction and destruction order. Both
binaries, run with `uaf` (`erase_after` of the iterator that is then
dereferenced) and with `pop` (`pop_front`), abort with `heap-use-after-free`.

`testdata/conformance/prism/cxx/cxx_forward_list_true.cpp` uses an iterator
after `erase_after` of a different element (`memsafety` true).
`cxx_forward_list_false.cpp` uses it after `erase_after(before_begin())`
when that call erases the element (`false`, witness `[1]`).
`testdata/conformance/libc-models/forward_list_contracts.cpp` is the
libc-contract harness: push, insert-after, and erase-other true; clear,
`erase_after` of the iterator itself, and `pop_front` are `MEM-UAF`;
`front` / `pop_front` on empty and a wrong `front()` assert are
`FUNC-CONTRACT`. Those three TUs compile against the model header. PIR was
not run.
