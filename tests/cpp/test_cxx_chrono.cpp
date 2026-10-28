// The <chrono> model is a file (src/prism/pir/models/cxx/chrono). Read it as
// text. Addition and subtraction that overflow the signed 64-bit count call
// __prism_check. The header does not return a wrapping `a + b` as the sum
// without that check, and the overflow path does not hand back a negative
// wrapped count. Same-type operator+= and operator-= use that check and
// store 0. They do not store a wrapped negative. Same-period duration_cast
// returns that count. A different period is not declared. Same-type
// operator==, operator!=, operator<, operator>, operator<=, and operator>=
// compare the stored counts. They do not convert a period, do not add, and
// do not sleep. A different period is not an overload. duration::zero() is
// a count of 0. duration::min() is the most negative count and
// duration::max() is the most positive count. They do not read another
// duration. A same-rep scalar product that does not fit is __prism_check
// then __builtin_trap. That path does not return a wrapped count and does
// not return a zero duration. Zero times any count is zero. Unary
// operator- returns a duration of the same type. Negating the most
// negative signed 64-bit count is __prism_check then __builtin_trap. That
// path does not return a zero duration and does not form the negation.
// Every other count returns duration(-count). Zero stays zero. operator/
// divides a duration by a scalar of the same representation. A zero
// divisor is __prism_check then __builtin_trap, before any quotient is
// formed. That path does not return a zero duration. Dividing the most
// negative signed 64-bit count by -1 is __prism_check then __builtin_trap
// and does not form the negation. Every other quotient that fits is the
// stored count divided by the scalar. Sleep stays absent. This is not
// PROVED and not CLEAN. This case does not compile the header.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string slice_between(const std::string& text, const std::string& begin, const std::string& end) {
    const auto i = text.find(begin);
    REQUIRE(i != std::string::npos);
    const auto j = text.find(end, i + begin.size());
    REQUIRE(j != std::string::npos);
    return text.substr(i, j - i);
}

int count_of(const std::string& body, const std::string& needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// A wrapping sum is an arithmetic return that sits ahead of __prism_check,
// or the tokens `return a + b` / `return a - b`. The overflow path returns
// a zero duration after the check, not the wrapped count.
// Compound assignment stores 0 after the same check. The binary-operator
// slice begins at "operator+(const duration".
void expect_checked(const std::string& body, const std::string& sum_return) {
    CHECK(body.find("return a + b") == std::string::npos);
    CHECK(body.find("return a - b") == std::string::npos);
    CHECK(body.find("return __a + __b") == std::string::npos);
    CHECK(body.find("return __a - __b") == std::string::npos);
    const auto check = body.find("__prism_check");
    REQUIRE(check != std::string::npos);
    const auto fail = body.find("if (!__ok) return duration<_Rep, _Period>(0);");
    REQUIRE(fail != std::string::npos);
    CHECK(fail > check);
    const auto okret = body.find(sum_return);
    REQUIRE(okret != std::string::npos);
    CHECK(okret > check);
    CHECK(okret > fail);
    std::size_t pos = 0;
    while ((pos = body.find("return", pos)) != std::string::npos) {
        const auto end = body.find(';', pos);
        REQUIRE(end != std::string::npos);
        const std::string expr = body.substr(pos, end - pos);
        const bool arithmetic = expr.find('+') != std::string::npos || expr.find(" - ") != std::string::npos;
        if (arithmetic) CHECK(pos > check);
        pos = end + 1;
    }
}

}  // namespace

