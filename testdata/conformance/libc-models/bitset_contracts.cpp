// std::bitset operational model (src/prism/pir/models/cxx/bitset) against
// C++23 [bitset.members]. The pir stage puts the model headers first on the
// include path, so <bitset> below is the model (the report's
// `extra.cxx_models` says "model: bitset"); see docs/PIR.md "C++ library
// models". Each function is one contract; `_false` ones must fail for the
// class their yml names. The yml records that contract. It is not a proof.
#include <bitset>
#include <cassert>
#include <limits>
#include <stdexcept>

// set / reset / flip / test / operator[] / count / size / all / any / none /
// to_ulong on a value that fits in unsigned long. Shifts are bitset_shift_true.
// AND is bitset_and_true.
int bitset_ops_true(int x) {
    std::bitset<8> b;
    assert(b.size() == 8);
    assert(b.none());
    assert(!b.any());
    assert(!b.all());
    assert(b.count() == 0);
    b.set(0);
    b.set(3, true);
    b.flip(1);
    assert(b.test(0));
    assert(b.test(1));
    assert(!b.test(2));
    assert(b[3]);
    b.reset(0);
    assert(!b.test(0));
    b[2] = true;
    assert(b.test(2));
    b.set();
    assert(b.all());
    assert(b.count() == 8);
    b.reset();
    assert(b.none());
    b.flip();
    assert(b.all());
    std::bitset<8> u(0x5u);
    assert(u.to_ulong() == 5u);
    assert(u.count() == 2);
    assert(u == std::bitset<8>(5u));
    (void)x;
    return 0;
}

