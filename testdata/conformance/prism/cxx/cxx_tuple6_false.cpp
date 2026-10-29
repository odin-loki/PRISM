// PRISM conformance task cxx/cxx_tuple6_false.cpp: expected false (memsafety)
// std::get<5> returns the stored 6, one past a 6-int buffer
#include <tuple>
int cxx_tuple6_false(int c) {
    std::tuple<int, int, int, int, int, int> t(0, 1, 2, 3, 4, c);
    int buf[6] = {0, 1, 2, 3, 4, 5};
    if (c < 0 || c > 6) return 0;
    return buf[std::get<5>(t)];
}
