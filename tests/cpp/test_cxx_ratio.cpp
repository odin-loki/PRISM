// Direct checks of the std::ratio model (src/prism/pir/models/cxx/ratio).
// Included by path, the same way tests/cpp/test_cxx_iostream.cpp includes its
// header. ratio<2,4> reduces. A zero denominator is a static_assert in the
// header text. An overflowing product is not stored as a value.

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
}
