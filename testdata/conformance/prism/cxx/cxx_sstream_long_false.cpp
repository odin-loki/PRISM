// PRISM conformance task cxx/cxx_sstream_long_false.cpp: expected false (no-overflow)
#include <sstream>
int cxx_sstream_long_false(int x) {
    std::ostringstream o;
    o << 12345678901LL;
    return static_cast<int>(o.str().size()) + x;  // 11 + x
}
