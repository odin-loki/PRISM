// PRISM conformance task cxx/cxx_set_erase_uaf_true.cpp: expected true (memsafety)
// std::set iterator used after its element is erased through another iterator
// (guarded: the iterator refers to the element after the erased one)
#include <set>
int cxx_set_erase_uaf_true(int c) {
    std::set<int> s;
    s.insert(2);
    s.insert(1);
    auto it = s.begin();
    ++it;                      // 2; insertion and erasure of others keep it valid
    s.insert(0);
    s.erase(s.begin());        // erases 0
    return c ? *it : 0;
}
