// PRISM conformance task cxx/cxx_variant_true.cpp: expected true (memsafety)
// read only the alternative that was stored
#include <variant>
int cxx_variant_true(int c) {
    std::variant<int, char> v;
    if (c) {
        v.emplace<int>(c);
        return std::get<int>(v);
    }
    v.emplace<char>('b');
    return std::get<char>(v);
}
