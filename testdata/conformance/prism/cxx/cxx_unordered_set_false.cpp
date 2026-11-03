// PRISM conformance task cxx/cxx_unordered_set_false.cpp: expected false (memsafety)
// find of a key that was never inserted is end(); dereferencing end() reads
// through a null node
#include <unordered_set>

int cxx_unordered_set_false(int k) {
    std::unordered_set<int> s;
    s.insert(0);
    return *s.find(k);
}
