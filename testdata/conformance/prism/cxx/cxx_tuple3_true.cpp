// PRISM conformance task cxx/cxx_tuple3_true.cpp: expected true (memsafety)
// std::get<2> reads the int that was stored as the third element.
#include <tuple>
int cxx_tuple3_true(int c) {
    std::tuple<int, int, int> t(0, 1, c);
    return std::get<2>(t);
}
