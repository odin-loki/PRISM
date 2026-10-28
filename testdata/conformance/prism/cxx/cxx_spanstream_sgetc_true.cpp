// PRISM conformance task cxx/cxx_spanstream_sgetc_true.cpp: expected true (no-oob)
// one sgetc of the first byte of a 4-byte caller buffer
#include <spanstream>
int cxx_spanstream_sgetc_true(int) {
    char buf[4] = {'A', 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, 4);
    return b.sgetc();
}
