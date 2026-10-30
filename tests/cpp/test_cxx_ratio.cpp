// Direct checks of the std::ratio model (src/prism/pir/models/cxx/ratio).
// Included by path, the same way tests/cpp/test_cxx_iostream.cpp includes its
// header. ratio<2,4> reduces. A zero denominator is a static_assert in the
// header text. An overflowing product is not stored as a value. ratio_equal
// is true only for the same reduced ratio. ratio_less cross-multiplies.
// ratio_not_equal is the opposite of that equality. ratio_greater is less
// with the ratios swapped. ratio_less_equal is not greater, and
// ratio_greater_equal is not less. An overflowing cross product is a
// contract check. It is not a true result and it is not a false result.

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC cl has no __builtin_trap; the model's overflow arm is never taken here.
#  include <cstdlib>
#  define __builtin_trap() std::abort()
#endif
#include "../../src/prism/pir/models/cxx/ratio"

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

static_assert(std::ratio<2, 4>::num == 1, "ratio<2,4> reduces");
static_assert(std::ratio<2, 4>::den == 2, "ratio<2,4> reduces");
static_assert(std::ratio<1, -2>::num == -1 && std::ratio<1, -2>::den == 2, "sign moves to the numerator");
static_assert(std::ratio_add<std::ratio<1, 2>, std::ratio<1, 3>>::num == 5, "1/2+1/3");
static_assert(std::ratio_add<std::ratio<1, 2>, std::ratio<1, 3>>::den == 6, "1/2+1/3");
static_assert(std::ratio_subtract<std::ratio<1, 2>, std::ratio<1, 3>>::num == 1, "1/2-1/3");
static_assert(std::ratio_subtract<std::ratio<1, 2>, std::ratio<1, 3>>::den == 6, "1/2-1/3");
static_assert(std::ratio_multiply<std::ratio<1, 2>, std::ratio<2, 3>>::num == 1, "1/2*2/3");
static_assert(std::ratio_multiply<std::ratio<1, 2>, std::ratio<2, 3>>::den == 3, "1/2*2/3");
static_assert(std::ratio_divide<std::ratio<1, 2>, std::ratio<2, 3>>::num == 3, "(1/2)/(2/3)");
static_assert(std::ratio_divide<std::ratio<1, 2>, std::ratio<2, 3>>::den == 4, "(1/2)/(2/3)");
static_assert(std::milli::num == 1 && std::milli::den == 1000, "milli is an alias");
static_assert(std::kilo::num == 1000 && std::kilo::den == 1, "kilo is an alias");
static_assert(std::ratio_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value, "2/4 equals 1/2");
static_assert(std::ratio_equal<std::ratio<1, -2>, std::ratio<-1, 2>>::value, "sign-normalized equality");
static_assert(std::ratio_equal<std::ratio<0, 1>, std::ratio<0, 5>>::value, "0/5 reduces to 0/1");
static_assert(!std::ratio_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value, "1/2 is not 1/3");
static_assert(!std::ratio_equal<std::ratio<1, -2>, std::ratio<1, 2>>::value, "negative is not the positive");
static_assert(std::ratio_less<std::ratio<1, 3>, std::ratio<1, 2>>::value, "1/3 < 1/2");
static_assert(std::ratio_less<std::ratio<-1, 2>, std::ratio<-1, 3>>::value, "-1/2 < -1/3");
static_assert(std::ratio_less<std::ratio<1, -2>, std::ratio<1, 3>>::value, "-1/2 < 1/3");
static_assert(!std::ratio_less<std::ratio<1, 2>, std::ratio<2, 4>>::value, "equal ratios are not less");
static_assert(!std::ratio_less<std::ratio<1, 2>, std::ratio<1, 3>>::value, "1/2 is not < 1/3");
static_assert(std::ratio_equal<std::ratio<INTMAX_MAX, 1>, std::ratio<INTMAX_MAX, 1>>::value,
              "a product that fits stays equal");
