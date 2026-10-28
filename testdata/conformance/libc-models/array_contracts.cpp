// std::array operational model (src/prism/pir/models/cxx/array) against
// C++23 [array]. The pir stage puts the model headers first on the include
// path, so <array> below is the model (the report's `extra.cxx_models` says
// "model: array"); see docs/PIR.md "C++ library models". Each function is
// one contract; `_false` ones must fail for the class their yml names. The
// elements are the array object: there is no heap. N == 0 has no element.
// operator== is two arrays of the same N. N == 0 is true and does not read
// an element. N > 0 is true only when every corresponding element compares
// equal with operator==. A mismatch is false.
// This is not PROVED and not CLEAN. Iterators, operator<=>, std::get, and
// std::to_array are not part of the model.
#include <array>
#include <cassert>
#include <stdexcept>

// [array.overview]: index, at, ends, fill, swap, and data() stay on the object.
int array_access_true(int x) {
    std::array<int, 4> a{1, 2, 3, x};
    assert(a.size() == 4);
    assert(!a.empty());
    assert(a.front() == 1);
    assert(a.back() == x);
    assert(a[1] == 2);
    assert(a.at(2) == 3);
    assert(a.data()[0] == 1);
    const int* p = a.data();
    a.fill(7);
    assert(a.data() == p);
    assert(a[0] == 7 && a.back() == 7);
    std::array<int, 4> b{4, 5, 6, 8};
    a.swap(b);
    assert(a.data() == p);
    assert(a.front() == 4 && a.back() == 8);
    assert(b.front() == 7 && b.back() == 7);
    return 0;
}

// N == 0 is empty: size, data() == nullptr, fill and swap touch no element.
int array_empty_true(int c) {
    std::array<int, 0> z{};
    assert(z.empty());
    assert(z.size() == 0);
    assert(z.max_size() == 0);
    assert(z.data() == nullptr);
    z.fill(c);
    std::array<int, 0> w{};
    if (c) z.swap(w);
    assert(z.empty() && w.empty());
    assert(w.data() == nullptr);
    return 0;
}

// operator[] at size() (libstdc++ precondition __n < size()).
int array_index_oob_false(int i) {
    if (i < 0 || i > 4) return 0;
    std::array<int, 4> a{10, 11, 12, 13};
    return a[static_cast<std::size_t>(i)];
}

// [array.members] at(n) throws out_of_range iff n >= N.
int array_at_true(int i) {
    if (i < 0 || i > 5) return 0;
    std::array<int, 4> a{1, 2, 3, 4};
    bool threw = false;
    try {
        (void)a.at(static_cast<std::size_t>(i));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw == (i >= 4));
    return 0;
}

// A wrong contract: at() never throws.
int array_at_nothrow_false(int i) {
    if (i < 0 || i > 5) return 0;
    std::array<int, 4> a{1, 2, 3, 4};
    bool threw = false;
    try {
        (void)a.at(static_cast<std::size_t>(i));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(!threw);
    return 0;
}

// front() on N == 0 (precondition !empty()). Not an element.
int array_front_empty_false(int c) {
    std::array<int, 0> a{};
    if (c) {
        std::array<int, 1> one{1};
        return one.front();
    }
    return a.front();
}

// back() on N == 0 (precondition !empty()).
int array_back_empty_false(int c) {
    std::array<int, 0> a{};
    if (c) {
        std::array<int, 1> one{c};
        return one.back();
    }
    return a.back();
}

// A wrong contract: front() is not the last element.
int array_order_false(int a, int b) {
    std::array<int, 2> x{a, b};
    assert(x.front() == b);
    return 0;
}

// [array.syn] operator== is true only when every corresponding element is.
int array_equal_true(int x) {
    std::array<int, 3> a{1, 2, x};
    std::array<int, 3> same{1, 2, x};
    assert(a == same);
    std::array<int, 3> head{0, 2, x};
    assert(!(a == head));
    std::array<int, 3> mid{1, 0, x};
    assert(!(a == mid));
    std::array<int, 3> tail{1, 2, 7};
    assert((a == tail) == (x == 7));
    std::array<int, 1> one{x};
    std::array<int, 1> one_same{x};
    assert(one == one_same);
    std::array<int, 1> other{x == 0 ? 1 : 0};
    assert(!(one == other));
    return 0;
}

// array<T, 0> compared with array<T, 0> is true. No element is read.
int array_equal_empty_true(int c) {
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    assert(z == w);
    if (c) assert(z == w);
    return 0;
}

// A wrong contract: a mismatch at index 0 still compares equal.
int array_equal_mismatch_false(int x) {
    std::array<int, 2> a{1, x};
    std::array<int, 2> b{0, x};
    assert(a == b);
    return 0;
}
