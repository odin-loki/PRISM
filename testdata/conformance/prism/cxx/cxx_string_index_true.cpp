// PRISM conformance task cxx/cxx_string_index_true.cpp: expected true (no-oob)
// std::string::operator[] past size() (guarded)
#include <string>
int cxx_string_index_true(int i) {
    std::string s("abcdefghijklmnopqrstuvwxyz");
    if (i < 0 || i > 26) return 0;
    return s[static_cast<std::size_t>(i)];  // s[26] is the terminator
}
