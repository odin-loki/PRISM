// Differential trace of std::minstd_rand and std::minstd_rand0. Compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/random) first on the include path. The first
// minstd_rand output after seed(1) is the Park-Miller step 48271. The first
// minstd_rand0 output after seed(1) is 16807. discard(0) leaves the next
// output the same as a twin engine. random_device is not called: its value
// is not a proof and is not compared.
// uniform_int_distribution<int> with equal bounds returns that bound.
// A draw from a span that fits is inside [a, b]. The sample itself is not
// printed: the model uses __VERIFIER_nondet_uint, which is not a proof of
// uniformity and is not the libstdc++ engine sequence. argv "invert" is an
// inverted range. argv "span" is a span that does not fit in an unsigned
// 32-bit count. Those paths must not print wrapped=.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

// The model declares these and does not define them. libstdc++ <random> does
// not call them. A failing check aborts before a wrapped value can be printed.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

extern "C" unsigned int __VERIFIER_nondet_uint(void) { return 7u; }

static int arg_is(int argc, char** argv, const char* name) {
    return argc >= 2 && std::strcmp(argv[1], name) == 0;
}

static unsigned long long u(std::minstd_rand::result_type v) { return static_cast<unsigned long long>(v); }

static unsigned long long u0(std::minstd_rand0::result_type v) {
    return static_cast<unsigned long long>(v);
}

int main(int argc, char** argv) {
    if (arg_is(argc, argv, "invert")) {
        volatile int hi = 3;
        volatile int lo = 1;
        std::minstd_rand g;
        std::uniform_int_distribution<int> d(hi, lo);
        const int r = d(g);
        std::printf("wrapped=%d\n", r);
        return 0;
    }
    if (arg_is(argc, argv, "span")) {
        volatile int lo = INT_MIN;
        volatile int hi = INT_MAX;
        std::minstd_rand g;
        std::uniform_int_distribution<int> d(lo, hi);
        const int r = d(g);
        std::printf("wrapped=%d\n", r);
        return 0;
    }

    std::minstd_rand a;
    a.seed(1);
    std::printf("first=%llu\n", u(a()));

    std::minstd_rand b;
    b.seed(1);
    std::minstd_rand twin;
    twin.seed(1);
    b.discard(0);
    std::printf("discard0=%llu twin=%llu\n", u(b()), u(twin()));

    std::minstd_rand step;
    step.seed(1);
    const auto first = step();
    const auto second = step();
    std::printf("second=%llu\n", u(second));

    std::minstd_rand skipped;
    skipped.seed(1);
    skipped.discard(1);
    std::printf("after_discard1=%llu same_first=%d\n", u(skipped()), int(first == 48271u));

    std::minstd_rand zero;
    zero.seed(0);
    std::printf("seed0=%llu min=%llu max=%llu\n", u(zero()), u(std::minstd_rand::min()), u(std::minstd_rand::max()));

    std::minstd_rand0 m0;
    m0.seed(1);
    std::printf("m0first=%llu\n", u0(m0()));

    std::minstd_rand0 m0b;
    m0b.seed(1);
    std::minstd_rand0 m0twin;
    m0twin.seed(1);
    m0b.discard(0);
    std::printf("m0discard0=%llu m0twin=%llu\n", u0(m0b()), u0(m0twin()));

    std::minstd_rand0 m0step;
    m0step.seed(1);
    (void)m0step();
    std::printf("m0second=%llu\n", u0(m0step()));

    std::minstd_rand0 m0zero;
    m0zero.seed(0);
    std::minstd_rand0 m0mod;
    m0mod.seed(std::minstd_rand0::modulus);
    const auto two_mod = static_cast<std::minstd_rand0::result_type>(
        static_cast<unsigned long long>(std::minstd_rand0::modulus) * 2ull);
    std::minstd_rand0 m0twice;
    m0twice.seed(two_mod);
    std::printf("m0seed0=%llu m0mod=%llu m0twice=%llu m0min=%llu m0max=%llu mul=%llu\n", u0(m0zero()),
                u0(m0mod()), u0(m0twice()), u0(std::minstd_rand0::min()), u0(std::minstd_rand0::max()),
                u0(std::minstd_rand0::multiplier));

    std::minstd_rand ug;
    ug.seed(1);
    std::uniform_int_distribution<int> same(4, 4);
    std::uniform_int_distribution<int> neg(-7, -7);
    std::uniform_int_distribution<int> bounds;
    std::minstd_rand ug2;
    ug2.seed(1);
    std::uniform_int_distribution<int> span(1, 6);
    const int draw = span(ug2);
    const int inside = draw >= 1 && draw <= 6;
    std::printf("same=%d neg=%d defa=%d defb=%d inside=%d lo=%d hi=%d\n", same(ug), neg(ug), bounds.a(),
                bounds.b(), inside, span.a(), span.b());
    return 0;
}
