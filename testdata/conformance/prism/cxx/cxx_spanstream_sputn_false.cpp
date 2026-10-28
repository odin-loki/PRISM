// PRISM conformance task cxx/cxx_spanstream_sputn_false.cpp: expected false (no-oob)
// c == 0: sputn with a negative count. The model checks FUNC-CONTRACT, then
// traps, and does not write.
// c != 0: two characters into a 4-byte buffer.
#include <spanstream>
int cxx_spanstream_sputn_false(int c) {
    char buf[4] = {};
    char src[4] = {'A', 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, 4);
    if (c != 0) return static_cast<int>(b.sputn(src, 2));
    return static_cast<int>(b.sputn(src, -1));
}
