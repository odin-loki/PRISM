// Differential trace of std::minstd_rand and std::minstd_rand0. Compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/random) first on the include path. The first
// minstd_rand output after seed(1) is the Park-Miller step 48271. The first
// minstd_rand0 output after seed(1) is 16807. discard(0) leaves the next
// output the same as a twin engine. random_device is not called: its value
// is not a proof and is not compared.
#include <cstdio>
#include <random>

static unsigned long long u(std::minstd_rand::result_type v) { return static_cast<unsigned long long>(v); }

static unsigned long long u0(std::minstd_rand0::result_type v) {
    return static_cast<unsigned long long>(v);
}

int main() {
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
    return 0;
}
