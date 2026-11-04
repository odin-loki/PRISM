// PRISM conformance task cxx/cxx_tuple11_true.cpp: expected true (memsafety)
// std::get<10> reads the int that was stored as the eleventh element.
#include <tuple>
int cxx_tuple11_true(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, c);
    return std::get<10>(t);
}
