// PRISM conformance task cxx/cxx_unordered_set_true.cpp: expected true (memsafety)
// find of the key that was just inserted is not end(); dereferencing it reads
// the stored key
#include <unordered_set>

int cxx_unordered_set_true(int k) {
    std::unordered_set<int> s;
    s.insert(k);
    auto it = s.find(k);
    if (it == s.end()) return 1;
    return *it;
}
