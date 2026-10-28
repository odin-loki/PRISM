// PRISM conformance task cxx/cxx_tuple4_true.cpp: expected true (memsafety)
// std::get<3> reads the int that was stored as the fourth element.
#include <tuple>
int cxx_tuple4_true(int c) {
    std::tuple<int, int, int, int> t(0, 1, 2, c);
    return std::get<3>(t);
}
