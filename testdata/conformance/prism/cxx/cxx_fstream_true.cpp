// PRISM conformance task cxx/cxx_fstream_true.cpp: expected true (memsafety)
// A default ifstream is not open. is_open() is false. read and get are not
// called. This is not a proof that a file exists.
#include <fstream>
int cxx_fstream_true(int) {
    std::ifstream in;
    if (in.is_open()) return 1;
    return 0;
}
