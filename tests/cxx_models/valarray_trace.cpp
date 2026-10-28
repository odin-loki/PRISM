// Trace of std::valarray. `#include <valarray>` is the model when its
// directory is first on the include path, and libstdc++ otherwise.
// A default valarray has size 0. One stored value is read back at [0].
// resize to a new length replaces the previous buffer. An empty [0] is not
// traced: libstdc++ would use a null pointer, and the model reports a check.
// sum of an empty valarray is 0. argv "ovf" / "uovf" / "fovf" asks for a
// total that does not fit. The model reports a check and does not print
// wrapped=. libstdc++ wraps the unsigned sum and, without a sanitizer,
// wraps the signed sum.
// min and max of a non-empty valarray are a stored extreme. argv "min0" /
// "max0" / "minz" / "maxz" asks for an empty valarray. The model reports a
// check and does not print value=. libstdc++ with assertions aborts in
// the library. An empty [0] is not traced.
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

// No default constructor. min and max must copy a stored element.
struct ValarrayTag {
    int key;
    int id;
    ValarrayTag() = delete;
    ValarrayTag(int k, int i) : key(k), id(i) {}
    bool operator<(const ValarrayTag& o) const { return key < o.key; }
};

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

    std::valarray<int> span(4);
    span[0] = 4;
    span[1] = -2;
    span[2] = 9;
    span[3] = 3;
    std::printf("span %d %d\n", span.min(), span.max());

    std::valarray<int> alone(1);
    alone[0] = 7;
    std::printf("alone %d %d\n", alone.min(), alone.max());

    std::valarray<int> ties(3);
    ties[0] = 5;
    ties[1] = 5;
    ties[2] = 1;
    std::printf("ties %d %d\n", ties.min(), ties.max());

    std::valarray<int> ends(3);
    ends[0] = 0;
    ends[1] = 2;
    ends[2] = -4;
    std::printf("ends %d %d\n", ends.min(), ends.max());

    std::valarray<unsigned> uu(3);
    uu[0] = 2;
    uu[1] = 0;
    uu[2] = 9;
    std::printf("uspan %u %u\n", uu.min(), uu.max());

    std::valarray<double> dd(3);
    dd[0] = 1.5;
    dd[1] = -0.25;
    dd[2] = 1.5;
    std::printf("dspan %.2f %.2f\n", dd.min(), dd.max());

    std::valarray<bool> bb(2);
    bb[0] = true;
    bb[1] = false;
    std::printf("bspan %d %d\n", static_cast<int>(bb.min()), static_cast<int>(bb.max()));

    std::valarray<long long> wide(2);
    wide[0] = std::numeric_limits<long long>::max();
    wide[1] = std::numeric_limits<long long>::min();
    std::printf("wide %lld %lld\n", wide.min(), wide.max());

    std::valarray<ValarrayTag> tags(ValarrayTag(2, 1), 3);
    tags[1] = ValarrayTag(2, 2);
    tags[2] = ValarrayTag(0, 3);
    const ValarrayTag tmin = tags.min();
    const ValarrayTag tmax = tags.max();
    std::printf("tag %d %d %d %d\n", tmin.key, tmin.id, tmax.key, tmax.id);

    if (arg_is(argc, argv, "min0")) {
        std::valarray<int> e;
        std::printf("value=%d\n", e.min());
        return 2;
    }
    if (arg_is(argc, argv, "max0")) {
        std::valarray<int> e;
        std::printf("value=%d\n", e.max());
        return 2;
    }
    if (arg_is(argc, argv, "minz")) {
        std::valarray<int> e(1);
        e.resize(0);
        std::printf("value=%d\n", e.min());
        return 2;
    }
    if (arg_is(argc, argv, "maxz")) {
        std::valarray<double> e;
        e.resize(0);
        std::printf("value=%g\n", e.max());
        return 2;
    }
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
    return empty.size() == 0 && one.size() == 0 && empty.sum() == 0 && one.sum() == 0 && span.min() == -2 &&
                   span.max() == 9 && alone.min() == 7 && alone.max() == 7 && tmin.id == 3 &&
                   (tmax.id == 1 || tmax.id == 2)
               ? 0
               : 1;
}
