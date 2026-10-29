// PRISM conformance task cxx/cxx_tuple_false.cpp: expected false (memsafety)
// std::get<0> returns the stored 1, one past a 1-int buffer
#include <tuple>
int cxx_tuple_false(int c) {
    std::tuple<int> t(c);
    int buf[1] = {0};
    if (c < 0 || c > 1) return 0;
    return buf[std::get<0>(t)];
}