TEST_CASE("cxx chrono: overflow calls __prism_check and does not return a wrapped sum") {
    const auto path = std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "chrono";
    REQUIRE(std::filesystem::is_regular_file(path));
    CHECK_FALSE(std::filesystem::is_directory(path));
    const std::string text = slurp(path);
    REQUIRE_FALSE(text.empty());

    CHECK(text.find("#include <ratio>") == std::string::npos);
    CHECK(text.find("_GLIBCXX_CHRONO") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std::chrono") != std::string::npos);
    CHECK(text.find("return _M_count") != std::string::npos);
    CHECK(text.find("_M_count = 0") != std::string::npos);
    CHECK(text.find("return a + b") == std::string::npos);
    CHECK(text.find("return a - b") == std::string::npos);
    CHECK(text.find("sleep_for(") == std::string::npos);
    CHECK(text.find("sleep_until(") == std::string::npos);
    // coverage session 5 declares time_point and the clocks (no OS reads)
    CHECK(text.find("class time_point") != std::string::npos);
    CHECK(text.find("system_clock") != std::string::npos);

    // The binary operators are sliced at "(const duration" because
    // operator+= also contains the text operator+. The overflow line stays
    // the return of a zero duration after __prism_check.
    const std::string add = slice_between(text, "operator+(const duration", "operator-(const duration");
    const std::string sub = slice_between(text, "operator-(const duration",
                                          "duration<_Rep, _Period>::operator+=");
    expect_checked(add, "return duration<_Rep, _Period>(static_cast<_Rep>(__x + __y));");
    expect_checked(sub, "return duration<_Rep, _Period>(static_cast<_Rep>(__x - __y));");
    CHECK(add.find("INT-SIGNED-OVF") != std::string::npos);
    CHECK(sub.find("INT-SIGNED-OVF") != std::string::npos);
    CHECK(add.find("if (!__ok) return duration<_Rep, _Period>(0);") != std::string::npos);
    CHECK(sub.find("if (!__ok) return duration<_Rep, _Period>(0);") != std::string::npos);

    CHECK(text.find("duration& operator+=(const duration& __d);") != std::string::npos);
    CHECK(text.find("duration& operator-=(const duration& __d);") != std::string::npos);
    const std::string add_eq =
        slice_between(text, "duration<_Rep, _Period>::operator+=", "duration<_Rep, _Period>::operator-=");
    const std::string sub_eq = slice_between(text, "duration<_Rep, _Period>::operator-=",
                                             "Count comparison. Same duration type only.");
    auto expect_stored = [](const std::string& body, const std::string& fits, const std::string& store) {
        const auto check = body.find("__prism_check");
        REQUIRE(check != std::string::npos);
        CHECK(body.find(fits) != std::string::npos);
        CHECK(body.find("INT-SIGNED-OVF") != std::string::npos);
        const auto zero = body.find("_M_count = static_cast<_Rep>(0);");
        REQUIRE(zero != std::string::npos);
        CHECK(zero > check);
        const auto kept = body.find(store);
        REQUIRE(kept != std::string::npos);
        CHECK(kept > zero);
        CHECK(body.find("if (!__ok) return duration<_Rep, _Period>(0);") == std::string::npos);
        CHECK(body.find("_Rep2") == std::string::npos);
        CHECK(body.find("_Period2") == std::string::npos);
        CHECK(body.find("return duration<_Rep, _Period>(static_cast<_Rep>(__x + __y))") == std::string::npos);
        CHECK(body.find("return duration<_Rep, _Period>(static_cast<_Rep>(__x - __y))") == std::string::npos);
    };
    expect_stored(add_eq, "__duration_add_fits", "_M_count = static_cast<_Rep>(__x + __y);");
    expect_stored(sub_eq, "__duration_sub_fits", "_M_count = static_cast<_Rep>(__x - __y);");
    CHECK(text.find("A different period is not declared, so that") != std::string::npos);

    // Same-period cast exists. Sleep stays absent (checked above). The cast
    // copies the count. It does not divide, wrap, or static_cast.
    const auto cast_at = text.find("template <typename _ToDur, typename _Rep>");
    REQUIRE(cast_at != std::string::npos);
    const auto cast_end = text.find("\n// True when __a + __b", cast_at);
    REQUIRE(cast_end != std::string::npos);
    const std::string cast = text.substr(cast_at, cast_end - cast_at);
    CHECK(cast.find("const duration<_Rep, __duration_period>& __d") != std::string::npos);
    CHECK(cast.find("__duration_same_type<typename _ToDur::period, __duration_period>") != std::string::npos);
    CHECK(cast.find("__duration_same_type<typename _ToDur::rep, _Rep>") != std::string::npos);
    CHECK(cast.find("return _ToDur(__d.count());") != std::string::npos);
    CHECK(cast.find("static_cast") == std::string::npos);
    CHECK(cast.find('/') == std::string::npos);
    CHECK(cast.find('%') == std::string::npos);
    CHECK(cast.find('+') == std::string::npos);
    CHECK(cast.find(" - ") == std::string::npos);
    CHECK(text.find("inline constexpr bool __duration_same_type<_A, _A> = true;") != std::string::npos);
    CHECK(text.find("A cast to a different period") != std::string::npos);
    CHECK(text.find("still falls back") != std::string::npos);
    CHECK(text.find("sleep_for(") == std::string::npos);
    CHECK(text.find("sleep_until(") == std::string::npos);

    // Same-type comparisons read the stored counts. They do not add, do not
    // convert a period, and do not sleep. A different period is not an
    // overload. The result is not PROVED and not CLEAN.
    CHECK(count_of(text, "if (!__ok) return duration<_Rep, _Period>(0);") == 2);
    CHECK(text.find("#include <compare>") == std::string::npos);
    CHECK(text.find("operator<=>") == std::string::npos);
    CHECK(count_of(text, "This is not PROVED and not CLEAN.") == 9);  // + the clocks
    CHECK(text.find("A different period is not an overload.") != std::string::npos);
    const auto note = text.find("Count comparison. Same duration type only.");
    const auto ops = text.find("constexpr bool operator==(const duration<_Rep, _Period>& __a,");
    REQUIRE(note != std::string::npos);
    REQUIRE(ops != std::string::npos);
    CHECK(note < ops);
    const auto tmpl = text.rfind("template <typename _Rep, typename _Period>", ops);
    REQUIRE(tmpl != std::string::npos);
    // operator* follows the six comparisons. The slice stops before it so
    // the product check is not treated as a comparison.
    const auto cmp_end = text.find("// Duration multiplied by a scalar of the same representation.", ops);
    REQUIRE(cmp_end != std::string::npos);
    const std::string cmp = text.substr(tmpl, cmp_end - tmpl);
    CHECK(count_of(cmp, "template <typename _Rep, typename _Period>") == 6);
    CHECK(count_of(cmp, "constexpr bool operator") == 6);
    CHECK(count_of(cmp, "const duration<_Rep, _Period>& __a,") == 6);
    CHECK(count_of(cmp, "const duration<_Rep, _Period>& __b) noexcept") == 6);
    CHECK(cmp.find("return __a.count() == __b.count();") != std::string::npos);
    CHECK(cmp.find("return __a.count() != __b.count();") != std::string::npos);
    CHECK(cmp.find("return __a.count() < __b.count();") != std::string::npos);
    CHECK(cmp.find("return __a.count() > __b.count();") != std::string::npos);
    CHECK(cmp.find("return __a.count() <= __b.count();") != std::string::npos);
    CHECK(cmp.find("return __a.count() >= __b.count();") != std::string::npos);
    CHECK(cmp.find("constexpr bool operator==(const duration<_Rep, _Period>& __a,") != std::string::npos);
    CHECK(cmp.find("constexpr bool operator!=(const duration<_Rep, _Period>& __a,") != std::string::npos);
    CHECK(cmp.find("constexpr bool operator<(const duration<_Rep, _Period>& __a,") != std::string::npos);
    CHECK(cmp.find("constexpr bool operator>(const duration<_Rep, _Period>& __a,") != std::string::npos);
    CHECK(cmp.find("constexpr bool operator<=(const duration<_Rep, _Period>& __a,") != std::string::npos);
    CHECK(cmp.find("constexpr bool operator>=(const duration<_Rep, _Period>& __a,") != std::string::npos);
    CHECK(cmp.find("_Rep2") == std::string::npos);
    CHECK(cmp.find("_Period2") == std::string::npos);
    CHECK(cmp.find("common_type") == std::string::npos);
    CHECK(cmp.find("__prism_check") == std::string::npos);
    CHECK(cmp.find("__duration_add_fits") == std::string::npos);
    CHECK(cmp.find("__duration_sub_fits") == std::string::npos);
    CHECK(cmp.find("duration_cast") == std::string::npos);
    CHECK(cmp.find("static_cast") == std::string::npos);
    CHECK(cmp.find("operator+") == std::string::npos);
    CHECK(cmp.find("operator-") == std::string::npos);
    CHECK(cmp.find('#') == std::string::npos);
    CHECK(cmp.find('+') == std::string::npos);
    CHECK(cmp.find(" - ") == std::string::npos);
    CHECK(cmp.find('/') == std::string::npos);
    CHECK(cmp.find("sleep_for(") == std::string::npos);
    CHECK(cmp.find("sleep_until(") == std::string::npos);
    CHECK(cmp.find("__duration_mul") == std::string::npos);
    CHECK(cmp.find("__builtin_trap") == std::string::npos);

    // zero, min, and max are static. They do not read a duration, do not
    // add, and do not sleep. min and max are the representation limits.
    const auto zfn = text.find(
        "static constexpr duration zero() noexcept { return duration(static_cast<_Rep>(0)); }");
    const auto minfn = text.find(
        "static constexpr duration min() noexcept { return duration(static_cast<_Rep>(__duration_rep_min)); }");
    const auto maxfn = text.find(
        "static constexpr duration max() noexcept { return duration(static_cast<_Rep>(__duration_rep_max)); }");
    REQUIRE(zfn != std::string::npos);
    REQUIRE(minfn != std::string::npos);
    REQUIRE(maxfn != std::string::npos);
    CHECK(zfn < ops);
    CHECK(minfn < ops);
    CHECK(maxfn < ops);
    CHECK(text.find("static constexpr duration min() noexcept { return duration(static_cast<_Rep>(0)); }") ==
          std::string::npos);
    CHECK(text.find("static constexpr duration max() noexcept { return duration(static_cast<_Rep>(0)); }") ==
          std::string::npos);
    CHECK(text.find("inline constexpr long long __duration_rep_max = 9223372036854775807LL;") != std::string::npos);
    CHECK(text.find("inline constexpr long long __duration_rep_min = -9223372036854775807LL - 1;") !=
          std::string::npos);
    const std::string limits =
        slice_between(text, "static constexpr duration zero()", "duration& operator+=(const duration& __d);");
    CHECK(count_of(limits, "static constexpr duration ") == 3);
    CHECK(limits.find("count()") == std::string::npos);
    CHECK(limits.find("__prism_check") == std::string::npos);
    CHECK(limits.find("__builtin_trap") == std::string::npos);
    CHECK(limits.find("sleep_for(") == std::string::npos);
    CHECK(limits.find("sleep_until(") == std::string::npos);
    CHECK(limits.find("operator+") == std::string::npos);
    CHECK(limits.find('+') == std::string::npos);
    CHECK(limits.find(" - ") == std::string::npos);
    CHECK(limits.find("const duration") == std::string::npos);

    // Same-rep multiply. Zero returns before the check. A product that does
    // not fit traps and does not return a zero duration or a wrapped count.
    const auto mul_note = text.find("// Duration multiplied by a scalar of the same representation.");
    REQUIRE(mul_note != std::string::npos);
    CHECK(mul_note > ops);
    const auto mul_end = text.find("// Unary operator-. Same duration type.", mul_note);
    REQUIRE(mul_end != std::string::npos);
    const std::string mul = text.substr(mul_note, mul_end - mul_note);
    CHECK(mul.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(mul.find("if (!__ok) return duration<_Rep, _Period>(0);") == std::string::npos);
    CHECK(mul.find("__x * __y") == std::string::npos);
    CHECK(mul.find("__a * __b") == std::string::npos);
    CHECK(mul.find("operator*=(") == std::string::npos);
    CHECK(mul.find("_Period2") == std::string::npos);
    CHECK(mul.find("_Rep2") == std::string::npos);
    CHECK(mul.find("sleep_for(") == std::string::npos);
    CHECK(mul.find("sleep_until(") == std::string::npos);
    CHECK(mul.find("#include") == std::string::npos);
    CHECK(count_of(mul, "requires __duration_same_type<_Scalar, _Rep>") == 2);
    CHECK(count_of(mul, "return __duration_mul(__d, __s);") == 2);
    CHECK(mul.find("operator*(const duration<_Rep, _Period>& __d, const _Scalar& __s)") != std::string::npos);
    CHECK(mul.find("operator*(const _Scalar& __s, const duration<_Rep, _Period>& __d)") != std::string::npos);
    CHECK(mul.find("constexpr duration<_Rep, _Period> operator*") == std::string::npos);
    const std::string mul_fn = slice_between(
        text, "duration<_Rep, _Period> __duration_mul(const duration<_Rep, _Period>& __d, const _Rep& __s)",
        "template <typename _Rep, typename _Period, typename _Scalar>");
    const auto zero_ret = mul_fn.find("return duration<_Rep, _Period>(static_cast<_Rep>(0));");
    const auto check = mul_fn.find("__prism_check(__ok, \"INT-SIGNED-OVF\",");
    const auto msg = mul_fn.find("std::chrono::duration multiplication overflows the representation");
    const auto trap = mul_fn.find("__builtin_trap();");
    const auto prod =
        mul_fn.find("return duration<_Rep, _Period>(static_cast<_Rep>(__duration_product(__x, __y)));");
    REQUIRE(zero_ret != std::string::npos);
    REQUIRE(check != std::string::npos);
    REQUIRE(msg != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(prod != std::string::npos);
    CHECK(zero_ret < check);
    CHECK(check < msg);
    CHECK(msg < trap);
    CHECK(trap < prod);
    const auto bad = mul_fn.find("if (!__ok) {");
    const auto else_at = mul_fn.find("} else {", bad);
    REQUIRE(bad != std::string::npos);
    REQUIRE(else_at != std::string::npos);
    const std::string arm = mul_fn.substr(bad, else_at - bad);
    CHECK(arm.find("__builtin_trap();") != std::string::npos);
    CHECK(arm.find("return") == std::string::npos);
    CHECK(arm.find("duration(0)") == std::string::npos);
    CHECK(arm.find("__duration_product") == std::string::npos);
    const std::string kept = mul_fn.substr(else_at);
    CHECK(kept.find("__duration_product(__x, __y)") != std::string::npos);
    CHECK(kept.find("__builtin_trap") == std::string::npos);
    CHECK(text.find("#include <ratio>") == std::string::npos);
    CHECK(text.find("#include <compare>") == std::string::npos);
    CHECK(text.find("operator*=(") == std::string::npos);
    CHECK(count_of(text, "if (!__ok) return duration<_Rep, _Period>(0);") == 2);

    // Unary negation. The minimum count traps and does not form the
    // negation. Every other count is duration(-count). Zero stays zero.
    // The locked zero return stays on operator+ and binary operator- only.
    const auto neg_note = text.find("// Unary operator-. Same duration type.");
    REQUIRE(neg_note != std::string::npos);
    CHECK(neg_note == mul_end);
    CHECK(neg_note > mul_note);
    const auto neg_end = text.find("// Duration divided by a scalar of the same representation.", neg_note);
    REQUIRE(neg_end != std::string::npos);
    const std::string neg = text.substr(neg_note, neg_end - neg_note);
    CHECK(neg.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(neg.find("if (!__ok) return duration<_Rep, _Period>(0);") == std::string::npos);
    CHECK(neg.find("return duration<_Rep, _Period>(0)") == std::string::npos);
    CHECK(neg.find("operator*=(") == std::string::npos);
    CHECK(neg.find("_Period2") == std::string::npos);
    CHECK(neg.find("_Rep2") == std::string::npos);
    CHECK(neg.find("sleep_for(") == std::string::npos);
    CHECK(neg.find("sleep_until(") == std::string::npos);
    CHECK(neg.find("#include") == std::string::npos);
    CHECK(neg.find("operator-(const duration<_Rep, _Period>& __a,") == std::string::npos);
    CHECK(count_of(neg, "operator-(const duration<_Rep, _Period>& __d)") == 1);
    const std::string neg_fn = slice_between(
        text, "duration<_Rep, _Period> operator-(const duration<_Rep, _Period>& __d)",
        "// Duration divided by a scalar of the same representation.");
    const auto neg_check = neg_fn.find("__prism_check(__ok, \"INT-SIGNED-OVF\",");
    const auto neg_msg = neg_fn.find("std::chrono::duration negation overflows the representation");
    const auto neg_trap = neg_fn.find("__builtin_trap();");
    const auto neg_ret = neg_fn.find("return duration<_Rep, _Period>(static_cast<_Rep>(-__x));");
    REQUIRE(neg_check != std::string::npos);
    REQUIRE(neg_msg != std::string::npos);
    REQUIRE(neg_trap != std::string::npos);
    REQUIRE(neg_ret != std::string::npos);
    CHECK(neg_check < neg_msg);
    CHECK(neg_msg < neg_trap);
    CHECK(neg_trap < neg_ret);
    CHECK(neg_fn.find("__duration_rep_min") != std::string::npos);
    CHECK(neg_fn.find("__x * ") == std::string::npos);
    CHECK(neg_fn.find("__duration_add_fits") == std::string::npos);
    CHECK(neg_fn.find("__duration_sub_fits") == std::string::npos);
    CHECK(neg_fn.find("__duration_mul") == std::string::npos);
    const auto neg_bad = neg_fn.find("if (!__ok) {");
    const auto neg_else = neg_fn.find("} else {", neg_bad);
    REQUIRE(neg_bad != std::string::npos);
    REQUIRE(neg_else != std::string::npos);
    const std::string neg_arm = neg_fn.substr(neg_bad, neg_else - neg_bad);
    CHECK(neg_arm.find("__builtin_trap();") != std::string::npos);
    CHECK(neg_arm.find("return") == std::string::npos);
    CHECK(neg_arm.find("duration(0)") == std::string::npos);
    CHECK(neg_arm.find("-") == std::string::npos);
    const std::string neg_kept = neg_fn.substr(neg_else);
    CHECK(neg_kept.find("return duration<_Rep, _Period>(static_cast<_Rep>(-__x));") != std::string::npos);
    CHECK(neg_kept.find("__builtin_trap") == std::string::npos);
    CHECK(count_of(text, "if (!__ok) return duration<_Rep, _Period>(0);") == 2);
    CHECK(count_of(text, "std::chrono::duration negation overflows the representation") == 1);
    CHECK(neg.find("operator/(const duration") == std::string::npos);
    CHECK(neg.find("__x / __y") == std::string::npos);

    // Division by a same-rep scalar. A zero divisor traps before a
    // quotient. The minimum count divided by -1 traps and does not form
    // the negation. Every other quotient is the stored count divided by
    // the scalar. The locked zero return stays on operator+ and binary
    // operator- only.
    const auto div_note = text.find("// Duration divided by a scalar of the same representation.");
    REQUIRE(div_note != std::string::npos);
    CHECK(div_note == neg_end);
    CHECK(div_note > neg_note);
    const auto div_end = text.find("}  // namespace std::chrono", div_note);
    REQUIRE(div_end != std::string::npos);
    const std::string div = text.substr(div_note, div_end - div_note);
    CHECK(div.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(div.find("if (!__ok) return duration<_Rep, _Period>(0);") == std::string::npos);
    CHECK(div.find("return duration<_Rep, _Period>(0)") == std::string::npos);
    CHECK(div.find("operator*=(") == std::string::npos);
    CHECK(div.find("operator/=(") == std::string::npos);
    CHECK(div.find("_Period2") == std::string::npos);
    CHECK(div.find("_Rep2") == std::string::npos);
    CHECK(div.find("sleep_for(") == std::string::npos);
    CHECK(div.find("sleep_until(") == std::string::npos);
    CHECK(div.find("#include") == std::string::npos);
    CHECK(div.find("common_type") == std::string::npos);
    CHECK(div.find("operator-(const duration<_Rep, _Period>& __d)") == std::string::npos);
    CHECK(div.find("operator+(const duration") == std::string::npos);
    CHECK(count_of(div, "requires __duration_same_type<_Scalar, _Rep>") == 1);
    CHECK(count_of(div, "operator/(const duration<_Rep, _Period>& __d, const _Scalar& __s)") == 1);
    const std::string div_fn = slice_between(
        text, "duration<_Rep, _Period> operator/(const duration<_Rep, _Period>& __d, const _Scalar& __s)",
        "}  // namespace std::chrono");
    const auto div_zcheck = div_fn.find("__prism_check(__nz, \"FUNC-CONTRACT\",");
    const auto div_zmsg = div_fn.find("std::chrono::duration division by zero");
    const auto div_ztrap = div_fn.find("__builtin_trap();");
    const auto div_ocheck = div_fn.find("__prism_check(__ok, \"INT-SIGNED-OVF\",");
    const auto div_omsg = div_fn.find("std::chrono::duration division overflows the representation");
    const auto div_otrap = div_fn.find("__builtin_trap();", div_ztrap + 1);
    const auto div_ret = div_fn.find("return duration<_Rep, _Period>(static_cast<_Rep>(__x / __y));");
    REQUIRE(div_zcheck != std::string::npos);
    REQUIRE(div_zmsg != std::string::npos);
    REQUIRE(div_ztrap != std::string::npos);
    REQUIRE(div_ocheck != std::string::npos);
    REQUIRE(div_omsg != std::string::npos);
    REQUIRE(div_otrap != std::string::npos);
    REQUIRE(div_ret != std::string::npos);
    CHECK(div_zcheck < div_zmsg);
    CHECK(div_zmsg < div_ztrap);
    CHECK(div_ztrap < div_ocheck);
    CHECK(div_ocheck < div_omsg);
    CHECK(div_omsg < div_otrap);
    CHECK(div_otrap < div_ret);
    CHECK(div_fn.find("__duration_rep_min") != std::string::npos);
    CHECK(div_fn.find("__y == -1") != std::string::npos);
    CHECK(div_fn.find("-__x") == std::string::npos);
    CHECK(div_fn.find("__duration_mul") == std::string::npos);
    CHECK(div_fn.find("__duration_add_fits") == std::string::npos);
    CHECK(div_fn.find("__duration_sub_fits") == std::string::npos);
    CHECK(count_of(div_fn, "__x / __y") == 1);
    const auto div_zbad = div_fn.find("if (!__nz) {");
    const auto div_zelse = div_fn.find("} else {", div_zbad);
    REQUIRE(div_zbad != std::string::npos);
    REQUIRE(div_zelse != std::string::npos);
    const std::string div_zarm = div_fn.substr(div_zbad, div_zelse - div_zbad);
    CHECK(div_zarm.find("__builtin_trap();") != std::string::npos);
    CHECK(div_zarm.find("return") == std::string::npos);
    CHECK(div_zarm.find("duration(0)") == std::string::npos);
    CHECK(div_zarm.find("/") == std::string::npos);
    CHECK(div_zarm.find("__x / __y") == std::string::npos);
    const auto div_obad = div_fn.find("if (!__ok) {", div_zelse);
    const auto div_oelse = div_fn.find("} else {", div_obad);
    REQUIRE(div_obad != std::string::npos);
    REQUIRE(div_oelse != std::string::npos);
    CHECK(div_obad > div_ztrap);
    const std::string div_oarm = div_fn.substr(div_obad, div_oelse - div_obad);
    CHECK(div_oarm.find("__builtin_trap();") != std::string::npos);
    CHECK(div_oarm.find("return") == std::string::npos);
    CHECK(div_oarm.find("duration(0)") == std::string::npos);
    CHECK(div_oarm.find("-") == std::string::npos);
    CHECK(div_oarm.find("/") == std::string::npos);
    CHECK(div_oarm.find("__x / __y") == std::string::npos);
    const std::string div_kept = div_fn.substr(div_oelse);
    CHECK(div_kept.find("return duration<_Rep, _Period>(static_cast<_Rep>(__x / __y));") != std::string::npos);
    CHECK(div_kept.find("__builtin_trap") == std::string::npos);
    const std::string div_before = div_fn.substr(0, div_ret);
    CHECK(div_before.find("__x / __y") == std::string::npos);
    CHECK(count_of(text, "if (!__ok) return duration<_Rep, _Period>(0);") == 2);
    CHECK(count_of(text, "std::chrono::duration division by zero") == 1);
    CHECK(count_of(text, "std::chrono::duration division overflows the representation") == 1);
    CHECK(text.find("operator*=(") == std::string::npos);
    CHECK(text.find("operator/=(") == std::string::npos);
}
