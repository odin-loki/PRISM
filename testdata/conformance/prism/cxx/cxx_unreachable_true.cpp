// PRISM conformance task cxx/cxx_unreachable_true.cpp: expected true (no-overflow)
// std::unreachable after an exhaustive switch
#include <utility>
int cxx_unreachable_true(int x) {
    if (x < 0 || x > 10) return 0;
    switch (x % 2) { case 0: return x + 1; case 1: return x - 1; }
    std::unreachable();
}
