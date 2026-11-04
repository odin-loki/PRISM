// PRISM conformance task cxx/cxx_tuple9_false.cpp: expected false (memsafety)
// std::get<8> returns the stored 9, one past a 9-int buffer
#include <tuple>
int cxx_tuple9_false(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, c);
    int buf[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    if (c < 0 || c > 9) return 0;
    return buf[std::get<8>(t)];
}
