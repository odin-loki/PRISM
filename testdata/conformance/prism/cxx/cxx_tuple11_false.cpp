// PRISM conformance task cxx/cxx_tuple11_false.cpp: expected false (memsafety)
// std::get<10> returns the stored 11, one past a 11-int buffer
#include <tuple>
int cxx_tuple11_false(int c) {
    std::tuple<int, int, int, int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, c);
    int buf[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    if (c < 0 || c > 11) return 0;
    return buf[std::get<10>(t)];
}
