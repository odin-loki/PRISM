// std::array, std::span, std::optional and std::unique_ptr against C++23
// ([array], [views.span], [optional], [unique.ptr]). These are not replaced
// by PRISM models: libstdc++'s own header code is small, has no allocation
// or loop the encoder cannot afford, and with -D_GLIBCXX_ASSERTIONS (always
// on in the pir stage) it checks exactly the preconditions below, so the
// library code is its own sound model (docs/PIR.md "C++ library models").
// Each function is one contract; `_false` ones must fail for their class.
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

// [array.overview]: a[i] for i < N is the i-th element; size() == N.
int array_index_true(int i) {
    std::array<int, 4> a{10, 11, 12, 13};
    if (i < 0 || i >= 4) return 0;
    assert(a.size() == 4);
    assert(a[static_cast<std::size_t>(i)] == 10 + i);
    assert(a.front() == 10 && a.back() == 13);
    return 0;
}

// a[N]: past the end (precondition __n < this->size()).
int array_oob_false(int i) {
    std::array<int, 4> a{10, 11, 12, 13};
    if (i < 0 || i > 4) return 0;
    return a[static_cast<std::size_t>(i)];
}

// [span.elem]: s[i] refers to the i-th element of the viewed sequence.
int span_index_true(int i, int x) {
    int a[4] = {1, 2, 3, 4};
    std::span<int> s(a + 1, 3);
    if (i < 0 || i >= 3) return 0;
    s[static_cast<std::size_t>(i)] = x;
    assert(a[i + 1] == x);
    assert(s.size() == 3);
    return 0;
}

// s[size()]: past the view (precondition idx < size()).
int span_oob_false(int i) {
    int a[4] = {1, 2, 3, 4};
    std::span<int> s(a, 2);
    if (i < 0 || i > 2) return 0;
    return s[static_cast<std::size_t>(i)];
}

// [optional.observe]: *o is the contained value when has_value(). (value()
// on an empty optional throws bad_optional_access, whose destructor lives in
// libstdc++.so: catching it is UNENCODED, so it is not part of this harness;
// the escape of that exception is refuted in prism/cxx/cxx_optional_value.)
int optional_true(int c) {
    std::optional<int> o;
    assert(!o.has_value() && o.value_or(-1) == -1);
    if (c) o = c;
    assert(o.has_value() == (c != 0));
    if (o) assert(*o == c);
    return 0;
}

// *o on an empty optional (precondition has_value()).
int optional_empty_false(int c) {
    std::optional<int> o;
    if (c) o = 1;
    return *o;
}

// [unique.ptr.single.observers]: *p is the owned object; release() gives up
// ownership (p becomes null) and reset() deletes it.
int unique_ptr_true(int c) {
    std::unique_ptr<int> p(new int(c));
    assert(*p == c);
    int* raw = p.release();
    assert(!p);
    p.reset(raw);
    assert(p.get() == raw);
    return 0;
}

// *p on a null unique_ptr (precondition get() != nullptr).
int unique_ptr_null_false(int c) {
    std::unique_ptr<int> p(c ? new int(1) : nullptr);
    return *p;
}

// A unique_ptr's object is deleted with it: a raw pointer kept past the
// owner's lifetime dangles.
int unique_ptr_uaf_false(int c) {
    int* raw;
    {
        std::unique_ptr<int> p(new int(c));
        raw = p.get();
    }
    return *raw;
}
