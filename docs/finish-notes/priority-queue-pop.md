# priority_queue pop

`std::priority_queue::pop` on an empty queue is already the FUNC-CONTRACT
check `__glibcxx_assert(!this->empty())`. The condition text does not
mention `size()`. The check runs before `_M_heap[0]` is loaded and before
`_M_drop`. An empty pop does not destroy a node and does not return `T{}`.

A non-empty pop still destroys the current top element and frees that node.

Nothing here is `PROVED`. Nothing here is `CLEAN`.
