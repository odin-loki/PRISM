// PRISM conformance task cxx/cxx_spanstream_sputn_true.cpp: expected true (no-oob)
// sputn of 6 characters into a 4-byte buffer. The write stops at the buffer.
#include <spanstream>
int cxx_spanstream_sputn_true(int) {
    char buf[4] = {};
    char src[6] = {'A', 'B', 'C', 'D', 'E', 'F'};
    std::basic_spanbuf<char> b(buf, 4);
    return static_cast<int>(b.sputn(src, 6));
}
