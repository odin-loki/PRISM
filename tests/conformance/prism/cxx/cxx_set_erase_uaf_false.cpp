// PRISM conformance task cxx/cxx_set_erase_uaf_false.cpp: expected false (memsafety)
// std::set iterator used after its element is erased through another iterator
#include <set>
int cxx_set_erase_uaf_false(int c) {
    std::set<int> s{3, 1, 2};
    auto it = s.begin();       // 1
    s.insert(c & 7);           // insertion never invalidates
    s.erase(s.begin());        // c = 0 inserts a new smallest element: `it` survives
    return *it;                // otherwise `it` was erased
}
