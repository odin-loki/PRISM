// PRISM conformance task cxx/cxx_bitset_false.cpp: expected false (no-oob)
// std::bitset::operator[] at N. test() past N throws std::out_of_range
// instead (bitset_contracts.cpp); this task is the subscript precondition.
#include <bitset>
int cxx_bitset_false(int i) {
    std::bitset<4> b;
    b.set(0);
    if (i < 0 || i > 4) return 0;
    return b[static_cast<std::size_t>(i)];
}
