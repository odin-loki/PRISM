// Direct checks of the <complex> stand-in. Included by path so prism_tests
// does not put the model directory on the include path for every TU.
// Division by complex(0, 0) must call __prism_check. real() and imag() of a
// constructed value are the values that were passed in. No heap allocation.

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
#include <type_traits>

namespace {

struct PrismCheck {
    int n = 0;
    int ok = 1;
    const char* cls = "";
    const char* msg = "";
};

PrismCheck g_check;

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
    const std::complex<double> a(1.0, 2.0);

    g_check = {};
    const auto zero = a / std::complex<double>(0, 0);
    (void)zero;
    CHECK(g_check.n == 1);
    CHECK(g_check.ok == 0);
    CHECK(std::strcmp(g_check.cls, "FUNC-CONTRACT") == 0);
    CHECK(std::strstr(g_check.msg, "zero") != nullptr);

    // A scalar zero converts to complex(0, 0). Same check, still no quotient.
    g_check = {};
    const auto scalar = a / 0.0;
    (void)scalar;
    CHECK(g_check.n == 1);
    CHECK(g_check.ok == 0);

    // One part nonzero is a real division, not the precondition failure.
    g_check = {};
    const auto q = a / std::complex<double>(0.0, 1.0);
    CHECK(g_check.n == 1);
    CHECK(g_check.ok != 0);
    CHECK(q.real() == 2.0);
    CHECK(q.imag() == -1.0);
}
