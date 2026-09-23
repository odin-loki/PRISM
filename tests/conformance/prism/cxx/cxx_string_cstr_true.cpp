// PRISM conformance task cxx/cxx_string_cstr_true.cpp: expected true (memsafety)
// c_str() used after the string is destroyed (guarded)
#include <string>
int cxx_string_cstr_true(int c) {
    std::string s("a string longer than the small buffer");
    const char *p = s.c_str();
    return p[c & 7];
}
