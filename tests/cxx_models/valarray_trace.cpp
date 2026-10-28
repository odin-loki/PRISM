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
// operator+ and operator- of two valarrays of equal size store each sum or
// difference. Two empty valarrays print size 0. argv "addsz" / "subsz"
// asks for different sizes. argv "addovf" / "subovf" / "uaddovf" /
// "usubovf" / "faddovf" / "fsubovf" asks for an element that does not fit.
// The model reports a check and does not print partial= or wrapped=.
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

    std::valarray<int> add_empty_a;
    std::valarray<int> add_empty_b;
    const std::valarray<int> add_empty = add_empty_a + add_empty_b;
    const std::valarray<int> sub_empty = add_empty_a - add_empty_b;
    std::printf("add0 %zu %zu\n", add_empty.size(), sub_empty.size());
    std::valarray<int> add_zero(3);
    add_zero.resize(0);
    const std::valarray<int> add_resized = add_zero + add_empty_b;
    std::printf("addz %zu\n", add_resized.size());

    std::valarray<int> lhs(3);
    lhs[0] = 1;
    lhs[1] = 2;
    lhs[2] = 4;
    std::valarray<int> rhs(3);
    rhs[0] = 10;
    rhs[1] = 20;
    rhs[2] = 30;
    const std::valarray<int> sm = lhs + rhs;
    const std::valarray<int> df = rhs - lhs;
    std::printf("add %d %d %d\n", sm[0], sm[1], sm[2]);
    std::printf("sub %d %d %d\n", df[0], df[1], df[2]);
    const std::valarray<int> self_sum = lhs + lhs;
    const std::valarray<int> self_diff = lhs - lhs;
    std::printf("self %d %d %d %d\n", self_sum[0], self_sum[1], self_diff[0], self_diff[2]);

    std::valarray<int> hi(1);
    hi[0] = std::numeric_limits<int>::max();
    std::valarray<int> lo(1);
    lo[0] = std::numeric_limits<int>::min();
    std::valarray<int> neg1(1);
    neg1[0] = -1;
    std::valarray<int> zer(1);
    zer[0] = 0;
    const std::valarray<int> e0 = hi + neg1;
    const std::valarray<int> e1 = hi - zer;
    const std::valarray<int> e2 = lo + zer;
    const std::valarray<int> e3 = lo - neg1;
    const std::valarray<int> e4 = lo - lo;
    const std::valarray<int> e5 = neg1 - lo;
    std::printf("edge %d %d %d %d %d %d\n", e0[0], e1[0], e2[0], e3[0], e4[0], e5[0]);

    std::valarray<unsigned> um(1);
    um[0] = std::numeric_limits<unsigned>::max();
    std::valarray<unsigned> u0(1);
    u0[0] = 0;
    std::valarray<unsigned> u1(1);
    u1[0] = 1;
    const std::valarray<unsigned> us = um + u0;
    const std::valarray<unsigned> ud = um - u1;
    std::printf("uadd %u %u\n", us[0], ud[0]);

    std::valarray<double> da(2);
    da[0] = 1.5;
    da[1] = -0.5;
    std::valarray<double> db(2);
    db[0] = 2.5;
    db[1] = -0.5;
    const std::valarray<double> dsum = da + db;
    const std::valarray<double> ddiff = da - db;
    std::printf("dadd %.1f %.1f %.1f %.1f\n", dsum[0], dsum[1], ddiff[0], ddiff[1]);

    std::valarray<bool> ba(2);
    ba[0] = true;
    ba[1] = false;
    std::valarray<bool> b2(2);
    b2[0] = true;
    b2[1] = true;
    const std::valarray<bool> bsum = ba + b2;
    const std::valarray<bool> bdiff = ba - b2;
    std::printf("badd %d %d %d %d\n", static_cast<int>(bsum[0]), static_cast<int>(bsum[1]),
                static_cast<int>(bdiff[0]), static_cast<int>(bdiff[1]));

    std::valarray<long long> lhi(1);
    lhi[0] = std::numeric_limits<long long>::max();
    std::valarray<long long> llo(1);
    llo[0] = std::numeric_limits<long long>::min();
    std::valarray<long long> ln1(1);
    ln1[0] = -1;
    const std::valarray<long long> l0 = lhi + ln1;
    const std::valarray<long long> l1 = llo - ln1;
    const std::valarray<long long> l2 = llo - llo;
    const std::valarray<long long> l3 = ln1 - llo;
    std::printf("ll %lld %lld %lld %lld\n", l0[0], l1[0], l2[0], l3[0]);

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
    if (arg_is(argc, argv, "addsz")) {
        std::valarray<int> a(2);
        a[0] = 1;
        a[1] = 2;
        std::valarray<int> b(1);
        b[0] = 3;
        const std::valarray<int> r = a + b;
        std::printf("partial=%zu\n", r.size());
        return 2;
    }
    if (arg_is(argc, argv, "subsz")) {
        std::valarray<int> a(1);
        a[0] = 4;
        std::valarray<int> b;
        const std::valarray<int> r = a - b;
        std::printf("partial=%zu\n", r.size());
        return 2;
    }
    if (arg_is(argc, argv, "addovf")) {
        std::valarray<int> a(2);
        a[0] = 1;
        a[1] = std::numeric_limits<int>::max();
        std::valarray<int> b(2);
        b[0] = 1;
        b[1] = 1;
        const std::valarray<int> r = a + b;
        std::printf("partial=%d wrapped=%d\n", r[0], r[1]);
        return 2;
    }
    if (arg_is(argc, argv, "subovf")) {
        std::valarray<int> a(1);
        a[0] = std::numeric_limits<int>::min();
        std::valarray<int> b(1);
        b[0] = 1;
        const std::valarray<int> r = a - b;
        std::printf("wrapped=%d\n", r[0]);
        return 2;
    }
    if (arg_is(argc, argv, "uaddovf")) {
        std::valarray<unsigned> a(1);
        a[0] = std::numeric_limits<unsigned>::max();
        std::valarray<unsigned> b(1);
        b[0] = 1u;
        const std::valarray<unsigned> r = a + b;
        std::printf("wrapped=%u\n", r[0]);
        return 2;
    }
    if (arg_is(argc, argv, "usubovf")) {
        std::valarray<unsigned> a(1);
        a[0] = 0u;
        std::valarray<unsigned> b(1);
        b[0] = 1u;
        const std::valarray<unsigned> r = a - b;
        std::printf("wrapped=%u\n", r[0]);
        return 2;
    }
    if (arg_is(argc, argv, "faddovf")) {
        std::valarray<double> a(1);
        a[0] = std::numeric_limits<double>::max();
        std::valarray<double> b(1);
        b[0] = std::numeric_limits<double>::max();
        const std::valarray<double> r = a + b;
        std::printf("wrapped=%g\n", r[0]);
        return 2;
    }
    if (arg_is(argc, argv, "fsubovf")) {
        std::valarray<double> a(1);
        a[0] = std::numeric_limits<double>::max();
        std::valarray<double> b(1);
        b[0] = std::numeric_limits<double>::lowest();
        const std::valarray<double> r = a - b;
        std::printf("wrapped=%g\n", r[0]);
        return 2;
    }
    return empty.size() == 0 && one.size() == 0 && empty.sum() == 0 && one.sum() == 0 && span.min() == -2 &&
                   span.max() == 9 && alone.min() == 7 && alone.max() == 7 && tmin.id == 3 &&
                   (tmax.id == 1 || tmax.id == 2) && add_empty.size() == 0 && sub_empty.size() == 0 && sm[0] == 11 &&
                   sm[2] == 34 && df[0] == 9 && df[2] == 26 && self_diff[0] == 0 && e4[0] == 0 &&
                   e5[0] == std::numeric_limits<int>::max() &&
                   us[0] == std::numeric_limits<unsigned>::max() && bdiff[0] == false && l2[0] == 0
               ? 0
               : 1;
}
