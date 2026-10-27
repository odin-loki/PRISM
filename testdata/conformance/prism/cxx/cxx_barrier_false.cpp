// PRISM conformance task cxx/cxx_barrier_false.cpp: expected false
// arrive past the expected count
#include <barrier>
int cxx_barrier_false(int n) {
    std::barrier b(1);
    if (n <= 1) return 0;
    b.arrive(n);
    return 0;
}
