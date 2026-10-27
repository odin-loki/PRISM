// std::from_chars integer model (src/prism/pir/models/cxx/charconv). The pir
// stage puts the model headers first on the include path, so <charconv> below
// is the model. Each function is one contract; `_false` ones must fail for
// the class their yml names.
#include <charconv>

// A small integer is stored and the result is success.
int charconv_small_true(int) {
    const char* s = "42";
    int v = 0;
    auto r = std::from_chars(s, s + 2, v);
    if (r.ec != std::errc{} || r.ptr != s + 2 || v != 42) return 1;
    return 0;
}

// A number that does not fit in int is not a success value. Adding one to the
// unchanged INT_MAX overflows.
int charconv_overflow_false(int) {
    const char* s = "2147483648";
    int v = 2147483647;
    std::from_chars(s, s + 10, v);
    return v + 1;
}

// A null range is a precondition check, not a successful zero.
int charconv_null_false(int) {
    int v = 7;
    auto r = std::from_chars(nullptr, nullptr, v);
    return r.ec == std::errc{} ? v : 1;
}
