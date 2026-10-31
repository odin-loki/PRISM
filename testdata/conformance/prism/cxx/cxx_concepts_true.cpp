// PRISM conformance task cxx/cxx_concepts_true.cpp: expected true (no-overflow)
// constrained template (concepts)
#include <concepts>
template <std::integral T> T half(T v) { return v / 2; }
int cxx_concepts_true(int x) { return half(x) + 1; }
