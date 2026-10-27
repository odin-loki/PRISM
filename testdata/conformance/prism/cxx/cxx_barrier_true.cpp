// PRISM conformance task cxx/cxx_barrier_true.cpp: expected true
// arrive exactly the expected count; do not call wait
#include <barrier>
int cxx_barrier_true(int n) {
    if (n <= 0) return 0;
    std::barrier b(n);
    b.arrive(n);
    return 0;
}
