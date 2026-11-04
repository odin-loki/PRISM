// PRISM conformance task cxx/cxx_tuple13_true.cpp: expected true (memsafety)
// std::get<12> reads the int that was stored as the thirteenth element.
#include <tuple>
int cxx_tuple13_true(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, c);
    return std::get<12>(t);
}
