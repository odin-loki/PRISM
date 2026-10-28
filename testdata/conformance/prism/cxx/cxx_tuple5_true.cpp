// PRISM conformance task cxx/cxx_tuple5_true.cpp: expected true (memsafety)
// std::get<4> reads the int that was stored as the fifth element.
#include <tuple>
int cxx_tuple5_true(int c) {
    std::tuple<int, int, int, int, int> t(0, 1, 2, 3, c);
    return std::get<4>(t);
}
