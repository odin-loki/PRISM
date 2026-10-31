// std::atomic model (src/prism/pir/models/cxx/atomic) against C++23
// [atomics.types.generic]. Every operation is a scheduler switch point.
#include <atomic>
#include <cassert>

// fetch_add returns the old value; exchange and load agree
int atomic_ops_true(int c) {
    if (c == 0) return 0;
    std::atomic<int> a{1};
    assert(a.fetch_add(2) == 1);
    assert(a.exchange(7) == 3);
    assert(a.load() == 7);
    return 0;
}

// compare_exchange_strong with a wrong expected value fails and updates it
int atomic_cas_true(int c) {
    if (c == 0) return 0;
    std::atomic<int> a{5};
    int e = 4;
    assert(!a.compare_exchange_strong(e, 9));
    assert(e == 5 && a.load() == 5);
    return 0;
}

// compare_exchange_weak may fail spuriously, so assuming success must fail
int atomic_weak_false(int c) {
    if (c == 0) return 0;
    std::atomic<int> a{5};
    int e = 5;
    assert(a.compare_exchange_weak(e, 9));
    return 0;
}
