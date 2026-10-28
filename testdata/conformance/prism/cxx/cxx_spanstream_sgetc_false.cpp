// PRISM conformance task cxx/cxx_spanstream_sgetc_false.cpp: expected false (no-oob)
// c == 0: sgetc when the offset is already the size (size 0). The model
// checks MEM-OOB-READ and does not read the pointer.
#include <spanstream>
int cxx_spanstream_sgetc_false(int c) {
    char buf[4] = {'A', 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, c == 0 ? 0 : 4);
    return b.sgetc();
}
