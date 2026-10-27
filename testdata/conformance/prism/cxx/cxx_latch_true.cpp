// PRISM conformance task cxx/cxx_latch_true.cpp: expected true (memsafety)
// Counts down to zero. try_wait is true only when the count is already zero.
#include <cassert>
#include <latch>
int cxx_latch_true(int n) {
    if (n < 1) n = 1;
    std::latch g(n);
    assert(!g.try_wait());
    g.count_down(n);
    assert(g.try_wait());
    return 0;
}