static_assert(std::ratio_less<std::ratio<INTMAX_MAX / 2, 2>, std::ratio<INTMAX_MAX / 2, 1>>::value,
              "wide cross product that fits");
static_assert(!std::ratio_less<std::ratio<INTMAX_MAX / 2, 1>, std::ratio<INTMAX_MAX / 2, 2>>::value,
              "wide cross product that fits, the other way");
static_assert(std::ratio_not_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value, "1/2 is unequal to 1/3");
static_assert(!std::ratio_not_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value, "the same reduced ratio is not unequal");
static_assert(!std::ratio_not_equal<std::ratio<1, -2>, std::ratio<-1, 2>>::value,
              "sign-normalized ratios are not unequal");
static_assert(std::ratio_not_equal<std::ratio<1, -2>, std::ratio<1, 2>>::value, "a negative is unequal to the positive");
static_assert(!std::ratio_not_equal<std::ratio<0, 1>, std::ratio<0, 5>>::value, "0/5 is not unequal to 0/1");
static_assert(std::ratio_greater<std::ratio<1, 2>, std::ratio<1, 3>>::value, "1/2 > 1/3");
static_assert(std::ratio_greater<std::ratio<-1, 3>, std::ratio<-1, 2>>::value, "-1/3 > -1/2");
static_assert(!std::ratio_greater<std::ratio<1, 3>, std::ratio<1, 2>>::value, "1/3 is not > 1/2");
static_assert(!std::ratio_greater<std::ratio<2, 4>, std::ratio<1, 2>>::value, "equal ratios are not greater");
static_assert(std::ratio_less_equal<std::ratio<1, 3>, std::ratio<1, 2>>::value, "1/3 <= 1/2");
static_assert(std::ratio_less_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value, "equal ratios are <=");
static_assert(std::ratio_less_equal<std::ratio<-1, 2>, std::ratio<-1, 3>>::value, "-1/2 <= -1/3");
static_assert(!std::ratio_less_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value, "1/2 is not <= 1/3");
static_assert(std::ratio_greater_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value, "1/2 >= 1/3");
static_assert(std::ratio_greater_equal<std::ratio<1, 2>, std::ratio<2, 4>>::value, "equal ratios are >=");
static_assert(std::ratio_greater_equal<std::ratio<1, -2>, std::ratio<-1, 2>>::value, "sign-normalized ratios are >=");
static_assert(!std::ratio_greater_equal<std::ratio<1, 3>, std::ratio<1, 2>>::value, "1/3 is not >= 1/2");
static_assert(std::ratio_greater<std::ratio<1, 2>, std::ratio<1, 3>>::value ==
                  std::ratio_less<std::ratio<1, 3>, std::ratio<1, 2>>::value,
              "> is < with the ratios swapped");
static_assert(std::ratio_less_equal<std::ratio<1, 3>, std::ratio<1, 2>>::value ==
                  !std::ratio_greater<std::ratio<1, 3>, std::ratio<1, 2>>::value,
              "<= is not >");
static_assert(std::ratio_greater_equal<std::ratio<1, 2>, std::ratio<1, 3>>::value ==
                  !std::ratio_less<std::ratio<1, 2>, std::ratio<1, 3>>::value,
              ">= is not <");
static_assert(std::ratio_not_equal<std::ratio<1, 2>, std::ratio<2, 4>>::value ==
                  !std::ratio_equal<std::ratio<1, 2>, std::ratio<2, 4>>::value,
              "!= is the opposite of ==");
static_assert(!std::ratio_not_equal<std::ratio<INTMAX_MAX, 1>, std::ratio<INTMAX_MAX, 1>>::value,
              "a product that fits stays not unequal");
static_assert(!std::ratio_greater<std::ratio<INTMAX_MAX, 1>, std::ratio<INTMAX_MAX, 1>>::value,
              "a product that fits is not greater than itself");
static_assert(std::ratio_less_equal<std::ratio<INTMAX_MAX, 1>, std::ratio<INTMAX_MAX, 1>>::value,
              "a product that fits stays <=");
