// PRISM conformance task cxx/cxx_sstream_long_true.cpp: expected true (no-overflow)
#include <sstream>
int cxx_sstream_long_true(int x) {
    std::ostringstream o;
    o << 12345678901LL;
    return x < 100 ? static_cast<int>(o.str().size()) + x : 0;  // 11 + x, bounded
}
