// PRISM conformance task cxx/cxx_optional_false.cpp: expected false (memsafety)
// std::optional::value() on an empty optional
#include <optional>
int cxx_optional_false(int c) {
    std::optional<int> o;
    if (c) return c;
    return o.value();
}
