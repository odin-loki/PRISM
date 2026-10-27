// PRISM conformance task cxx/cxx_spanstream_false.cpp: expected false (no-oob)
// c == 0: sputc one past a size-0 buffer (the offset is already the size)
#include <spanstream>
int cxx_spanstream_false(int c) {
    char buf[4] = {};
    std::basic_spanbuf<char> b(buf, c == 0 ? 0 : 4);
    b.sputc('A');
    return buf[0];
}
