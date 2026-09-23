// PRISM conformance task cxx/cxx_string_substr_false.cpp: expected false (no-uncaught)
// std::string::substr past size() throws out_of_range out of a noexcept function
#include <string>
int cxx_string_substr_false(int p) noexcept {
    std::string s("abcd");
    if (p < 0 || p > 5) return 0;
    return static_cast<int>(s.substr(static_cast<std::size_t>(p)).size());  // p = 5 throws
}
