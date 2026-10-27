// PRISM conformance task cxx/cxx_chrono_true.cpp: expected true (no-overflow)
// Adds two small durations and checks the count. Zero stays zero.
#include <chrono>
int cxx_chrono_true(int c) {
    (void)c;
    std::chrono::duration<long long> zero{};
    if (zero.count() != 0) return 1;
    std::chrono::duration<long long> a(2);
    std::chrono::duration<long long> b(3);
    if (a.count() != 2 || b.count() != 3) return 1;
    return (a + b).count() == 5 ? 0 : 1;
}
