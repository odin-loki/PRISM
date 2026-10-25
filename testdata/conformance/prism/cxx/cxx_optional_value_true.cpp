// PRISM conformance task cxx/cxx_optional_value_true.cpp: expected true (no-uncaught)
// std::optional::value() on an empty optional in a noexcept function (guarded)
#include <optional>
int cxx_optional_value_true(int c) noexcept {
    std::optional<int> o;
    if (c) o = c & 7;
    return o.value_or(-1);
}