// bitset<0> stores nothing: any() is false, and test(0) throws.
int bitset_empty_true(int) {
    std::bitset<0> b;
    assert(b.size() == 0);
    assert(!b.any());
    assert(b.none());
    assert(b.all());
    assert(b.count() == 0);
    assert(b.to_ulong() == 0u);
    b.set();
    b.flip();
    b.reset();
    b <<= 0;
    b >>= 1;
    assert((b << 4) == b);
    assert((b >> 0) == b);
    b &= b;
    assert((b & b) == b);
    assert(!b.any());
    assert(b.all());
    assert(b.count() == 0);
    assert(b.to_ulong() == 0u);
    bool threw = false;
    try {
        (void)b.test(0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
    return 0;
}

// Shifts. A count of 0 leaves the bits unchanged. A count >= N yields every
// bit 0. Bits shifted in are 0. The original is unchanged by << and >>.
// test() at N still throws after a shift; it does not read a padding bit.
int bitset_shift_true(int) {
    std::bitset<8> b(0xA5u);
    assert((b << 0) == b);
    assert((b >> 0) == b);
    b <<= 0;
    assert(b == std::bitset<8>(0xA5u));
    assert((b << 1) == std::bitset<8>(0x4Au));
    assert((b >> 1) == std::bitset<8>(0x52u));
    assert(b == std::bitset<8>(0xA5u));
    assert((std::bitset<8>(1u) << 7) == std::bitset<8>(0x80u));
    assert((std::bitset<8>(0x80u) >> 7) == std::bitset<8>(1u));
    b <<= 8;
    assert(b.none());
    b = std::bitset<8>(0xFFu);
    b >>= 9;
    assert(b.none());
    b = std::bitset<8>(0xFFu);
    b <<= std::numeric_limits<std::size_t>::max();
    assert(b.none());
    assert((std::bitset<8>(0xA5u) >> std::numeric_limits<std::size_t>::max()).none());

    std::bitset<3> n(0x7u);
    n <<= 1;
    assert(n == std::bitset<3>(0x6u));
    assert(n.to_ulong() == 6u);
    assert(n.count() == 2);
    assert(!n.test(0));
    assert(n.test(1));
    assert(n.test(2));
    n >>= 1;
    assert(n == std::bitset<3>(0x3u));
    assert(n.to_ulong() == 3u);
    assert(n.count() == 2);
    assert((std::bitset<3>(0x7u) >> 1) == std::bitset<3>(0x3u));

    std::bitset<4> filled;
    filled.set();
    filled <<= 1;
    assert(!filled.all());
    assert(filled.count() == 3);
    assert(!filled.test(0));
    assert(filled.test(3));

    std::bitset<70> w;
    w.set(0);
    w.set(63);
    w.set(69);
    const auto held = w;
    w <<= 1;
    assert(!w.test(0));
    assert(w.test(1));
    assert(!w.test(63));
    assert(w.test(64));
    assert(!w.test(69));
    assert(w.count() == 2);
    assert(held.test(0) && held.test(63) && held.test(69));
    assert((held << 1) == w);
    w >>= 1;
    // Bit 69 was shifted off the top, so the round trip does not restore it.
    assert(w.test(0));
    assert(w.test(63));
    assert(!w.test(64));
    assert(!w.test(69));
    assert(w.count() == 2);

    std::bitset<70> across;
    across.set(0);
    across.set(4);
    const auto across_src = across;
    across <<= 65;
    assert(!across.test(0));
    assert(across.test(65));
    assert(across.test(69));
    assert(across.count() == 2);
    assert(across_src.test(0) && across_src.test(4));
    across >>= 65;
    assert(across == across_src);

    std::bitset<70> words;
    words.set(0);
    words.set(31);
    words <<= 32;
    assert(words.test(32));
    assert(words.test(63));
    assert(!words.test(0));
    assert(words.count() == 2);
    words >>= 32;
    assert(words.test(0));
    assert(words.test(31));
    assert(words.count() == 2);

    std::bitset<70> aligned;
    aligned.set(0);
    aligned.set(5);
    aligned <<= 64;
    assert(aligned.test(64));
    assert(aligned.test(69));
    assert(!aligned.test(0));
    assert(!aligned.test(5));
    assert(aligned.count() == 2);
    aligned >>= 64;
    assert(aligned.test(0));
    assert(aligned.test(5));
    assert(!aligned.test(64));
    assert(aligned.count() == 2);

    std::bitset<70> top;
    top.set(69);
    top <<= 1;
    assert(top.none());
    top.set(0);
    top >>= 70;
    assert(top.none());

    bool threw = false;
    try {
        (void)(std::bitset<4>(0xFu) << 1).test(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
    return 0;
}

// AND. Each result bit is set only when both operands have that bit set.
// operator& leaves both operands unchanged. Padding in the last word is not
// a bit: bitset<3> of all ones stays to_ulong() == 7 and count() == 3.
// test() at N still throws after an AND; it does not read a padding bit.
int bitset_and_true(int) {
    std::bitset<8> a(0xA5u);
    std::bitset<8> b(0xF0u);
    assert((a & b) == std::bitset<8>(0xA0u));
    assert(a == std::bitset<8>(0xA5u));
    assert(b == std::bitset<8>(0xF0u));
    assert((a & a) == a);
    assert((a & std::bitset<8>()).none());
    std::bitset<8> ones;
    ones.set();
    assert((a & ones) == a);
    assert((b & a) == (a & b));
    a &= b;
    assert(a == std::bitset<8>(0xA0u));
    assert(b == std::bitset<8>(0xF0u));
    a &= a;
    assert(a == std::bitset<8>(0xA0u));

    std::bitset<8> lhs;
    lhs.set(0);
    lhs.set(2);
    lhs.set(7);
    std::bitset<8> rhs;
    rhs.set(2);
    rhs.set(3);
    rhs.set(7);
    const auto both = lhs & rhs;
    assert(both.test(2));
    assert(both.test(7));
    assert(!both.test(0));
    assert(!both.test(3));
    assert(both.count() == 2);
    assert(lhs.test(0) && lhs.test(2) && lhs.test(7) && lhs.count() == 3);

    std::bitset<3> n(0x7u);
    n &= std::bitset<3>(0x5u);
    assert(n == std::bitset<3>(0x5u));
    assert(n.to_ulong() == 5u);
    assert(n.count() == 2);
    assert(!n.test(1));
    std::bitset<3> wide_in(0xFFu);
    std::bitset<3> mask(0xAAu);
    const auto narrow = wide_in & mask;
    assert(narrow == std::bitset<3>(0x2u));
    assert(narrow.to_ulong() == 2u);
    assert(narrow.count() == 1);
    std::bitset<3> filled;
    filled.set();
    filled &= filled;
    assert(filled.to_ulong() == 7u);
    assert(filled.count() == 3);
    assert(filled.all());
    assert((filled & std::bitset<3>()).none());
    assert((filled & std::bitset<3>()).count() == 0);

    std::bitset<70> w;
    w.set(0);
    w.set(63);
    w.set(69);
    std::bitset<70> v;
    v.set(63);
    v.set(64);
    v.set(69);
    const auto held = w;
    const auto r = w & v;
    assert(!r.test(0));
    assert(r.test(63));
    assert(!r.test(64));
    assert(r.test(69));
    assert(r.count() == 2);
    assert(w == held);
    assert(w.count() == 3);
    w &= v;
    assert(w == r);
    assert(v.test(63) && v.test(64) && v.test(69) && v.count() == 3);

    std::bitset<96> full;
    full.set();
    assert((full & full).count() == 96);
    assert((full & full).all());
    assert((full & std::bitset<96>()).none());

    std::bitset<0> z;
    z &= z;
    assert((z & z) == z);
    assert(!z.any());
    assert(z.all());
    assert(z.count() == 0);
    assert(z.to_ulong() == 0u);

    bool threw = false;
    try {
        (void)(std::bitset<4>(0xFu) & std::bitset<4>(0xAu)).test(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
    return 0;
}

// operator[] at size(): past the last bit (precondition __position < size()).
int bitset_index_oob_false(int n) {
    if (n < 0 || n > 3) return 0;
    std::bitset<4> b;
    b.set(static_cast<std::size_t>(n));
    return b[b.size()];
}

// test() at N throws std::out_of_range. It does not return a bit. The throw
// leaves this noexcept function.
int bitset_test_oob_false(int) noexcept {
    std::bitset<4> b;
    return b.test(4);
}

// A bit above the width of unsigned long does not fit in to_ulong().
int bitset_to_ulong_overflow_false(int) noexcept {
    constexpr std::size_t n = std::numeric_limits<unsigned long>::digits + 1;
    std::bitset<n> b;
    b.set(std::numeric_limits<unsigned long>::digits);
    return static_cast<int>(b.to_ulong());
}
