// The <random> model is the file src/prism/pir/models/cxx/random. This
// translation unit includes that file and also reads it as text. The first
// minstd_rand output after seed(1) is 48271. The first minstd_rand0 output
// after seed(1) is 16807. The header text says random_device is not a proof.
// minstd_rand0 is declared. mt19937, ranlux, knuth_b, and
// default_random_engine stay in the omitted-engines list.
// uniform_int_distribution<int> stores a and b. An inverted range is a
// FUNC-CONTRACT check, then a trap, and that path does not store the pair.
// operator() on an inverted range does not return a value. a == b returns a
// and does not call __VERIFIER_nondet_uint. A span that does not fit in an
// unsigned 32-bit count traps and does not return a wrapped value. A span
// that fits returns a + (nondet % span), inside [a, b]. That is not a proof
// of uniformity.

#include <doctest/doctest.h>

#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#undef _GLIBCXX_RANDOM
#include "../../src/prism/pir/models/cxx/random"

namespace fs = std::filesystem;

namespace {
unsigned int g_nondet_value = 0;
int g_nondet_calls = 0;
}  // namespace

extern "C" unsigned int __VERIFIER_nondet_uint(void) {
    ++g_nondet_calls;
    return g_nondet_value;
}

TEST_CASE("cxx random model: seed 1 is 48271 and random_device is not a proof") {
    const fs::path header = fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "random";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    std::ifstream in(header, std::ios::binary);
    REQUIRE(in);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(text.find("random_device is not a proof") != std::string::npos);
    CHECK(text.find("not a measure of entropy") != std::string::npos);
    CHECK(text.find("not PROVED") != std::string::npos);
    CHECK(text.find("not CLEAN") != std::string::npos);
    CHECK(text.find("#ifndef _GLIBCXX_RANDOM") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("#include <vector>") == std::string::npos);
    CHECK(text.find("#include <functional>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("return 0") == std::string::npos);
    CHECK(text.find("class minstd_rand0") != std::string::npos);
    CHECK(text.find("uniform_int_distribution") != std::string::npos);
    CHECK(text.find("shuffle") != std::string::npos);

    const auto omitted = text.find("the other engines (");
    REQUIRE(omitted != std::string::npos);
    const auto omitted_end = text.find(')', omitted);
    REQUIRE(omitted_end != std::string::npos);
    const std::string engines = text.substr(omitted, omitted_end - omitted);
    CHECK(engines.find("minstd_rand0") == std::string::npos);
    CHECK(engines.find("mt19937") != std::string::npos);
    CHECK(engines.find("ranlux") != std::string::npos);
    CHECK(engines.find("knuth_b") != std::string::npos);
    CHECK(engines.find("default_random_engine") != std::string::npos);

    static_assert(std::minstd_rand::multiplier == 48271);
    static_assert(std::minstd_rand::modulus == 2147483647);
    static_assert(std::minstd_rand::default_seed == 1);

    std::minstd_rand eng;
    eng.seed(1);
    CHECK(eng() == static_cast<std::minstd_rand::result_type>(48271));

    std::minstd_rand again;
    again.seed(1);
    (void)again();
    CHECK(again() == static_cast<std::minstd_rand::result_type>(182605794));

    std::minstd_rand a;
    a.seed(1);
    std::minstd_rand twin;
    twin.seed(1);
    a.discard(0);
    CHECK(a() == twin());
}

TEST_CASE("cxx random model: minstd_rand0 seed 1 is 16807") {
    static_assert(std::minstd_rand0::multiplier == 16807);
    static_assert(std::minstd_rand0::increment == 0);
    static_assert(std::minstd_rand0::modulus == 2147483647);
    static_assert(std::minstd_rand0::default_seed == 1);
    static_assert(std::minstd_rand0::min() == 1);
    static_assert(std::minstd_rand0::max() == std::minstd_rand0::modulus - 1);

    std::minstd_rand0 eng;
    eng.seed(1);
    CHECK(eng() == static_cast<std::minstd_rand0::result_type>(16807));

    std::minstd_rand0 again;
    again.seed(1);
    (void)again();
    CHECK(again() == static_cast<std::minstd_rand0::result_type>(282475249));

    std::minstd_rand0 a;
    a.seed(1);
    std::minstd_rand0 twin;
    twin.seed(1);
    a.discard(0);
    const auto first = a();
    CHECK(first == static_cast<std::minstd_rand0::result_type>(16807));
    CHECK(first == twin());

    std::minstd_rand0 skipped;
    skipped.seed(1);
    skipped.discard(1);
    CHECK(skipped() == static_cast<std::minstd_rand0::result_type>(282475249));

    std::minstd_rand0 zero;
    zero.seed(0);
    CHECK(zero() == static_cast<std::minstd_rand0::result_type>(16807));

    std::minstd_rand0 wrapped;
    wrapped.seed(std::minstd_rand0::modulus);
    CHECK(wrapped() == static_cast<std::minstd_rand0::result_type>(16807));

    const auto two_mod = static_cast<std::minstd_rand0::result_type>(
        static_cast<unsigned long long>(std::minstd_rand0::modulus) * 2ull);
    std::minstd_rand0 twice;
    twice.seed(two_mod);
    CHECK(twice() == static_cast<std::minstd_rand0::result_type>(16807));
}

TEST_CASE("cxx random model: uniform_int_distribution<int> stays inside [a, b]") {
    const fs::path header = fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "random";
    REQUIRE(fs::is_regular_file(header));

    std::ifstream in(header, std::ios::binary);
    REQUIRE(in);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(text.find("#include <vector>") == std::string::npos);
    CHECK(text.find("#include <functional>") == std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("class mt19937") == std::string::npos);
    CHECK(text.find("class uniform_real_distribution") == std::string::npos);
    CHECK(text.find("class bernoulli_distribution") == std::string::npos);
    CHECK(text.find("class normal_distribution") == std::string::npos);
    CHECK(text.find("uniform_int_distribution<long>") == std::string::npos);
    CHECK(text.find("uniform_int_distribution<int>") != std::string::npos);
    CHECK(text.find("This is not a proof of uniformity.") != std::string::npos);
    CHECK(text.find("not PROVED") != std::string::npos);
    CHECK(text.find("not CLEAN") != std::string::npos);
    CHECK(text.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(text.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(text.find("return 0") == std::string::npos);

    const auto omitted = text.find("the other engines (");
    REQUIRE(omitted != std::string::npos);
    const auto omitted_end = text.find(')', omitted);
    REQUIRE(omitted_end != std::string::npos);
    const std::string engines = text.substr(omitted, omitted_end - omitted);
    CHECK(engines.find("minstd_rand0") == std::string::npos);
    CHECK(engines.find("mt19937") != std::string::npos);

    const auto ctor = text.find("explicit uniform_int_distribution(");
    const auto op = text.find("result_type operator()(URNG&)");
    REQUIRE(ctor != std::string::npos);
    REQUIRE(op != std::string::npos);
    REQUIRE(ctor < op);
    const std::string ctor_body = text.substr(ctor, op - ctor);
    const auto inv =
        ctor_body.find("__prism_check(0, \"FUNC-CONTRACT\", \"uniform_int_distribution range is inverted\")");
    REQUIRE(inv != std::string::npos);
    const auto inv_trap = ctor_body.find("__builtin_trap()", inv);
    const auto ctor_else = ctor_body.find("else", inv_trap);
    const auto store_a = ctor_body.find("_M_a = a;");
    const auto store_b = ctor_body.find("_M_b = b;");
    REQUIRE(inv_trap != std::string::npos);
    REQUIRE(ctor_else != std::string::npos);
    REQUIRE(store_a != std::string::npos);
    REQUIRE(store_b != std::string::npos);
    CHECK(inv < inv_trap);
    CHECK(inv_trap < ctor_else);
    CHECK(ctor_else < store_a);
    CHECK(store_a < store_b);
    const std::string inv_arm = ctor_body.substr(inv, inv_trap - inv);
    CHECK(inv_arm.find("_M_a") == std::string::npos);
    CHECK(inv_arm.find("_M_b") == std::string::npos);
    CHECK(inv_arm.find("return") == std::string::npos);

    const std::string op_body = text.substr(op);
    const auto op_inv =
        op_body.find("__prism_check(0, \"FUNC-CONTRACT\", \"uniform_int_distribution range is inverted\")");
    REQUIRE(op_inv != std::string::npos);
    const auto op_trap = op_body.find("__builtin_trap()", op_inv);
    const auto eq = op_body.find("if (_M_a == _M_b)");
    const auto eq_ret = op_body.find("return _M_a;");
    const auto nondet = op_body.find("__VERIFIER_nondet_uint()");
    const auto span_msg =
        op_body.find("uniform_int_distribution span does not fit in an unsigned 32-bit count");
    REQUIRE(op_trap != std::string::npos);
    REQUIRE(eq != std::string::npos);
    REQUIRE(eq_ret != std::string::npos);
    REQUIRE(nondet != std::string::npos);
    REQUIRE(span_msg != std::string::npos);
    CHECK(op_inv < op_trap);
    CHECK(op_trap < eq);
    CHECK(eq < eq_ret);
    CHECK(eq_ret < nondet);
    const std::string op_inv_arm = op_body.substr(op_inv, op_trap - op_inv);
    CHECK(op_inv_arm.find("return") == std::string::npos);
    CHECK(op_inv_arm.find("__VERIFIER_nondet_uint") == std::string::npos);
    const std::string eq_arm = op_body.substr(eq, eq_ret - eq);
    CHECK(eq_arm.find("__VERIFIER_nondet_uint") == std::string::npos);

    const auto span_trap = op_body.find("__builtin_trap()", span_msg);
    const auto span_else = op_body.find("else", span_trap);
    const auto pct = op_body.find("% span");
    const auto sum_ret =
        op_body.find("return static_cast<int>(static_cast<long long>(_M_a) + static_cast<long long>(pick));");
    REQUIRE(span_trap != std::string::npos);
    REQUIRE(span_else != std::string::npos);
    REQUIRE(pct != std::string::npos);
    REQUIRE(sum_ret != std::string::npos);
    CHECK(eq_ret < span_msg);
    CHECK(span_msg < span_trap);
    CHECK(span_trap < span_else);
    CHECK(span_else < pct);
    CHECK(pct < sum_ret);
    const std::string span_arm = op_body.substr(span_msg, span_trap - span_msg);
    CHECK(span_arm.find("return") == std::string::npos);
    CHECK(span_arm.find("%") == std::string::npos);
    CHECK(span_arm.find("__VERIFIER_nondet_uint") == std::string::npos);

    std::minstd_rand eng;
    g_nondet_calls = 0;
    g_nondet_value = 99;
    std::uniform_int_distribution<int> same(4, 4);
    CHECK(same.a() == 4);
    CHECK(same.b() == 4);
    CHECK(same.min() == 4);
    CHECK(same.max() == 4);
    CHECK(same(eng) == 4);
    CHECK(g_nondet_calls == 0);

    g_nondet_calls = 0;
    std::uniform_int_distribution<int> neg(-7, -7);
    CHECK(neg(eng) == -7);
    CHECK(g_nondet_calls == 0);

    g_nondet_calls = 0;
    std::uniform_int_distribution<int> zero(0, 0);
    CHECK(zero(eng) == 0);
    CHECK(g_nondet_calls == 0);

    std::uniform_int_distribution<int> def;
    CHECK(def.a() == 0);
    CHECK(def.b() == INT_MAX);

    auto draw = [&](int a, int b, unsigned int nondet) {
        g_nondet_value = nondet;
        g_nondet_calls = 0;
        std::uniform_int_distribution<int> dist(a, b);
        const int got = dist(eng);
        const unsigned long long span =
            static_cast<unsigned long long>(static_cast<long long>(b) - static_cast<long long>(a)) + 1ull;
        const unsigned long long pick = static_cast<unsigned long long>(nondet) % span;
        const int expect = static_cast<int>(static_cast<long long>(a) + static_cast<long long>(pick));
        CHECK(g_nondet_calls == 1);
        CHECK(got == expect);
        CHECK(got >= a);
        CHECK(got <= b);
        CHECK(dist.a() == a);
        CHECK(dist.b() == b);
    };

    draw(1, 6, 0);
    draw(1, 6, 5);
    draw(1, 6, 6);
    draw(1, 6, 7);
    draw(-3, 2, 0);
    draw(-3, 2, 5);
    draw(-3, 2, 100);
    draw(0, INT_MAX, 0);
    draw(0, INT_MAX, 2147483647u);
    draw(-1, INT_MAX, 1);
    draw(INT_MIN, INT_MAX - 1, 0);
    draw(INT_MIN, INT_MAX - 1, 4294967294u);
    draw(INT_MIN, INT_MAX - 1, 4294967295u);
}
