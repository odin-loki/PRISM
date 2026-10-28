// The <chrono> model is a file (src/prism/pir/models/cxx/chrono). Read it as
// text. Addition and subtraction that overflow the signed 64-bit count call
// __prism_check. The header does not return a wrapping `a + b` as the sum
// without that check, and the overflow path does not hand back a negative
// wrapped count. Same-type operator+= and operator-= use that check and
// store 0. They do not store a wrapped negative. Same-period duration_cast
// returns that count. A different period is not declared. Sleep stays
// absent. This case does not compile the header.

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
    CHECK(text.find("class time_point") == std::string::npos);
    CHECK(text.find("system_clock") == std::string::npos);

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
    const std::string sub_eq =
        slice_between(text, "duration<_Rep, _Period>::operator-=", "}  // namespace std::chrono");
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
}