static_assert(std::ratio_greater_equal<std::ratio<INTMAX_MAX, 1>, std::ratio<INTMAX_MAX, 1>>::value,
              "a product that fits stays >=");
static_assert(std::ratio_greater<std::ratio<INTMAX_MAX / 2, 1>, std::ratio<INTMAX_MAX / 2, 2>>::value,
              "wide cross product that fits, greater");
static_assert(!std::ratio_greater<std::ratio<INTMAX_MAX / 2, 2>, std::ratio<INTMAX_MAX / 2, 1>>::value,
              "wide cross product that fits, not greater");
static_assert(std::ratio_less_equal<std::ratio<INTMAX_MAX / 2, 2>, std::ratio<INTMAX_MAX / 2, 1>>::value,
              "wide cross product that fits, <=");
static_assert(!std::ratio_less_equal<std::ratio<INTMAX_MAX / 2, 1>, std::ratio<INTMAX_MAX / 2, 2>>::value,
              "wide cross product that fits, not <=");
static_assert(std::ratio_greater_equal<std::ratio<INTMAX_MAX / 2, 1>, std::ratio<INTMAX_MAX / 2, 2>>::value,
              "wide cross product that fits, >=");
static_assert(std::ratio_not_equal<std::ratio<INTMAX_MAX / 2, 1>, std::ratio<INTMAX_MAX / 2, 2>>::value,
              "wide cross product that fits, unequal");

TEST_CASE("cxx ratio model: ratio<2,4> reduces to 1/2") {
    CHECK(std::ratio<2, 4>::num == 1);
    CHECK(std::ratio<2, 4>::den == 2);
}

