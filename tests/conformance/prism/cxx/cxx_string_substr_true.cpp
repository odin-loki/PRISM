// PRISM conformance task cxx/cxx_string_substr_true.cpp: expected true (no-uncaught)
// std::string::substr past size() throws out_of_range out of a noexcept function (guarded)
#include <string>
int cxx_string_substr_true(int p) noexcept {
    std::string s("abcd");
    if (p < 0 || p > 4) return 0;
    return static_cast<int>(s.substr(static_cast<std::size_t>(p)).size());
}
