// PRISM conformance task cxx/cxx_spanstream_sungetc_false.cpp: expected false (no-oob)
// c == 0: sungetc when the offset is already 0. The model checks
// MEM-OOB-READ, does not decrement, and does not read before the buffer.
// c != 0: one in-range sgetc, then sungetc of that byte.
#include <spanstream>
int cxx_spanstream_sungetc_false(int c) {
    char buf[4] = {'A', 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, 4);
    if (c != 0) b.sgetc();
    return b.sungetc();
}
