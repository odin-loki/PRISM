// Direct checks of the <complex> stand-in. Included by path so prism_tests
// does not put the model directory on the include path for every TU.
// Division by complex(0, 0) is __prism_check then __builtin_trap. That arm
// does not return *this. A scalar zero builds complex(y, 0) and hits the
// same arm. real() and imag() of a constructed value are the values that
// were passed in. No heap allocation.
// abs of (0, 0) is 0. A square that does not fit is __prism_check then
// __builtin_trap, and that path does not return a magnitude.
// norm of (0, 0) is 0. A square that does not fit is the same kind of
// check and trap, and that path does not return a finite sum. A square
// that fits returns the sum of the squares.

#include <doctest/doctest.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/complex"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace fs = std::filesystem;

namespace {

struct PrismCheck {
    int n = 0;
    int ok = 1;
    const char* cls = "";
    const char* msg = "";
};

PrismCheck g_check;

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of the function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

int count_of(std::string_view hay, std::string_view needle) {
    int n = 0;
    for (std::size_t at = 0; (at = hay.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

// Brace block that opens at or after `at`, including its braces.
std::string brace_after(std::string_view src, std::size_t at) {
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(open, i - open + 1));
        }
    }
    return {};
}

}  // namespace

extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    ++g_check.n;
    g_check.ok = ok;
    g_check.cls = cls ? cls : "";
    g_check.msg = msg ? msg : "";
}

TEST_CASE("cxx complex model: real and imag are the constructed values") {
    const std::complex<double> z(1.5, -2.25);
    CHECK(z.real() == 1.5);
    CHECK(z.imag() == -2.25);

    const std::complex<double> only_real(8.0);
    CHECK(only_real.real() == 8.0);
    CHECK(only_real.imag() == 0.0);

    const std::complex<double> s = z + std::complex<double>(0.5, 2.25);
    CHECK(s.real() == 2.0);
    CHECK(s.imag() == 0.0);

    static_assert(sizeof(std::complex<double>) == 2 * sizeof(double));
    static_assert(std::is_standard_layout_v<std::complex<double>>);
    static_assert(std::is_trivially_copyable_v<std::complex<double>>);
}

TEST_CASE("cxx complex model: division by a zero complex is a precondition check") {
    const fs::path header = fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "complex";
    REQUIRE(fs::is_regular_file(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("not PROVED") != std::string::npos);
    CHECK(src.find("not CLEAN") != std::string::npos);
    CHECK(src.find("if (__zero) return *this;") == std::string::npos);

    const std::string body = body_of(src, "complex& operator/=(const complex& __z)");
    REQUIRE_FALSE(body.empty());
    const auto check =
        body.find("__prism_check(!__zero, \"FUNC-CONTRACT\", \"complex division by zero\");");
    REQUIRE(check != std::string::npos);
    const auto zero_if = body.find("if (__zero)", check);
    REQUIRE(zero_if != std::string::npos);
    CHECK(check < zero_if);
    const std::string between = body.substr(check, zero_if - check);
    CHECK(between.find("return *this") == std::string::npos);
    CHECK(between.find("__builtin_trap") == std::string::npos);
    const std::string arm = brace_after(body, zero_if);
    REQUIRE_FALSE(arm.empty());
    CHECK(arm.find("__builtin_trap") != std::string::npos);
    CHECK(arm.find("return *this") == std::string::npos);
    CHECK(arm.find("return ") == std::string::npos);
    CHECK(arm.find("_M_real") == std::string::npos);
    CHECK(arm.find("_M_imag") == std::string::npos);
    CHECK(arm.find("/") == std::string::npos);

    const auto arm_at = body.find(arm, zero_if);
    REQUIRE(arm_at != std::string::npos);
    const auto after = arm_at + arm.size();
    const auto den = body.find("const _Tp __den", after);
    const auto imag = body.find("_M_imag =", after);
    const auto real = body.find("_M_real =", after);
    const auto quot = body.find(" / __den", after);
    const auto ret = body.find("return *this", after);
    REQUIRE(den != std::string::npos);
    REQUIRE(imag != std::string::npos);
    REQUIRE(real != std::string::npos);
    REQUIRE(quot != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(den < imag);
    CHECK(imag < real);
    CHECK(real < ret);
    CHECK(count_of(body, "return *this") == 1);
    CHECK(count_of(body, "__builtin_trap") == 1);
    CHECK(count_of(src, "complex division by zero") == 1);

    const std::string scalar =
        body_of(src, "complex<_Tp> operator/(const complex<_Tp>& __x, const _Tp& __y)");
    REQUIRE_FALSE(scalar.empty());
    CHECK(scalar.find("return __x / complex<_Tp>(__y);") != std::string::npos);
    CHECK(scalar.find("__prism_check") == std::string::npos);
    CHECK(scalar.find("__builtin_trap") == std::string::npos);
    CHECK(scalar.find("return *this") == std::string::npos);

    const std::complex<double> a(1.0, 2.0);

    // One part nonzero is a real division, not the precondition failure.
    g_check = {};
    const auto q = a / std::complex<double>(0.0, 1.0);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);
    CHECK(std::strcmp(g_check.cls, "FUNC-CONTRACT") == 0);
    CHECK(q.real() == 2.0);
    CHECK(q.imag() == -1.0);

    // A nonzero scalar builds complex(y, 0) and takes the same quotient path.
    g_check = {};
    const auto half = a / 2.0;
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);
    CHECK(half.real() == 0.5);
    CHECK(half.imag() == 1.0);
}

