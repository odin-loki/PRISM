// PRISM conformance task cxx/cxx_optional_value_false.cpp: expected false (no-uncaught)
// std::optional::value() on an empty optional in a noexcept function
#include <optional>
int cxx_optional_value_false(int c) noexcept {
    std::optional<int> o;
    if (c) o = c & 7;
    return o.value();  // bad_optional_access escapes noexcept
}
