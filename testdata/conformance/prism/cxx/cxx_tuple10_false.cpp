// PRISM conformance task cxx/cxx_tuple10_false.cpp: expected false (memsafety)
// std::get<9> returns the stored 10, one past a 10-int buffer
#include <tuple>
int cxx_tuple10_false(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, c);
    int buf[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    if (c < 0 || c > 10) return 0;
    return buf[std::get<9>(t)];
}
