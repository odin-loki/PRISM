// PRISM conformance task cxx/cxx_tuple13_false.cpp: expected false (memsafety)
// std::get<12> returns the stored 13, one past a 13-int buffer
#include <tuple>
int cxx_tuple13_false(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, c);
    int buf[13] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    if (c < 0 || c > 13) return 0;
    return buf[std::get<12>(t)];
}
