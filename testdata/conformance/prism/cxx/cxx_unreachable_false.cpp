// PRISM conformance task cxx/cxx_unreachable_false.cpp: expected false (no-overflow)
// std::unreachable after an exhaustive switch; the even branch overflows near INT_MAX
#include <utility>
int cxx_unreachable_false(int x) {
    if (x < 0) return 0;
    switch (x % 2) { case 0: return x + 2; case 1: return x - 1; }
    std::unreachable();
}
