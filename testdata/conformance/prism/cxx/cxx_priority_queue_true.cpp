// PRISM conformance task cxx/cxx_priority_queue_true.cpp: expected true (memsafety)
// reference from top() kept across push. push does not free element nodes;
// the new value is at most 7, so the root node is still the one `r` names.
#include <priority_queue>
int cxx_priority_queue_true(int c) {
    std::priority_queue<int> q;
    q.push(10);
    q.push(20);
    const int& r = q.top();  // 20
    q.push(c & 7);           // does not free the node `r` refers to
    return r;
}
