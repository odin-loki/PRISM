# queue less

`operator<` is a function template on `src/prism/pir/models/cxx/queue`.
Two empty queues compare not-less. `empty()` reads the head pointer and
does not load a node, and that return happens before a node is named. An
empty queue is less than a non-empty queue. A non-empty queue is not less
than an empty queue. Otherwise the stored elements are compared from front to back.
The walk stops at the first deciding element. An element past the
shorter queue is not read. The comparison does not return `T{}`.

`operator>`, `operator<=`, `operator>=`, `operator==`, `operator!=`, and
`operator<=>` stay undeclared.

`tests/cxx_models/queue_less_trace.cpp` matches libstdc++ stdout. Two empty
`queue<int>` values print `lt empty=0`. An empty queue less than a
one-element queue prints `el=1`. The `MemLt` call counts stay at 0 for
those empty paths, and a negative element past the deciding index is not
read (`pread=0`).

Nothing here is `PROVED`. Nothing here is `CLEAN`.
