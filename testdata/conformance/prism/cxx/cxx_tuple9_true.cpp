// PRISM conformance task cxx/cxx_tuple9_true.cpp: expected true (memsafety)
// std::get<8> reads the int that was stored as the ninth element.
#include <tuple>
int cxx_tuple9_true(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, c);
    return std::get<8>(t);
}
