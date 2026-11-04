# priority_queue less

`operator<` is a function template on
`src/prism/pir/models/cxx/priority_queue`. It walks stored elements in
heap-index order. It does not sort them and it does not use the comparator.
Two empty queues are not less. The return reads `_M_size` and does not load
the root. An empty queue is less than a non-empty queue, and that path does
not load the empty root. A non-empty queue compared with an empty queue is
not less. Otherwise each index compares the stored elements with
`operator<`. A difference does not read a later index. An index past the
shorter queue is not read. The comparison does not return `T{}`.

`operator>`, `operator<=`, `operator>=`, `operator==`, `operator!=`, and
`operator<=>` stay undeclared. libstdc++ has no `priority_queue` comparison
operators, so `tests/cxx_models/priority_queue_less_trace.cpp` is compiled
only against this header.

Two heaps that hold the same values `{1,2,3,4}` in different index order
compare less (`store=1`). A sort of those values would compare them equal.
A negative element past the deciding index is not read (`hpoison=0`,
`mpoison=0`).

Nothing here is `PROVED`. Nothing here is `CLEAN`.
