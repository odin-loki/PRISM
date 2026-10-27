// PRISM conformance task cxx/cxx_list_false.cpp: expected false (memsafety)
// iterator to a std::list element used after that element is erased
#include <list>
int cxx_list_false(int c) {
    std::list<int> l;
    l.push_back(10);
    l.push_back(20);
    auto it = l.begin();       // 10
    if (!c) ++it;              // 20
    l.erase(l.begin());        // c != 0: erases the element `it` refers to
    return *it;
}
