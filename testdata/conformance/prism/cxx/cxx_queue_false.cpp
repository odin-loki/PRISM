// PRISM conformance task cxx/cxx_queue_false.cpp: expected false (memsafety)
// reference from front() used after pop frees that element's node
#include <queue>
int cxx_queue_false(int c) {
    std::queue<int> q;
    q.push(10);
    q.push(20);
    int& r = q.front();  // 10
    if (c) q.pop();      // c != 0: frees the node r refers to
    return r;
}
