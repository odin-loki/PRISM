// PRISM conformance task cxx/cxx_forward_list_true.cpp: expected true (memsafety)
// iterator to a std::forward_list element used after a different element is erased
#include <forward_list>
int cxx_forward_list_true(int c) {
    std::forward_list<int> l;
    l.push_front(20);
    l.push_front(10);
    auto it = l.begin();  // 10
    if (c) l.erase_after(it);  // erases 20; `it` stays valid
    return *it;
}
