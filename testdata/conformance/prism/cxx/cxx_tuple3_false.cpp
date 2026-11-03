// PRISM conformance task cxx/cxx_tuple3_false.cpp: expected false (memsafety)
// std::get<2> returns the stored 3, one past a 3-int buffer
#include <tuple>
int cxx_tuple3_false(int c) {
    std::tuple<int, int, int> t(0, 1, c);
    int buf[3] = {0, 1, 2};
    if (c < 0 || c > 3) return 0;
    return buf[std::get<2>(t)];
}
