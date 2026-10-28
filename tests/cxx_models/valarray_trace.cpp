// Trace of std::valarray. `#include <valarray>` is the model when its
// directory is first on the include path, and libstdc++ otherwise.
// A default valarray has size 0. One stored value is read back at [0].
// resize to a new length replaces the previous buffer. An empty [0] is not
// traced: libstdc++ would use a null pointer, and the model reports a check.
// sum of an empty valarray is 0. argv "ovf" / "uovf" / "fovf" asks for a
// total that does not fit. The model reports a check and does not print
// wrapped=. libstdc++ wraps the unsigned sum and, without a sanitizer,
// wraps the signed sum.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <valarray>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <valarray> never calls it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

static int arg_is(int argc, char** argv, const char* want) {
    return argc > 1 && argv[1] != nullptr && std::strcmp(argv[1], want) == 0;
}

int main(int argc, char** argv) {
    std::valarray<int> empty;
    std::printf("empty %zu %d\n", empty.size(), empty.sum());

    std::valarray<int> one;
    one.resize(1, 7);
    std::printf("one %zu %d %d\n", one.size(), one[0], one.sum());

    one.resize(3, 4);
    std::printf("grow %zu %d %d\n", one.size(), one[0], one.sum());

    one.resize(1, 9);
    std::printf("shrink %zu %d %d\n", one.size(), one[0], one.sum());

    one.resize(0);
    std::printf("zero %zu %d\n", one.size(), one.sum());

    std::valarray<int> mix(3);
    mix[0] = 1;
    mix[1] = 2;
    mix[2] = 4;
    std::printf("mix %d\n", mix.sum());

    std::valarray<unsigned> u(2);
    u[0] = 4;
    u[1] = 5;
    std::printf("unsigned %u\n", u.sum());

    std::valarray<double> d;
    std::printf("double0 %.1f\n", d.sum());
    d.resize(2, 0.0);
    d[0] = 1.5;
    d[1] = 2.5;
    std::printf("double %.1f\n", d.sum());

    std::valarray<int> edge(2);
    edge[0] = 0;
    edge[1] = std::numeric_limits<int>::min();
    std::printf("min0 %d\n", edge.sum());
    edge[0] = 1;
    std::printf("min1 %d\n", edge.sum());
    edge[0] = std::numeric_limits<int>::max();
    edge[1] = -1;
    std::printf("maxm %d\n", edge.sum());
    edge[1] = 0;
    std::printf("max0 %d\n", edge.sum());
    edge[0] = 0;
    edge[1] = std::numeric_limits<int>::max();
    std::printf("maxb %d\n", edge.sum());

    std::valarray<long long> ll(2);
    ll[0] = 0;
    ll[1] = std::numeric_limits<long long>::min();
    std::printf("llmin %lld\n", ll.sum());
    ll[0] = std::numeric_limits<long long>::max();
    ll[1] = -1;
    std::printf("llmax %lld\n", ll.sum());

    if (arg_is(argc, argv, "ovf")) {
        std::valarray<int> big(2);
        big[0] = std::numeric_limits<int>::max();
        big[1] = 1;
        std::printf("wrapped=%d\n", big.sum());
        return 2;
    }
    if (arg_is(argc, argv, "uovf")) {
        std::valarray<unsigned> big(2);
        big[0] = std::numeric_limits<unsigned>::max();
        big[1] = 1u;
        std::printf("wrapped=%u\n", big.sum());
        return 2;
    }
    if (arg_is(argc, argv, "fovf")) {
        std::valarray<double> big(2);
        big[0] = std::numeric_limits<double>::max();
        big[1] = std::numeric_limits<double>::max();
        std::printf("wrapped=%g\n", big.sum());
        return 2;
    }
    return empty.size() == 0 && one.size() == 0 && empty.sum() == 0 && one.sum() == 0 ? 0 : 1;
}
