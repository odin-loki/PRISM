// PRISM conformance task cxx/cxx_tuple7_false.cpp: expected false (memsafety)
// std::get<6> returns the stored 7, one past a 7-int buffer
#include <tuple>
int cxx_tuple7_false(int c) {
    std::tuple<int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, c);
    int buf[7] = {0, 1, 2, 3, 4, 5, 6};
    if (c < 0 || c > 7) return 0;
    return buf[std::get<6>(t)];
}
