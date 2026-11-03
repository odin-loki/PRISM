// PRISM conformance task cxx/cxx_spaceship_false.cpp: expected false (no-overflow)
// three-way comparison picks the distance, which overflows at the extremes
#include <compare>
int cxx_spaceship_false(int a, int b) {
    auto c = a <=> b;
    return c < 0 ? b - a : a - b;
}
