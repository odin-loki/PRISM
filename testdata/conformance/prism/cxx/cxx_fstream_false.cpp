// PRISM conformance task cxx/cxx_fstream_false.cpp: expected false (memsafety)
// c == 0: read and get on a default ifstream. The stream is not open, so
// each call is a FUNC-CONTRACT check. The buffer is not written and get
// does not store a character.
#include <fstream>
int cxx_fstream_false(int c) {
    std::ifstream in;
    char buf[4] = {'a', 'b', 'c', 'd'};
    char g = 'Q';
    if (c != 0) return 0;
    in.read(buf, 4);
    in.get(g);
    (void)in.get();
    if (buf[0] != 'a' || buf[1] != 'b' || buf[2] != 'c' || buf[3] != 'd') return 1;
    if (g != 'Q') return 1;
    return 0;
}
