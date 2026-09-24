// std::string against C++23 [basic.string]. std::string is libstdc++'s own
// code (no PRISM model: <string> is part of every iostream/exception header,
// so it cannot be swapped for a model; docs/PIR.md "C++ library models"),
// checked with -D_GLIBCXX_ASSERTIONS. Each function is one contract;
// `_false` ones must fail for their class.
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string>

// [string.access]: s[i] for i < size() is the i-th character, s[size()] is
// the terminator.
int string_index_true(int i) {
    std::string s("abc");
    if (i < 0 || i > 3) return 0;
    assert(s[static_cast<std::size_t>(i)] == (i < 3 ? 'a' + i : 0));
    return 0;
}

// s[size() + 1]: past the terminator (precondition __pos <= size()).
int string_index_oob_false(int i) {
    std::string s("abc");
    if (i < 0 || i > 4) return 0;
    return s[static_cast<std::size_t>(i)];
}

// [string.access]: at(n) throws out_of_range iff n >= size().
int string_at_true(int i) {
    std::string s("ab");
    if (i < 0 || i > 3) return 0;
    bool threw = false;
    try {
        (void)s.at(static_cast<std::size_t>(i));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw == (i >= 2));
    return 0;
}

// c_str() of a string that has been destroyed (heap buffer released).
int string_cstr_uaf_false(int c) {
    const char* p;
    {
        std::string s("a string longer than the small buffer");
        p = s.c_str();
    }
    return p[c & 3];
}
