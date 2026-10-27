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
// to_ulong on a value that fits in unsigned long.
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
    assert(!b.any());
    bool threw = false;
    try {
        (void)b.test(0);
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
