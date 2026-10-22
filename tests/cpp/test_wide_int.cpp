// Portable 128-bit integers (include/prism/wide_int.hpp).

#include <doctest/doctest.h>

#include "prism/wide_int.hpp"

#include <limits>

using prism::format_i128;
using prism::i128;
using prism::i128_add;
using prism::i128_eq;
using prism::i128_from_i64;
using prism::i128_max;
using prism::i128_min;
using prism::i128_mul;
using prism::i128_neg;
using prism::i128_sub;
using prism::i128_to_i64;
using prism::parse_i128;
using prism::u128;
using prism::u128_mask;

namespace {

void require_parse(const char* text, i128 want) {
    auto got = parse_i128(text);
    REQUIRE(got.has_value());
    CHECK(i128_eq(*got, want));
}

}  // namespace

TEST_CASE("wide_int format decimal") {
    CHECK(format_i128(i128(0)) == "0");
    CHECK(format_i128(i128(42)) == "42");
    CHECK(format_i128(i128(-1)) == "-1");
    CHECK(format_i128(*parse_i128("-9223372036854775808")) == "-9223372036854775808");
    CHECK(format_i128(*parse_i128("18446744073709551615")) == "18446744073709551615");
    CHECK(format_i128(*parse_i128("170141183460469231731687303715884105727")) ==
          "170141183460469231731687303715884105727");
    CHECK(format_i128(*parse_i128("-170141183460469231731687303715884105728")) ==
          "-170141183460469231731687303715884105728");
}

TEST_CASE("wide_int parse int(text,0)") {
    require_parse("0", i128(0));
    require_parse("-0", i128(0));
    require_parse("0x_ff", i128(255));
    require_parse("0o10", i128(8));
    require_parse("0b1010", i128(10));
    CHECK(!parse_i128("010"));
    CHECK(!parse_i128(""));
    CHECK(!parse_i128("+"));
    require_parse("-42", i128(-42));
}

TEST_CASE("wide_int parse format roundtrip") {
    for (const char* s :
         {"0", "1", "-1", "255", "18446744073709551615", "-9223372036854775808", "0x7fffffffffffffff",
          "-0x8000000000000000"}) {
        auto v = parse_i128(s);
        REQUIRE(v.has_value());
        auto again = parse_i128(format_i128(*v));
        REQUIRE(again.has_value());
        CHECK(i128_eq(*again, *v));
    }
}

TEST_CASE("wide_int arithmetic smoke") {
    CHECK(i128_eq(i128_add(i128(100), i128(23)), i128(123)));
    CHECK(i128_eq(i128_neg(i128(5)), i128(-5)));
    CHECK(i128_eq(i128_mul(i128(1000), i128(1000)), i128(1000000)));
    CHECK(i128_eq(i128_sub(i128(10), i128(3)), i128(7)));
    CHECK(i128_eq(i128_max(i128(1), i128(2)), i128(2)));
    CHECK(i128_eq(i128_min(i128(1), i128(2)), i128(1)));
    CHECK(i128_to_i64(i128(-42)) == -42);
}

TEST_CASE("wide_int u128 bitwise smoke") {
    CHECK((u128(3) | u128(5)) == u128(7));
    CHECK((u128(0xff) & u128(0xf0)) == u128(0xf0));
    CHECK(u128_mask(8) == u128(0xff));
    CHECK((u128(1) << 64) != u128(0));
#if !PRISM_HAS_INT128
    CHECK((u128(1) << 64) == u128(1, 0));
    CHECK((u128(1, 0) >> 64) == u128(1));
#endif
    CHECK(u128(1000) / u128(10) == u128(100));
}
