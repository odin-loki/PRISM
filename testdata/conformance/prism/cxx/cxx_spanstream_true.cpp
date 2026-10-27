// PRISM conformance task cxx/cxx_spanstream_true.cpp: expected true (no-oob)
// one sputc into a 4-byte caller buffer, then that byte read back
#include <spanstream>
int cxx_spanstream_true(int) {
    char buf[4] = {};
    std::basic_spanbuf<char> b(buf, 4);
    b.sputc('A');
    return buf[0];
}
