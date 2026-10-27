// PRISM conformance task cxx/cxx_forward_list_false.cpp: expected false (memsafety)
// iterator to a std::forward_list element used after that element is erased
#include <forward_list>
int cxx_forward_list_false(int c) {
    std::forward_list<int> l;
    l.push_front(20);
    l.push_front(10);
    auto it = l.begin();            // 10
    if (!c) ++it;                   // 20
    l.erase_after(l.before_begin());  // c != 0: erases the element `it` refers to
    return *it;
}
