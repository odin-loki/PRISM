// PRISM conformance task cxx/cxx_spanstream_sungetc_true.cpp: expected true (no-oob)
// sgetc of the first byte, then sungetc returns that byte. 255 stays 255.
#include <spanstream>
int cxx_spanstream_sungetc_true(int) {
    char buf[4] = {static_cast<char>(static_cast<unsigned char>(255)), 'B', 'C', 'D'};
    std::basic_spanbuf<char> b(buf, 4);
    b.sgetc();
    return b.sungetc();
}
