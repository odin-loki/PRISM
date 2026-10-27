// std::stack operational model (src/prism/pir/models/cxx/stack). The pir
// stage puts the model headers first on the include path, so <stack> below
// is the model (the report's `extra.cxx_models` says "model: stack"). Each
// function is one contract. At most a few elements.
#include <cassert>
#include <stack>

// push leaves size and top as the last value pushed. An older top() reference
// stays valid across a later push (that push allocates a new node).
int stack_push_true(int x) {
    std::stack<int> s;
    s.push(1);
    int& older = s.top();
    s.push(x);
    s.push(2);
    assert(s.size() == 3);
    assert(s.empty() == false);
    assert(s.top() == 2);
    assert(older == 1);
    return 0;
}

// pop of a newer element frees that node only. The reference to the element
// underneath still names its own node.
int stack_under_true(int v) {
    std::stack<int> s;
    s.push(v);
    int& older = s.top();
    s.push(2);
    s.push(3);
    s.pop();
    assert(s.size() == 2);
    assert(s.top() == 2);
    assert(older == v);
    s.pop();
    assert(s.top() == v);
    assert(older == v);
    return older;
}

// pop destroys the element and frees its node: the top() reference dangles.
int stack_pop_uaf_false(int c) {
    std::stack<int> s;
    s.push(10);
    s.push(20);
    int& t = s.top();
    s.pop();
    return t + (c & 1);
}

// top() on an empty stack (precondition !empty()).
int stack_top_empty_false(int c) {
    std::stack<int> s;
    if (c) s.push(1);
    return s.top();
}

// pop() on an empty stack (precondition !empty()). Not a returned zero.
int stack_pop_empty_false(int c) {
    std::stack<int> s;
    if (c) s.push(1);
    s.pop();
    return 0;
}

// A wrong contract: top() is not the first value pushed.
int stack_order_false(int a, int b) {
    std::stack<int> s;
    s.push(a);
    s.push(b);
    assert(s.top() == a);
    return 0;
}
