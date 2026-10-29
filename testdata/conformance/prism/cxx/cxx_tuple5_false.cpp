// PRISM conformance task cxx/cxx_tuple5_false.cpp: expected false (memsafety)
// std::get<4> returns the stored 5, one past a 5-int buffer
#include <tuple>
int cxx_tuple5_false(int c) {
    std::tuple<int, int, int, int, int> t(0, 1, 2, 3, c);
    int buf[5] = {0, 1, 2, 3, 4};
    if (c < 0 || c > 5) return 0;
    return buf[std::get<4>(t)];
}
