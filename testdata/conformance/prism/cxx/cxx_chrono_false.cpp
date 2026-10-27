// PRISM conformance task cxx/cxx_chrono_false.cpp: expected false (no-overflow)
// A duration whose count is the maximum int64, plus a duration of 1.
#include <chrono>
int cxx_chrono_false(int c) {
    (void)c;
    std::chrono::duration<long long> a(9223372036854775807LL);
    std::chrono::duration<long long> b(1);
    return static_cast<int>((a + b).count());
}
