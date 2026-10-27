// std::queue operational model (src/prism/pir/models/cxx/queue) against
// C++23 [queue.defn] / [queue.ops]. The pir stage puts the model headers
// first on the include path, so <queue> below is the model (the report's
// `extra.cxx_models` says "model: queue"); see docs/PIR.md "C++ library
// models". Each function is one contract; `_false` ones must fail for the
// class their yml names. At most 4 elements (size-bounded proofs).
// priority_queue is not part of the model.
#include <cassert>
#include <queue>

// push: FIFO, size, and both ends.
int queue_push_true(int x) {
    std::queue<int> q;
    q.push(1);
    q.push(x);
    assert(q.size() == 2);
    assert(!q.empty());
    assert(q.front() == 1);
    assert(q.back() == x);
    return 0;
}

// pop destroys the front element. The new front is the next one pushed.
int queue_pop_true(int x) {
    std::queue<int> q;
    q.push(x);
    q.push(2);
    q.push(3);
    q.pop();
    assert(q.size() == 2);
    assert(q.front() == 2);
    assert(q.back() == 3);
    return 0;
}

// pop frees the front node: the reference from front() is MEM-UAF.
int queue_pop_uaf_false(int c) {
    std::queue<int> q;
    q.push(c & 7);
    q.push(1);
    int& r = q.front();
    q.pop();
    return r;
}

// front() on an empty queue (precondition !empty()).
int queue_front_empty_false(int c) {
    std::queue<int> q;
    if (c) q.push(1);
    return q.front();
}

// back() on an empty queue (precondition !empty()).
int queue_back_empty_false(int c) {
    std::queue<int> q;
    if (c) q.push(1);
    return q.back();
}

// pop() on an empty queue (precondition !empty()).
int queue_pop_empty_false(int c) {
    std::queue<int> q;
    if (c) q.push(1);
    q.pop();
    return 0;
}

// A wrong contract: front() is not the last value pushed.
int queue_order_false(int a, int b) {
    std::queue<int> q;
    q.push(a);
    q.push(b);
    assert(q.front() == b);
    return 0;
}
