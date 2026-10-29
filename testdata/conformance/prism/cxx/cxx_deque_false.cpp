// PRISM conformance task cxx/cxx_deque_false.cpp: expected false (memsafety)
// iterator kept across a swap with a temporary: the temporary takes the
// storage it points into and frees it, so the read is a use after free
#include <deque>
int cxx_deque_false(int c) {
    std::deque<int> d;
    d.push_back(1);
    auto it = d.begin();
    if (c) {
        std::deque<int>().swap(d);  // the temporary frees it
    }
    return *it;
}
