// PRISM conformance task cxx/cxx_tuple6_true.cpp: expected true (memsafety)
// std::get<5> reads the int that was stored as the sixth element.
#include <tuple>
int cxx_tuple6_true(int c) {
    std::tuple<int, int, int, int, int, int> t(0, 1, 2, 3, 4, c);
    return std::get<5>(t);
}