// std::generator model (src/prism/pir/models/cxx/generator) against C++23
// [coro.generator]. The pir stage puts the model headers first, so
// <generator> below is the model. Each function is one contract.
#include <cassert>
#include <generator>

static std::generator<int> count(int n) {
    for (int i = 0; i < n; ++i) co_yield i;
}
static std::generator<int> nested() {
    co_yield 1;
    co_yield std::ranges::elements_of(count(3));
    co_yield 9;
}

// yields 0,1,2 in order and then ends
int generator_order_true(int c) {
    if (c == 0) return 0;
    int s = 0, k = 0;
    for (int v : count(3)) { assert(v == k); ++k; s += v; }
    assert(k == 3 && s == 3);
    return 0;
}

// elements_of delegates to the inner generator: 1,0,1,2,9
int generator_elements_of_true(int c) {
    if (c == 0) return 0;
    int s = 0;
    for (int v : nested()) s += v;
    assert(s == 13);
    return 0;
}

// a wrong sum must fail
int generator_sum_false(int c) {
    if (c == 0) return 0;
    int s = 0;
    for (int v : count(3)) s += v;
    assert(s == 4);
    return 0;
}
