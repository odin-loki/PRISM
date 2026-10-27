// PRISM conformance task cxx/cxx_stack_true.cpp: expected true (memsafety)
// reference to a std::stack element used after a newer element is popped
#include <stack>
int cxx_stack_true(int c) {
    std::stack<int> s;
    s.push(10);
    int& older = s.top();  // 10
    s.push(20);
    if (c) s.pop();        // frees 20; `older` stays
    return older;
}
