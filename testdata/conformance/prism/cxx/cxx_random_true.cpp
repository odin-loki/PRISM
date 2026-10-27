// PRISM conformance task cxx/cxx_random_true.cpp: expected true (no-overflow)
// minstd_rand seeded with 1 yields the Park-Miller step 48271.
// discard(0) leaves the next output unchanged relative to a twin engine.
#include <random>
int cxx_random_true(int c) {
    if (c < 0) return 0;
    std::minstd_rand a;
    a.seed(1);
    if (a() != 48271u) return 1;
    std::minstd_rand b;
    b.seed(1);
    std::minstd_rand twin;
    twin.seed(1);
    b.discard(0);
    if (b() != twin()) return 1;
    return 0;
}
