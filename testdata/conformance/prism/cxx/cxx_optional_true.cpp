// PRISM conformance task cxx/cxx_optional_true.cpp: expected true (memsafety)
// read the value stored in an engaged std::optional
#include <optional>
int cxx_optional_true(int c) {
    std::optional<int> o(c);
    return o.value();
}
