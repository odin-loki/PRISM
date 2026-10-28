// Differential trace of std::bitset (compiled once against libstdc++ and once
// with PRISM's model header src/prism/pir/models/cxx/bitset first on the
// include path), both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical. operator[] at an index >= N is not called: libstdc++
// reads a padding bit, and the model treats that index as a precondition
// and does not return one. Shifts are part of the trace: a count of 0 is
// unchanged, a count >= N is all zeros, and bits shifted in are 0. AND is
// part of the trace: a result bit is set only when both sides have it, and
// padding in the last word is not a bit of the result.
#include <bitset>
#include <cstdio>
#include <limits>
#include <stdexcept>

static_assert((std::bitset<8>(0xA5ull) << 0) == std::bitset<8>(0xA5ull));
static_assert((std::bitset<8>(0xA5ull) >> 0) == std::bitset<8>(0xA5ull));
static_assert((std::bitset<8>(0xA5ull) << 1) == std::bitset<8>(0x4Aull));
static_assert((std::bitset<8>(0xA5ull) >> 1) == std::bitset<8>(0x52ull));
static_assert((std::bitset<8>(1ull) << 7) == std::bitset<8>(0x80ull));
static_assert((std::bitset<8>(0x80ull) >> 7) == std::bitset<8>(1ull));
static_assert((std::bitset<8>(0xA5ull) << 8).none());
static_assert((std::bitset<8>(0xFFull) >> 9).none());
static_assert((std::bitset<8>(0xFFull) << std::numeric_limits<std::size_t>::max()).none());
static_assert((std::bitset<3>(0x7ull) << 1) == std::bitset<3>(0x6ull));
static_assert(((std::bitset<3>(0x7ull) << 1) >> 1) == std::bitset<3>(0x3ull));
static_assert(((std::bitset<3>(0x7ull) << 1) >> 1).to_ulong() == 3ull);
static_assert((std::bitset<3>(0x7ull) >> 1).count() == 2);
static_assert((std::bitset<0>() << 0) == std::bitset<0>());
static_assert((std::bitset<0>() << 1).none());
static_assert((std::bitset<0>() >> 4).all());
static_assert((std::bitset<0>() << 1).count() == 0);
static_assert((std::bitset<8>(0xA5ull) & std::bitset<8>(0xF0ull)) == std::bitset<8>(0xA0ull));
static_assert((std::bitset<8>(0xA5ull) & std::bitset<8>(0x5Aull)).none());
static_assert((std::bitset<8>(0xA5ull) & std::bitset<8>(0xA5ull)) == std::bitset<8>(0xA5ull));
static_assert((std::bitset<3>(0x7ull) & std::bitset<3>(0x7ull)).to_ulong() == 7ull);
static_assert((std::bitset<3>(0x7ull) & std::bitset<3>(0x7ull)).count() == 3);
static_assert((std::bitset<3>(0xFFull) & std::bitset<3>(0xAAull)).to_ulong() == 2ull);
static_assert((std::bitset<3>(0xFFull) & std::bitset<3>(0xAAull)).count() == 1);
static_assert((std::bitset<0>() & std::bitset<0>()).none());
static_assert((std::bitset<0>() & std::bitset<0>()).all());
static_assert((std::bitset<0>() & std::bitset<0>()).count() == 0);

constexpr bool and_assign_ok() {
    std::bitset<8> a(0xA5ull);
    std::bitset<8> b(0xF0ull);
    a &= b;
    return a == std::bitset<8>(0xA0ull) && b == std::bitset<8>(0xF0ull) && (a & a) == a;
}
static_assert(and_assign_ok());

constexpr bool wide_and_ok() {
    std::bitset<70> a;
    a.set(0);
    a.set(63);
    a.set(69);
    std::bitset<70> b;
    b.set(63);
    b.set(64);
    b.set(69);
    const auto r = a & b;
    return !r.test(0) && r.test(63) && !r.test(64) && r.test(69) && r.count() == 2 && a.test(0) && a.test(63) &&
           a.test(69) && a.count() == 3;
}
static_assert(wide_and_ok());

constexpr bool wide_shift_ok() {
    std::bitset<70> w;
    w.set(0);
    w.set(4);
    w <<= 65;
    if (!w.test(65) || !w.test(69) || w.test(0) || w.count() != 2) return false;
    w >>= 65;
    return w.test(0) && w.test(4) && w.count() == 2;
}
static_assert(wide_shift_ok());

constexpr bool top_bit_shifts_off() {
    std::bitset<70> w;
    w.set(69);
    w <<= 1;
    return w.none() && w.count() == 0;
}
static_assert(top_bit_shifts_off());

constexpr bool word_shift_ok() {
    std::bitset<70> w;
    w.set(0);
    w.set(5);
    w <<= 64;
    if (!w.test(64) || !w.test(69) || w.test(0) || w.count() != 2) return false;
    w >>= 64;
    return w.test(0) && w.test(5) && !w.test(64) && w.count() == 2;
}
static_assert(word_shift_ok());

