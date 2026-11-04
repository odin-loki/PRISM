# queue pop

`std::queue::pop` on an empty queue is already the FUNC-CONTRACT check
`__glibcxx_assert(!this->empty())` inside `_M_pop`. The condition text does
not mention `size()`. `__glibcxx_assert_fail` does not return, so the empty
call does not load the front node and does not destroy or free one. It does
not return `T{}`.

`pop()` calls `_M_pop()` and does not itself name the node. A non-empty pop
still destroys the front element and deallocates that node.

Nothing here is `PROVED`. Nothing here is `CLEAN`.
