// PRISM conformance task cxx/cxx_map_erase_uaf_true.cpp: expected true (memsafety)
// iterator to a std::map element used after that element is erased (guarded:
// erasing another key leaves the iterator valid)
#include <map>
int cxx_map_erase_uaf_true(int c) {
    std::map<int, int> m;
    m[1] = 10;
    m[2] = 20;
    auto it = m.find(2);
    m.erase(1);  // another element: `it` stays valid
    return c ? it->second : 0;
}