TEST_CASE("cxx ratio model: zero denominator is ill-formed and overflow is not a stored value") {
    const std::string path = std::string(PRISM_SOURCE_DIR) + "/src/prism/pir/models/cxx/ratio";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    const auto zero = text.find("struct _Prism_ratio_parts<_Num, _Den, false>");
    REQUIRE(zero != std::string::npos);
    const auto zero_end = text.find("};", zero);
    REQUIRE(zero_end != std::string::npos);
    const std::string zero_body = text.substr(zero, zero_end - zero);
    CHECK(zero_body.find("static_assert(_Den != 0, \"denominator cannot be zero\")") != std::string::npos);
    CHECK(zero_body.find("num =") == std::string::npos);
    CHECK(zero_body.find("den =") == std::string::npos);

    const auto overflow = text.find("struct _Prism_mul_checked<_A, _B, false>");
    REQUIRE(overflow != std::string::npos);
    const auto overflow_end = text.find("};", overflow);
    REQUIRE(overflow_end != std::string::npos);
    const std::string overflow_body = text.substr(overflow, overflow_end - overflow);
    CHECK(overflow_body.find("static_assert") != std::string::npos);
    CHECK(overflow_body.find("product does not fit in intmax_t") != std::string::npos);
    CHECK(overflow_body.find("value") == std::string::npos);
    CHECK(text.find("__builtin_mul_overflow") == std::string::npos);
    CHECK(text.find("__builtin_add_overflow") == std::string::npos);
    CHECK(text.find("#include <chrono>") == std::string::npos);
    CHECK(text.find("#include <compare>") == std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(text.find("using atto") == std::string::npos);
    CHECK(text.find("using femto") == std::string::npos);
    CHECK(text.find("using pico") == std::string::npos);
    CHECK(text.find("using tera") == std::string::npos);
    CHECK(text.find("using peta") == std::string::npos);
    CHECK(text.find("using exa") == std::string::npos);
    CHECK(text.find("static_assert(_Den != 0, \"denominator cannot be zero\")") != std::string::npos);
    CHECK(text.find("static_assert(_Num >= -INTMAX_MAX && _Den >= -INTMAX_MAX,") != std::string::npos);
    CHECK(text.find("absolute value is not representable by intmax_t") != std::string::npos);

    const auto trait_body = [&](const char* name) {
        const std::string key = std::string("struct ") + name + " {";
        const auto at = text.find(key);
        REQUIRE(at != std::string::npos);
        const auto end = text.find("};", at);
        REQUIRE(end != std::string::npos);
        return text.substr(at, end - at);
    };
    const std::string eq_body = trait_body("ratio_equal");
    const std::string lt_body = trait_body("ratio_less");
    const std::string ne_body = trait_body("ratio_not_equal");
    const std::string gt_body = trait_body("ratio_greater");
    const std::string le_body = trait_body("ratio_less_equal");
    const std::string ge_body = trait_body("ratio_greater_equal");
    CHECK(eq_body.find("!") == std::string::npos);
    CHECK(lt_body.find("!") == std::string::npos);
    CHECK(eq_body.find("_Prism_ratio_compare<_R1::num, _R1::den, _R2::num, _R2::den, "
                       "_Prism_ratio_rel::_S_equal>()") != std::string::npos);
    CHECK(lt_body.find("_Prism_ratio_compare<_R1::num, _R1::den, _R2::num, _R2::den, "
                       "_Prism_ratio_rel::_S_less>()") != std::string::npos);
    CHECK(ne_body.find("_Prism_ratio_compare<_R1::num, _R1::den, _R2::num, _R2::den, "
                       "_Prism_ratio_rel::_S_not_equal>()") != std::string::npos);
    CHECK(gt_body.find("_Prism_ratio_compare<_R2::num, _R2::den, _R1::num, _R1::den, "
                       "_Prism_ratio_rel::_S_less>()") != std::string::npos);
    CHECK(gt_body.find("!") == std::string::npos);
    CHECK(le_body.find("_Prism_ratio_compare<_R2::num, _R2::den, _R1::num, _R1::den, "
                       "_Prism_ratio_rel::_S_not_less>()") != std::string::npos);
    CHECK(ge_body.find("_Prism_ratio_compare<_R1::num, _R1::den, _R2::num, _R2::den, "
                       "_Prism_ratio_rel::_S_not_less>()") != std::string::npos);
    CHECK(ne_body.find("return true") == std::string::npos);
    CHECK(ne_body.find("return false") == std::string::npos);
    CHECK(gt_body.find("return true") == std::string::npos);
    CHECK(gt_body.find("return false") == std::string::npos);
    CHECK(le_body.find("return true") == std::string::npos);
    CHECK(le_body.find("return false") == std::string::npos);
    CHECK(ge_body.find("return true") == std::string::npos);
    CHECK(ge_body.find("return false") == std::string::npos);

    const auto arm = text.find("if (!_Prism_mul_fits(_N1, _D2) || !_Prism_mul_fits(_N2, _D1))");
    REQUIRE(arm != std::string::npos);
    const auto arm_end = text.find("const intmax_t _Left", arm);
    REQUIRE(arm_end != std::string::npos);
    const std::string arm_body = text.substr(arm, arm_end - arm);
    const auto check = arm_body.find("__prism_check");
    const auto trap = arm_body.find("__builtin_trap");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    CHECK(check < trap);
    CHECK(arm_body.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(arm_body.find("ratio comparison cross product does not fit in intmax_t") != std::string::npos);
    CHECK(arm_body.find("return") == std::string::npos);
    CHECK(arm_body.find("false") == std::string::npos);
    CHECK(arm_body.find("true") == std::string::npos);
    const std::string after = text.substr(arm_end);
    CHECK(after.find("return _Left == _Right") != std::string::npos);
    CHECK(after.find("return _Left < _Right") != std::string::npos);
    CHECK(after.find("return !(_Left == _Right)") != std::string::npos);
    CHECK(after.find("return !(_Left < _Right)") != std::string::npos);
    const auto neg_eq = after.find("return !(_Left == _Right)");
    const auto neg_lt = after.find("return !(_Left < _Right)");
    const auto products = after.find("const intmax_t _Left");
    REQUIRE(products != std::string::npos);
    CHECK(products < neg_eq);
    CHECK(products < neg_lt);
}
