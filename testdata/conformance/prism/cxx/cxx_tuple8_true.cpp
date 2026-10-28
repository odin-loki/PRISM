// PRISM conformance task cxx/cxx_tuple8_true.cpp: expected true (memsafety)
// std::get<7> reads the int that was stored as the eighth element.
#include <tuple>
int cxx_tuple8_true(int c) {
    std::tuple<int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, c);
    return std::get<7>(t);
}
