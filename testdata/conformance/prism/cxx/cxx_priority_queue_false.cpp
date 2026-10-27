// PRISM conformance task cxx/cxx_priority_queue_false.cpp: expected false (memsafety)
// reference from top() used after pop. pop destroys that element and frees
// its node.
#include <priority_queue>
int cxx_priority_queue_false(int c) {
    std::priority_queue<int> q;
    q.push(10);
    q.push(20);
    const int& r = q.top();  // 20
    if (c) q.pop();          // frees the node `r` refers to
    return r;
}
