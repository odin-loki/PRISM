// PRISM conformance task cxx/cxx_list_true.cpp: expected true (memsafety)
// iterator to a std::list element used after a different element is erased
#include <list>
int cxx_list_true(int c) {
    std::list<int> l;
    l.push_back(10);
    l.push_back(20);
    auto it = l.begin();  // 10
    auto kill = it;
    ++kill;                // 20
    if (c) l.erase(kill);  // `it` stays valid
    return *it;
}
