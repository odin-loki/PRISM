// PRISM conformance task cxx/cxx_deque_true.cpp: expected true (memsafety)
// reference kept across push_back (deque does not invalidate references
// to existing elements when inserting at the end)
#include <deque>
int cxx_deque_true(int c) {
    std::deque<int> d;
    d.push_back(1);
    int& r = d.front();
    d.push_back(c & 7);  // r stays valid
    return r + d.back();
}
