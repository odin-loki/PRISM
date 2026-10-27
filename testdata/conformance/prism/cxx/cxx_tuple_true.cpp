// PRISM conformance task cxx/cxx_tuple_true.cpp: expected true (memsafety)
// std::get<0> reads the int that was stored in the tuple.
#include <tuple>
int cxx_tuple_true(int c) {
    std::tuple<int> t(c);
    return std::get<0>(t);
}
