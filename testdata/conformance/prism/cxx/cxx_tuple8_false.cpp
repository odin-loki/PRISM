// PRISM conformance task cxx/cxx_tuple8_false.cpp: expected false (memsafety)
// std::get<7> returns the stored 8, one past a 8-int buffer
#include <tuple>
int cxx_tuple8_false(int c) {
    std::tuple<int, int, int, int, int, int, int, int> t(0, 1, 2, 3, 4, 5, 6, c);
    int buf[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    if (c < 0 || c > 8) return 0;
    return buf[std::get<7>(t)];
}
