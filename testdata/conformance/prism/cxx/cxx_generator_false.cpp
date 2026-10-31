// PRISM conformance task cxx/cxx_generator_false.cpp: expected false (noassert)
// the generator yields nothing when c == 0, so the first element is absent
#include <generator>
#include <cassert>
static std::generator<int> upto(int n) { for (int i = 0; i < n; ++i) co_yield i + 1; }
int cxx_generator_false(int c) {
    int first = 0;
    for (int x : upto(c & 1)) { first = x; break; }
    assert(first == 1);
    return first;
}
