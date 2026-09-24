// PRISM conformance task cxx/cxx_set_erase_uaf_true.cpp: expected true (memsafety)
// std::set iterator used after its element is erased through another iterator
// (guarded: a smaller element is inserted first, so the erased one is another)
#include <set>
int cxx_set_erase_uaf_true(int c) {
    std::set<int> s{3, 1, 2};
    auto it = s.begin();       // 1
    s.insert(-(c & 7) - 1);    // a new smallest element; insertion never invalidates
    s.erase(s.begin());        // erases the new element, not *it
    return *it;
}
