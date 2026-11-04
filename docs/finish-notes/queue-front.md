# queue front

`std::queue::front` and `back` on an empty queue are already the
FUNC-CONTRACT check `__glibcxx_assert(!this->empty())`. The condition text
does not mention `size()`. The failed check does not return, and the node
load is the next statement. `empty()` is `_M_head == nullptr`, a pointer
compare, not a load of a heap node. An empty front or back does not return
`T{}`.

Nothing here is `PROVED`. Nothing here is `CLEAN`.