TEST_CASE("cxx complex model: abs of (0, 0) is 0 and a fitting magnitude is the root of the sum of squares") {
    g_check = {};
    CHECK(std::abs(std::complex<double>(0.0, 0.0)) == 0.0);
    CHECK(std::abs(std::complex<double>(-0.0, -0.0)) == 0.0);
    CHECK(g_check.n == 0);

    g_check = {};
    CHECK(std::abs(std::complex<double>(3.0, 4.0)) == 5.0);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);
    CHECK(std::strcmp(g_check.cls, "FUNC-CONTRACT") == 0);

    g_check = {};
    CHECK(std::abs(std::complex<double>(-3.0, 0.0)) == 3.0);
    CHECK(g_check.ok != 0);

    g_check = {};
    CHECK(std::abs(std::complex<double>(0.0, -5.0)) == 5.0);
    CHECK(g_check.ok != 0);

    g_check = {};
    CHECK(std::abs(std::complex<float>(0.0f, 0.0f)) == 0.0f);
    CHECK(g_check.n == 0);
    CHECK(std::abs(std::complex<float>(3.0f, 4.0f)) == 5.0f);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);

    g_check = {};
    CHECK(std::abs(std::complex<long double>(0.0L, 0.0L)) == 0.0L);
    CHECK(g_check.n == 0);
    CHECK(std::abs(std::complex<long double>(3.0L, 4.0L)) == 5.0L);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);
}

TEST_CASE("cxx complex model: a square that does not fit traps, and arg polar proj stay omitted") {
    const fs::path header = fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "complex";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#include <cmath>") == std::string::npos);
    CHECK(src.find("models/cxx/cmath") == std::string::npos);
    {
        std::istringstream lines(src);
        std::string line;
        while (std::getline(lines, line)) {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            CHECK(line.compare(i, 8, "#include") != 0);
        }
    }
    CHECK(src.find("atanh, abs,") == std::string::npos);
    CHECK(src.find("atanh, arg, norm, polar") == std::string::npos);
    CHECK(src.find("atanh, arg, polar, and proj are not declared") != std::string::npos);
    CHECK(src.find("__prism_check(!__zero, \"FUNC-CONTRACT\", \"complex division by zero\");") != std::string::npos);
    CHECK(src.find("if (__zero) return *this;") == std::string::npos);
    CHECK(count_of(src, "complex division by zero") == 1);

    const std::string body = body_of(src, "_Tp abs(const complex<_Tp>& __z)");
    REQUIRE_FALSE(body.empty());
    const auto zero = body.find("if (__re == _Tp() && __im == _Tp()) return _Tp();");
    const auto check = body.find("__prism_check(__fit ? 1 : 0, \"FUNC-CONTRACT\", \"complex abs square does not fit\");");
    const auto trap = body.find("if (!__fit) __builtin_trap();");
    const auto sum = body.find("const _Tp __sum = __rr + __ii;");
    const auto sqrt_d = body.find("__builtin_sqrt(__sum)");
    const auto sqrt_f = body.find("__builtin_sqrtf(__sum)");
    const auto sqrt_l = body.find("__builtin_sqrtl(__sum)");
    REQUIRE(zero != std::string::npos);
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(sum != std::string::npos);
    REQUIRE(sqrt_d != std::string::npos);
    REQUIRE(sqrt_f != std::string::npos);
    REQUIRE(sqrt_l != std::string::npos);
    CHECK(zero < check);
    CHECK(check < trap);
    CHECK(trap < sum);
    CHECK(sum < sqrt_d);
    CHECK(sqrt_d < sqrt_f);
    CHECK(sqrt_f < sqrt_l);
    CHECK(count_of(body, "return ") == 4);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("return __sum") == std::string::npos);
    CHECK(body.find("return __rr") == std::string::npos);
    CHECK(body.find("return __ii") == std::string::npos);
    CHECK(count_of(src, "complex abs square does not fit") == 1);

    for (const char* sig : {"arg(const complex", "polar(", "proj(const complex",
                            "sinh(const complex", "cosh(const complex", "tanh(const complex",
                            "asinh(const complex", "acosh(const complex", "atanh(const complex",
                            "sqrt(const complex", "exp(const complex", "hypot("}) {
        CAPTURE(sig);
        CHECK(src.find(sig) == std::string::npos);
    }
}

