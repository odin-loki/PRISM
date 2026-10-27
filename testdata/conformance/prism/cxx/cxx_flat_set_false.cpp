// PRISM conformance task cxx/cxx_flat_set_false.cpp: expected false (memsafety)
// pointer saved from the iterator, then erase of that key, then a use
#include <flat_set>
int cxx_flat_set_false(int c) {
    std::flat_set<int> s;
    s.insert(1);
    const int* p = &*s.begin();
    if (c)
        s.erase(1);
    return *p;
}
