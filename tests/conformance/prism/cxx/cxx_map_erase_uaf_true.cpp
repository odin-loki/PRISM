// PRISM conformance task cxx/cxx_map_erase_uaf_true.cpp: expected true (memsafety)
// iterator to a std::map element used after that element is erased (guarded:
// erasing another key leaves the iterator valid)
#include <map>
int cxx_map_erase_uaf_true(int c) {
    std::map<int, int> m{{1, 10}, {2, 20}, {3, 30}};
    auto it = m.find(2);
    if ((c & 3) != 2) m.erase(c & 3);
    return it->second;
}
