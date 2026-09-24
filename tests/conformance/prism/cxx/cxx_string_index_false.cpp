// PRISM conformance task cxx/cxx_string_index_false.cpp: expected false (no-oob)
// std::string::operator[] past size()
#include <string>
int cxx_string_index_false(int i) {
    std::string s("abcdefghijklmnopqrstuvwxyz");
    if (i < 0 || i > 27) return 0;
    return s[static_cast<std::size_t>(i)];
}
