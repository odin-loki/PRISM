// PRISM conformance task cxx/cxx_variant_false.cpp: expected false (memsafety)
// std::get of the alternative that is not stored
#include <variant>
int cxx_variant_false(int c) {
    std::variant<int, char> v;
    v.emplace<int>(10);
    if (!c) v.emplace<char>('a');
    return std::get<char>(v);
}
