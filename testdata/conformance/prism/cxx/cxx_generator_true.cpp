// PRISM conformance task cxx/cxx_generator_true.cpp: expected true (noassert)
// std::generator yields 0..n-1; the sum is n(n-1)/2 for small n
#include <generator>
#include <cassert>
static std::generator<int> upto(int n) { for (int i = 0; i < n; ++i) co_yield i; }
int cxx_generator_true(int c) {
    int n = c & 3, s = 0;
    for (int x : upto(n)) s += x;
    assert(s == n * (n - 1) / 2);
    return s;
}
