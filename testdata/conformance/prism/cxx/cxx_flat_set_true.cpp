// PRISM conformance task cxx/cxx_flat_set_true.cpp: expected true (memsafety)
// insert one key; contains is true; the destructor frees that node
#include <flat_set>
int cxx_flat_set_true(int k) {
    std::flat_set<int> s;
    s.insert(k);
    return s.contains(k) ? 1 : 0;
}
