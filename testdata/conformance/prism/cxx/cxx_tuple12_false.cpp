// PRISM conformance task cxx/cxx_tuple12_false.cpp: expected false (memsafety)
// std::get<11> returns the stored 12, one past a 12-int buffer
#include <tuple>
int cxx_tuple12_false(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, c);
    int buf[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    if (c < 0 || c > 12) return 0;
    return buf[std::get<11>(t)];
}
