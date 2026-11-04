// Trace of from_chars_result and to_chars_result operator!=
// (tests/cpp/test_cxx_charconv_ne.cpp). Compiled once against libstdc++ and
// once with the model header (src/prism/pir/models/cxx/charconv) first.
// operator!= is the negation of operator==. A null ptr is compared by
// address only and is not loaded. from_chars and to_chars are not called.
// No floating-point overload is called.
#include <charconv>
#include <cstdio>
#include <type_traits>

static int bit(bool v) { return v ? 1 : 0; }

static void line(const char* name, bool ne, bool eq) {
    std::printf("%s ne=%d eq=%d neg=%d\n", name, bit(ne), bit(eq), bit(ne == !eq));
}

constexpr const char* k_text = "42";
constexpr std::from_chars_result k_a{k_text, std::errc{}};
constexpr std::from_chars_result k_b{k_text, std::errc{}};
constexpr std::from_chars_result k_ptr{k_text + 1, std::errc{}};
constexpr std::from_chars_result k_ec{k_text, std::errc::result_out_of_range};
constexpr std::from_chars_result k_both{k_text + 1, std::errc::invalid_argument};
constexpr std::from_chars_result k_null{nullptr, std::errc{}};
constexpr std::from_chars_result k_null2{nullptr, std::errc{}};
constexpr std::from_chars_result k_null_ec{nullptr, std::errc::invalid_argument};

static_assert(!(k_a != k_b));
static_assert(k_a == k_b);
static_assert(!(k_a != k_a));
static_assert(k_a != k_ptr);
static_assert(k_a != k_ec);
static_assert(k_a != k_both);
static_assert(!(k_null != k_null2));
static_assert(k_null == k_null2);
static_assert(k_null != k_null_ec);
static_assert(!(k_null == k_null_ec));
static_assert(k_null != k_a);
static_assert(std::is_same_v<decltype(k_a != k_b), bool>);
static_assert(std::is_same_v<decltype(k_null != k_null2), bool>);

constexpr std::to_chars_result t_null{nullptr, std::errc{}};
constexpr std::to_chars_result t_null2{nullptr, std::errc{}};
constexpr std::to_chars_result t_null_ec{nullptr, std::errc::value_too_large};
static_assert(!(t_null != t_null2));
static_assert(t_null == t_null2);
static_assert(t_null != t_null_ec);
static_assert(!(t_null == t_null_ec));
static_assert(std::is_same_v<decltype(t_null != t_null2), bool>);

int main() {
    line("from same", k_a != k_b, k_a == k_b);
    line("from self", k_a != k_a, k_a == k_a);
    line("from ptr", k_a != k_ptr, k_a == k_ptr);
    line("from ec", k_a != k_ec, k_a == k_ec);
    line("from both", k_a != k_both, k_a == k_both);
    line("from null", k_null != k_null2, k_null == k_null2);
    line("from nullec", k_null != k_null_ec, k_null == k_null_ec);
    line("from nullp", k_null != k_a, k_null == k_a);

    char cell = 'Q';
    char* p = &cell;
    char* past = p + 1;
    const std::to_chars_result a{p, std::errc{}};
    const std::to_chars_result b{p, std::errc{}};
    const std::to_chars_result ptr{past, std::errc{}};
    const std::to_chars_result ec{p, std::errc::value_too_large};
    const std::to_chars_result both{past, std::errc::invalid_argument};
    const std::to_chars_result n{nullptr, std::errc{}};
    const std::to_chars_result n2{nullptr, std::errc{}};
    const std::to_chars_result nec{nullptr, std::errc::value_too_large};
    line("to same", a != b, a == b);
    line("to self", a != a, a == a);
    line("to ptr", a != ptr, a == ptr);
    line("to ec", a != ec, a == ec);
    line("to both", a != both, a == both);
    line("to null", n != n2, n == n2);
    line("to nullec", n != nec, n == nec);
    line("to nullp", n != a, n == a);
    std::printf("to cell=%c\n", cell);
    return 0;
}
