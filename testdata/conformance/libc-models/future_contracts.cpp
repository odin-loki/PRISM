// std::future operational model (src/prism/pir/models/cxx/future) against
// the part of C++23 [futures] this header models. The pir stage puts the
// model headers first on the include path, so <future> below is the model
// (the report's `extra.cxx_models` says "model: future"); see docs/PIR.md
// "C++ library models". Each function is one contract; `_false` ones must
// fail for the class their yml names. A default future has no shared state.
// wait, wait_for, wait_until, async, and packaged_task are not part of the
// model.
#include <cassert>
#include <future>

// set_value stores one int. The associated future reads it once.
int future_roundtrip_true(int x) {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    assert(f.valid());
    p.set_value(x);
    int v = f.get();
    assert(v == x);
    assert(!f.valid());
    return 0;
}

// A default future has no shared state.
int future_default_true(int) {
    std::future<int> f;
    assert(!f.valid());
    return 0;
}

// get() on a default future. Not a value-initialised int.
int future_get_empty_false(int c) {
    std::future<int> f;
    if (c) return c;
    return f.get();
}

// A second set_value is a check. The first value is not overwritten.
int future_set_twice_false(int x) {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    p.set_value(x);
    p.set_value(x);
    return f.get();
}

// A second get() is a check. The stored value was already read.
int future_get_twice_false(int x) {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    p.set_value(x);
    (void)f.get();
    return f.get();
}
