// PRISM conformance task cxx/cxx_bitset_true.cpp: expected true (no-oob)
// std::bitset::operator[] at N (guarded)
#include <bitset>
int cxx_bitset_true(int i) {
    std::bitset<4> b;
    b.set(0);
    if (i < 0 || i >= 4) return 0;
    return b[static_cast<std::size_t>(i)];
}
