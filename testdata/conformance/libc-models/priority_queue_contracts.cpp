// std::priority_queue operational model (src/prism/pir/models/cxx/priority_queue)
// against C++23 [priority.queue]. The pir stage puts the model headers first
// on the include path, so <priority_queue> below is the model (the report's
// `extra.cxx_models` says "model: priority_queue"); see docs/PIR.md "C++
// library models". Each function is one contract; `_false` ones must fail
// for the class their yml names. At most 4 elements (size-bounded proofs).
// emplace, swap, and the range constructors are not part of the model.
#include <cassert>
#include <priority_queue>

struct PqSeq {
    using value_type = int;
};

// comp(a, b) means a is ordered before b. greater-than makes the least element the top.
struct PqMin {
    bool operator()(int a, int b) const { return a > b; }
};

// push / size / empty: the top is the greatest of the pushed values.
int priority_queue_push_true(int x) {
    std::priority_queue<int> q;
    assert(q.empty());
    assert(q.size() == 0);
    q.push(1);
    q.push(x);
    q.push(0);
    assert(!q.empty());
    assert(q.size() == 3);
    int m = 1;
    if (x > m) m = x;
    assert(q.top() == m);
    return 0;
}

// pop removes the current greatest. The three values come back in non-increasing order.
int priority_queue_pop_order_true(int x) {
    if (x < 0 || x > 3) return 0;
    std::priority_queue<int> q;
    q.push(1);
    q.push(3);
    q.push(x);
    int a = q.top();
    q.pop();
    int b = q.top();
    q.pop();
    int c = q.top();
    q.pop();
    assert(q.empty());
    assert(q.size() == 0);
    assert(a >= b && b >= c);
    assert(a + b + c == 1 + 3 + x);
    return 0;
}

// A comparator that orders the larger value first: the top is the least element.
int priority_queue_min_true(int x) {
    if (x < 0 || x > 3) return 0;
    std::priority_queue<int, PqSeq, PqMin> q;
    q.push(1);
    q.push(3);
    q.push(x);
    int a = q.top();
    q.pop();
    int b = q.top();
    q.pop();
    int c = q.top();
    assert(a <= b && b <= c);
    assert(a + b + c == 1 + 3 + x);
    return 0;
}

// top()'s reference is the root node. pop frees that node: the load is MEM-UAF.
int priority_queue_top_pop_uaf_false(int c) {
    std::priority_queue<int> q;
    q.push(1);
    q.push(c & 7);
    const int& r = q.top();
    q.pop();
    return r;
}

// top() on an empty queue (precondition !empty()).
int priority_queue_top_empty_false(int c) {
    std::priority_queue<int> q;
    if (c) q.push(1);
    return q.top();
}

// pop() on an empty queue (precondition !empty()).
int priority_queue_pop_empty_false(int c) {
    std::priority_queue<int> q;
    if (c) q.push(1);
    q.pop();
    return 0;
}

// A wrong contract: top() is not the lesser of the two values.
int priority_queue_order_false(int a, int b) {
    std::priority_queue<int> q;
    q.push(a);
    q.push(b);
    int m = a < b ? a : b;
    assert(q.top() == m);
    return 0;
}
