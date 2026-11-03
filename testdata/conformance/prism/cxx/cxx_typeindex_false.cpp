// PRISM conformance task cxx/cxx_typeindex_false.cpp: expected false (no-oob)
// two type_index of int compare equal, so the index is 1 on a one-int buffer
#include <typeindex>
#include <typeinfo>
int cxx_typeindex_false(int n) {
    const std::type_index a(typeid(int));
    const std::type_index b(typeid(int));
    int buf[1] = {0};
    const int idx = (a == b) ? 1 : 0;
    if (n != 1) return 0;
    return buf[idx];
}
