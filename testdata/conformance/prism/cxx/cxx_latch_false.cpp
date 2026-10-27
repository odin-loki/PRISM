// PRISM conformance task cxx/cxx_latch_false.cpp: expected false (memsafety)
// count_down past zero is a precondition failure
#include <latch>
int cxx_latch_false(int n) {
    std::latch g(1);
    if (n <= 1) return 0;
    g.count_down(n);
    return 0;
}
