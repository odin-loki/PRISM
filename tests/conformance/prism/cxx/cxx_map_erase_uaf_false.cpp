// PRISM conformance task cxx/cxx_map_erase_uaf_false.cpp: expected false (memsafety)
// iterator to a std::map element used after that element is erased
#include <map>
int cxx_map_erase_uaf_false(int c) {
    std::map<int, int> m{{1, 10}, {2, 20}, {3, 30}};
    auto it = m.find(2);
    m.erase(c & 3);  // c = 2 erases the element `it` refers to
    return it->second;
}
