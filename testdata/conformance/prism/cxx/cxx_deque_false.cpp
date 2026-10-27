// PRISM conformance task cxx/cxx_deque_false.cpp: expected false (memsafety)
// iterator kept across push_back (deque invalidates every iterator; the
// reference to the existing element would still be valid)
#include <deque>
int cxx_deque_false(int c) {
    std::deque<int> d;
    d.push_back(1);
    auto it = d.begin();
    if (c) d.push_back(2);  // invalidates it
    return *it;
}
