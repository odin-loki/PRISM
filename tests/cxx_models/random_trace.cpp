// Differential trace of std::minstd_rand. Compiled once against libstdc++ and
// once with PRISM's model header (src/prism/pir/models/cxx/random) first on
// the include path. The first output after seed(1) is the Park-Miller step
// 48271. discard(0) leaves the next output the same as a twin engine.
// random_device is not called: its value is not a proof and is not compared.
#include <cstdio>
#include <random>

static unsigned long long u(std::minstd_rand::result_type v) { return static_cast<unsigned long long>(v); }

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
    return 0;
}
