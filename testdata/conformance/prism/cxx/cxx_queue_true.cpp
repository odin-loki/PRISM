// PRISM conformance task cxx/cxx_queue_true.cpp: expected true (memsafety)
// reference to an element that pop does not free (pop frees the front node
// only). push allocates a new node and leaves this one in place.
#include <queue>
int cxx_queue_true(int c) {
    std::queue<int> q;
    q.push(10);
    q.push(20);
    int& r = q.back();  // 20
    q.push(c & 7);      // new node; r stays valid
    q.pop();            // frees 10, not 20
    return r + q.front();
}
