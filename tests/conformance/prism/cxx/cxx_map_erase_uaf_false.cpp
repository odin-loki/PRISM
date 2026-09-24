// PRISM conformance task cxx/cxx_map_erase_uaf_false.cpp: expected false (memsafety)
// iterator to a std::map element used after that element is erased
#include <map>
int cxx_map_erase_uaf_false(int c) {
    std::map<int, int> m;
    m[1] = 10;
    m[2] = 20;
    auto it = m.find(c ? 1 : 2);  // c != 0: the element erased below
    m.erase(1);
    return it->second;
}
