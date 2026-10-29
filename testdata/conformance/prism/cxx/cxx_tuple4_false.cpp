// PRISM conformance task cxx/cxx_tuple4_false.cpp: expected false (memsafety)
// std::get<3> returns the stored 4, one past a 4-int buffer
#include <tuple>
int cxx_tuple4_false(int c) {
    std::tuple<int, int, int, int> t(0, 1, 2, c);
    int buf[4] = {0, 1, 2, 3};
    if (c < 0 || c > 4) return 0;
    return buf[std::get<3>(t)];
}
