// PRISM conformance task cxx/cxx_stack_false.cpp: expected false (memsafety)
// reference from top() used after pop frees that element
#include <stack>
int cxx_stack_false(int c) {
    std::stack<int> s;
    s.push(10);
    s.push(20);
    int& t = s.top();  // 20
    if (c) s.pop();    // frees the node `t` refers to
    return t;
}
