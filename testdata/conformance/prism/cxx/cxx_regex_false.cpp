// PRISM conformance task cxx/cxx_regex_false.cpp: expected false (memsafety)
// the match of a+ in aaa has length 3, one past a three-int buffer
#include <regex>
#include <string>
int cxx_regex_false(int n) {
    const std::regex re("a+");
    const std::string s = "aaa";
    std::smatch m;
    if (!std::regex_search(s, m, re)) return 0;
    int buf[3] = {0, 1, 2};
    if (n < 0 || n > 1) return 0;
    return buf[static_cast<std::size_t>(m.length(0)) + static_cast<std::size_t>(n)];
}
