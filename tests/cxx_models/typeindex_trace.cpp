// Trace of std::type_index. Include <typeindex> and compare with
// type_info::operator==. Compiled against libstdc++ and against
// src/prism/pir/models/cxx (that directory first on the include path),
// the lines should match. tests/cpp/test_cxx_typeindex.cpp checks the
// model text.
//
// Equality of two type_index values is not a proof of a program's behavior.
// std::hash<type_index> returns hash_code(), so equal indexes hash the same.
#include <cstdio>
#include <typeindex>

int main() {
    const std::type_index a(typeid(int));
    const std::type_index b(typeid(int));
    const std::type_index c(typeid(double));

    const int same = (a == b) ? 1 : 0;
    const int diff = (a == c) ? 1 : 0;
    const int hash_ok = (a.hash_code() == typeid(int).hash_code() && c.hash_code() == typeid(double).hash_code()) ? 1 : 0;
    const int name_ok = (a.name() == typeid(int).name() && c.name() == typeid(double).name()) ? 1 : 0;
    const int less = (a < c) ? 1 : 0;
    const int before = typeid(int).before(typeid(double)) ? 1 : 0;
    const std::hash<std::type_index> hasher;
    const int std_hash = (hasher(a) == a.hash_code() && hasher(b) == b.hash_code() && hasher(a) == hasher(b) &&
                          hasher(c) == c.hash_code())
                             ? 1
                             : 0;

    std::printf("same %d\n", same);
    std::printf("diff %d\n", diff);
    std::printf("hash %d\n", hash_ok);
    std::printf("name %d\n", name_ok);
    std::printf("less %d\n", less);
    std::printf("stdhash %d\n", std_hash);

    if (!same) return 1;
    if (diff) return 2;
    if (!hash_ok) return 3;
    if (!name_ok) return 4;
    if (less != before) return 5;
    if (a.name() == nullptr || c.name() == nullptr) return 6;
    if (!std_hash) return 7;
    return 0;
}
