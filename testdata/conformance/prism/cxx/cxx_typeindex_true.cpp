// PRISM conformance task cxx/cxx_typeindex_true.cpp: expected true (no-oob)
// type_index stores the type_info address. operator== is type_info::operator==.
// That comparison is not a proof of this function: both branches read buf[0].
#include <typeindex>
int cxx_typeindex_true(int n) {
    const std::type_index a(typeid(int));
    const std::type_index b(typeid(int));
    const std::type_index c(typeid(double));
    int buf[1] = {n};
    if (a.name() == nullptr || c.name() == nullptr) return buf[0];
    if ((a == b) && !(a == c) && a.hash_code() == b.hash_code() && a.name() == b.name()) return buf[0];
    return buf[0];
}
