// PRISM conformance task cxx/cxx_string_cstr_false.cpp: expected false (memsafety)
// c_str() used after the string is destroyed
#include <string>
int cxx_string_cstr_false(int c) {
    const char *p;
    {
        std::string s("a string longer than the small buffer");
        p = s.c_str();
    }
    return p[c & 7];  // the string's buffer is gone
}
