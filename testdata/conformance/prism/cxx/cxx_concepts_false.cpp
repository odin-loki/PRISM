// PRISM conformance task cxx/cxx_concepts_false.cpp: expected false (no-overflow)
// constrained template overflows
#include <concepts>
template <std::integral T> T twice(T v) { return v * 2; }
int cxx_concepts_false(int x) { return twice(x); }