static void show(const char* what, const std::bitset<8>& b) {
    std::printf("%s size=%zu count=%zu any=%d all=%d none=%d ul=%lu bits", what, b.size(), b.count(), int(b.any()),
                int(b.all()), int(b.none()), b.to_ulong());
    for (std::size_t i = 0; i < b.size(); ++i) std::printf(" %d", int(b.test(i)));
    std::printf("\n");
}

static void ops() {
    std::bitset<8> b;
    show("default", b);
    b.set(0);
    b.set(3, true);
    b.flip(1);
    show("set-flip", b);
    b.reset(0);
    b[2] = true;
    b[5] = b[3];
    show("reset-idx", b);
    b.flip(5);
    show("idx-flip", b);
    b.set();
    show("setall", b);
    b.reset();
    show("resetall", b);
    b.flip();
    show("flipall", b);
    std::bitset<8> u(0xA5ull);
    show("a5", u);
    std::printf("eq %d ne %d\n", int(u == std::bitset<8>(0xA5ull)), int(u == b));
    std::bitset<3> narrow(0xFFull);
    std::printf("narrow ul=%lu count=%zu all=%d\n", narrow.to_ulong(), narrow.count(), int(narrow.all()));
    narrow.flip();
    std::printf("narrow-flip ul=%lu count=%zu none=%d\n", narrow.to_ulong(), narrow.count(), int(narrow.none()));
    narrow.set();
    std::printf("narrow-set ul=%lu all=%d\n", narrow.to_ulong(), int(narrow.all()));
}

static void shifts() {
    std::bitset<8> b(0xA5ull);
    show("shl0", b << 0);
    show("shr0", b >> 0);
    show("shl1", b << 1);
    show("shr1", b >> 1);
    b <<= 0;
    show("shleq0", b);
    b <<= 1;
    show("shleq1", b);
    b >>= 1;
    show("shreq1", b);
    std::bitset<8> cleared(0xA5ull);
    cleared <<= 8;
    show("shl8", cleared);
    cleared = std::bitset<8>(0xFFull);
    cleared >>= 9;
    show("shr9", cleared);
    cleared = std::bitset<8>(0xFFull);
    cleared <<= static_cast<std::size_t>(-1);
    show("shlhuge", cleared);

    std::bitset<3> n(0x7ull);
    n <<= 1;
    std::printf("n3shl ul=%lu count=%zu all=%d\n", n.to_ulong(), n.count(), int(n.all()));
    n >>= 1;
    std::printf("n3back ul=%lu count=%zu\n", n.to_ulong(), n.count());
    std::bitset<3> m(0x7ull);
    m >>= 1;
    std::printf("n3shr ul=%lu count=%zu none=%d\n", m.to_ulong(), m.count(), int(m.none()));

    std::bitset<70> w;
    w.set(0);
    w.set(63);
    w.set(69);
    const auto held = w << 0;
    w <<= 1;
    std::printf("wshl1 count=%zu t0=%d t1=%d t63=%d t64=%d t69=%d held=%d\n", w.count(), int(w.test(0)), int(w.test(1)),
                int(w.test(63)), int(w.test(64)), int(w.test(69)), int(held.test(0) && held.test(63) && held.test(69)));
    w >>= 1;
    std::printf("wback count=%zu t0=%d t62=%d t63=%d t68=%d t69=%d\n", w.count(), int(w.test(0)), int(w.test(62)),
                int(w.test(63)), int(w.test(68)), int(w.test(69)));

    std::bitset<70> across;
    across.set(0);
    across.set(4);
    across <<= 65;
    std::printf("w65 count=%zu t0=%d t64=%d t65=%d t69=%d\n", across.count(), int(across.test(0)), int(across.test(64)),
                int(across.test(65)), int(across.test(69)));
    std::bitset<70> src;
    src.set(0);
    src.set(4);
    std::printf("w65src %d\n", int(src == (src << 0) && (src << 65) == across));

    std::bitset<70> top;
    top.set(69);
    top <<= 1;
    std::printf("woff count=%zu any=%d\n", top.count(), int(top.any()));
    top.set(0);
    top >>= 70;
    std::printf("wge count=%zu any=%d\n", top.count(), int(top.any()));

    std::bitset<70> aligned;
    aligned.set(0);
    aligned.set(5);
    aligned <<= 64;
    std::printf("w64 count=%zu t0=%d t5=%d t64=%d t69=%d\n", aligned.count(), int(aligned.test(0)), int(aligned.test(5)),
                int(aligned.test(64)), int(aligned.test(69)));
    aligned >>= 64;
    std::printf("w64back count=%zu t0=%d t5=%d t64=%d\n", aligned.count(), int(aligned.test(0)), int(aligned.test(5)),
                int(aligned.test(64)));

    try {
        (void)(std::bitset<4>(0xFull) << 1).test(4);
        std::printf("shift-test4 missed\n");
    } catch (const std::out_of_range& e) {
        std::printf("shift-test4: [%s]\n", e.what());
    }
}

