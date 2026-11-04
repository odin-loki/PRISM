// PRISM conformance task cxx/cxx_tuple10_true.cpp: expected true (memsafety)
// std::get<9> reads the int that was stored as the tenth element.
#include <tuple>
int cxx_tuple10_true(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, c);
    return std::get<9>(t);
}
