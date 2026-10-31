// PRISM conformance task cxx/cxx_spaceship_true.cpp: expected true (no-overflow)
// three-way comparison
#include <compare>
int cxx_spaceship_true(int a, int b) {
    auto c = a <=> b;
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}