static void ands() {
    std::bitset<8> a(0xA5ull);
    std::bitset<8> b(0xF0ull);
    show("and", a & b);
    show("and-lhs", a);
    show("and-rhs", b);
    a &= b;
    show("andeq", a);
    show("andeq-rhs", b);
    show("and0", std::bitset<8>(0xA5ull) & std::bitset<8>());
    std::bitset<8> ones;
    ones.set();
    show("andid", std::bitset<8>(0xA5ull) & ones);
    show("andself", std::bitset<8>(0xA5ull) & std::bitset<8>(0xA5ull));

    std::bitset<3> n(0x7ull);
    n &= std::bitset<3>(0x5ull);
    std::printf("n3and ul=%lu count=%zu all=%d\n", n.to_ulong(), n.count(), int(n.all()));
    std::bitset<3> full;
    full.set();
    full &= full;
    std::printf("n3andall ul=%lu count=%zu all=%d\n", full.to_ulong(), full.count(), int(full.all()));
    const auto masked = std::bitset<3>(0xFFull) & std::bitset<3>(0xAAull);
    std::printf("n3mask ul=%lu count=%zu\n", masked.to_ulong(), masked.count());

    std::bitset<70> w;
    w.set(0);
    w.set(63);
    w.set(69);
    std::bitset<70> v;
    v.set(63);
    v.set(64);
    v.set(69);
    const auto r = w & v;
    std::printf("wand count=%zu t0=%d t63=%d t64=%d t69=%d lhs=%zu\n", r.count(), int(r.test(0)), int(r.test(63)),
                int(r.test(64)), int(r.test(69)), w.count());
    w &= v;
    std::printf("wandeq count=%zu eq=%d rhs=%zu\n", w.count(), int(w == r), v.count());

    std::bitset<96> span;
    span.set();
    const auto kept = span & span;
    std::printf("w96 count=%zu all=%d zero=%d\n", kept.count(), int(kept.all()), int((span & std::bitset<96>()).none()));

    try {
        (void)(std::bitset<4>(0xFull) & std::bitset<4>(0xAu)).test(4);
        std::printf("and-test4 missed\n");
    } catch (const std::out_of_range& e) {
        std::printf("and-test4: [%s]\n", e.what());
    }
}

static void throws() {
    try {
        (void)std::bitset<4>().test(4);
    } catch (const std::out_of_range& e) {
        std::printf("test4: [%s]\n", e.what());
    }
    try {
        (void)std::bitset<0>().test(0);
    } catch (const std::out_of_range& e) {
        std::printf("test0: [%s]\n", e.what());
    }
    try {
        std::bitset<4>().set(4);
    } catch (const std::out_of_range& e) {
        std::printf("set4: [%s]\n", e.what());
    }
    try {
        std::bitset<4>().reset(9);
    } catch (const std::out_of_range& e) {
        std::printf("reset: [%s]\n", e.what());
    }
    try {
        std::bitset<4>().flip(7);
    } catch (const std::out_of_range& e) {
        std::printf("flip: [%s]\n", e.what());
    }
    std::bitset<70> wide;
    wide.set(64);
    try {
        auto v = wide.to_ulong();
        std::printf("to_ulong %lu\n", v);
    } catch (const std::overflow_error& e) {
        std::printf("ovf: [%s]\n", e.what());
    }
    try {
        auto v = wide.to_ullong();
        std::printf("to_ullong %llu\n", v);
    } catch (const std::overflow_error& e) {
        std::printf("ovfull: [%s]\n", e.what());
    }
}

static void empty() {
    std::bitset<0> z;
    std::printf("empty any %d all %d none %d count %zu size %zu ul %lu\n", int(z.any()), int(z.all()), int(z.none()),
                z.count(), z.size(), z.to_ulong());
    z.set();
    z.flip();
    z.reset();
    z <<= 0;
    z >>= 3;
    const auto z2 = z << 1;
    z &= z;
    const auto z3 = z & z;
    std::printf("empty-after any %d\n", int(z.any()));
    std::printf("empty-shift any %d all %d count %zu eq %d\n", int(z2.any()), int(z2.all()), z2.count(), int(z2 == z));
    std::printf("empty-and any %d all %d count %zu eq %d\n", int(z3.any()), int(z3.all()), z3.count(), int(z3 == z));
    std::printf("ul digits %d\n", std::numeric_limits<unsigned long>::digits);
}

int main() {
    ops();
    shifts();
    ands();
    throws();
    empty();
    return 0;
}