TEST_CASE("cxx complex model: norm of (0, 0) is 0 and a fitting value is the sum of the squares") {
    g_check = {};
    CHECK(std::norm(std::complex<double>(0.0, 0.0)) == 0.0);
    CHECK(std::norm(std::complex<double>(-0.0, -0.0)) == 0.0);
    CHECK(g_check.n == 0);

    g_check = {};
    CHECK(std::norm(std::complex<double>(3.0, 4.0)) == 25.0);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);
    CHECK(std::strcmp(g_check.cls, "FUNC-CONTRACT") == 0);
    CHECK(std::strcmp(g_check.msg, "complex norm square does not fit") == 0);

    g_check = {};
    CHECK(std::norm(std::complex<double>(-3.0, 0.0)) == 9.0);
    CHECK(g_check.ok != 0);

    g_check = {};
    CHECK(std::norm(std::complex<double>(0.0, -5.0)) == 25.0);
    CHECK(g_check.ok != 0);

    g_check = {};
    CHECK(std::norm(std::complex<float>(0.0f, 0.0f)) == 0.0f);
    CHECK(g_check.n == 0);
    CHECK(std::norm(std::complex<float>(3.0f, 4.0f)) == 25.0f);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);

    g_check = {};
    CHECK(std::norm(std::complex<long double>(0.0L, 0.0L)) == 0.0L);
    CHECK(g_check.n == 0);
    CHECK(std::norm(std::complex<long double>(3.0L, 4.0L)) == 25.0L);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);

    const fs::path header = fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "complex";
    const std::string src = slurp(header);
    const std::string body = body_of(src, "_Tp norm(const complex<_Tp>& __z)");
    REQUIRE_FALSE(body.empty());
    const auto zero = body.find("if (__re == _Tp() && __im == _Tp()) return _Tp();");
    const auto check =
        body.find("__prism_check(__fit ? 1 : 0, \"FUNC-CONTRACT\", \"complex norm square does not fit\");");
    const auto trap = body.find("if (!__fit) __builtin_trap();");
    const auto sum = body.find("return __rr + __ii;");
    REQUIRE(zero != std::string::npos);
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(sum != std::string::npos);
    CHECK(zero < check);
    CHECK(check < trap);
    CHECK(trap < sum);
    if (check < trap)
        CHECK(body.substr(check, trap - check).find("return") == std::string::npos);
    CHECK(count_of(body, "return ") == 2);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("return __rr;") == std::string::npos);
    CHECK(body.find("return __ii;") == std::string::npos);
    CHECK(body.find("return __sum") == std::string::npos);
    CHECK(body.find("__builtin_sqrt") == std::string::npos);
    CHECK(count_of(src, "complex norm square does not fit") == 1);
    CHECK(count_of(src, "_Tp norm(const complex<_Tp>& __z)") == 1);
}
