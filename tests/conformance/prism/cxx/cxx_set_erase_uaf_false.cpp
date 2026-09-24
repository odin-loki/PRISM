// PRISM conformance task cxx/cxx_set_erase_uaf_false.cpp: expected false (memsafety)
// std::set iterator used after its element is erased through another iterator
#include <set>
int cxx_set_erase_uaf_false(int c) {
    std::set<int> s;
    s.insert(2);
    s.insert(1);
    auto it = s.begin();       // 1
    if (!c) ++it;              // 2
    s.erase(s.begin());        // c != 0: erases *it
    return *it;
}
