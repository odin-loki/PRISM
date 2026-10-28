// PRISM conformance task cxx/cxx_tuple7_true.cpp: expected true (memsafety)
// std::get<6> reads the int that was stored as the seventh element.
#include <tuple>
int cxx_tuple7_true(int c) {
    std::tuple<int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, c);
    return std::get<6>(t);
}
