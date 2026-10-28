// Differential trace of <bit> (tests/cpp/test_cxx_bit.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/bit) first on the include path. countl_zero(0)
// is the digit count. popcount of 0xA is 2. A rotate by a multiple of the
// width returns the same bits. byteswap(0) is 0. A one-byte value is
// unchanged. A wider unsigned value reverses its own bytes.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

static_assert(std::byteswap(0u) == 0u);
static_assert(std::byteswap(static_cast<unsigned char>(0x7F)) == static_cast<unsigned char>(0x7F));
static_assert(std::byteswap(static_cast<std::uint8_t>(0)) == static_cast<std::uint8_t>(0));
static_assert(std::byteswap(static_cast<std::uint8_t>(0xAB)) == static_cast<std::uint8_t>(0xAB));
static_assert(std::byteswap(static_cast<std::uint16_t>(0)) == static_cast<std::uint16_t>(0));
static_assert(std::byteswap(static_cast<std::uint16_t>(0x1234)) == static_cast<std::uint16_t>(0x3412));
static_assert(std::byteswap(static_cast<std::uint32_t>(0)) == static_cast<std::uint32_t>(0));
static_assert(std::byteswap(static_cast<std::uint32_t>(1)) == static_cast<std::uint32_t>(0x01000000u));
static_assert(std::byteswap(static_cast<std::uint32_t>(0x12345678u)) == static_cast<std::uint32_t>(0x78563412u));
static_assert(std::byteswap(static_cast<std::uint64_t>(0)) == static_cast<std::uint64_t>(0));
static_assert(std::byteswap(static_cast<std::uint64_t>(0x0123456789ABCDEFULL)) ==
              static_cast<std::uint64_t>(0xEFCDAB8967452301ULL));
static_assert(std::byteswap(std::byteswap(static_cast<std::uint32_t>(0xA5A5A5A5u))) ==
              static_cast<std::uint32_t>(0xA5A5A5A5u));
static_assert(std::byteswap(std::byteswap(static_cast<std::uint64_t>(0x0123456789ABCDEFULL))) ==
              static_cast<std::uint64_t>(0x0123456789ABCDEFULL));

// Native stand-in for the PIR intrinsic. The overflow path of bit_ceil aborts
// here. Values that fit do not call it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

template <typename T>
int counts_ok() {
    constexpr int d = std::numeric_limits<T>::digits;
    if (std::countl_zero(T{0}) != d) return 0;
    if (std::countr_zero(T{0}) != d) return 0;
    if (std::countl_one(static_cast<T>(~T{0})) != d) return 0;
    if (std::countr_one(static_cast<T>(~T{0})) != d) return 0;
    if (std::countl_zero(T{1}) != d - 1) return 0;
    if (std::popcount(T{0}) != 0) return 0;
    if (std::popcount(static_cast<T>(0xA)) != 2) return 0;
    if (std::countr_one(T{1}) != 1) return 0;
    if (std::countr_one(T{3}) != 2) return 0;
    if (std::bit_ceil(T{0}) != T{1}) return 0;
    if (std::bit_ceil(T{1}) != T{1}) return 0;
    if (std::bit_ceil(T{3}) != T{4}) return 0;
    if (std::bit_floor(T{0}) != T{0}) return 0;
    if (std::bit_floor(T{5}) != T{4}) return 0;
    if (std::bit_width(T{0}) != 0) return 0;
    if (std::bit_width(T{5}) != 3) return 0;
    if (std::has_single_bit(T{0})) return 0;
    if (!std::has_single_bit(T{1})) return 0;
    if (std::has_single_bit(T{3})) return 0;
    if (!std::has_single_bit(T{8})) return 0;
    return 1;
}

template <typename T>
int round_ok(T v) {
    constexpr int d = std::numeric_limits<T>::digits;
    if (std::rotl(v, 0) != v || std::rotr(v, 0) != v) return 0;
    if (std::rotl(v, d) != v || std::rotr(v, d) != v) return 0;
    if (std::rotl(v, -d) != v || std::rotr(v, -d) != v) return 0;
    const int bits = std::popcount(v);
    for (int k = -d * 2; k <= d * 2; ++k) {
        if (std::rotr(std::rotl(v, k), k) != v) return 0;
        if (std::rotl(std::rotr(v, k), k) != v) return 0;
        if (std::popcount(std::rotl(v, k)) != bits) return 0;
        if (std::popcount(std::rotr(v, k)) != bits) return 0;
    }
    return 1;
}

template <typename T>
void dump(const char* name, T x) {
    const int d = std::numeric_limits<T>::digits;
    const auto u = [](T v) { return static_cast<unsigned long long>(v); };
    std::printf("%s clz0 %d digits %d ctz0 %d clo1 %d popA %d pop0 %d\n", name, std::countl_zero(T{0}), d,
                std::countr_zero(T{0}), std::countl_one(static_cast<T>(~T{0})), std::popcount(static_cast<T>(0xA)),
                std::popcount(T{0}));
    std::printf("%s rotl1 %llu rotr1 %llu rotl0 %llu rotlw %llu rotlm1 %llu\n", name, u(std::rotl(x, 1)),
                u(std::rotr(x, 1)), u(std::rotl(x, 0)), u(std::rotl(x, d)), u(std::rotl(x, -1)));
    std::printf("%s ceil0 %llu ceil1 %llu ceil3 %llu floor0 %llu floor5 %llu width0 %d width5 %d s1 %d s0 %d s3 %d\n",
                name, u(std::bit_ceil(T{0})), u(std::bit_ceil(T{1})), u(std::bit_ceil(T{3})), u(std::bit_floor(T{0})),
                u(std::bit_floor(T{5})), std::bit_width(T{0}), std::bit_width(T{5}), int(std::has_single_bit(T{1})),
                int(std::has_single_bit(T{0})), int(std::has_single_bit(T{3})));
}

int main() {
    const int digits = std::numeric_limits<unsigned>::digits;
    std::printf("unsigned clz0 %d digits %d\n", std::countl_zero(0u), digits);
    std::printf("unsigned popA %d\n", std::popcount(0xAu));
    dump("unsigned", 0xA5A5A5A5u);
    dump("u8", static_cast<std::uint8_t>(0xA5));
    dump("u64", static_cast<std::uint64_t>(0x0123456789ABCDEFULL));
    const int cu = counts_ok<unsigned>();
    const int c8 = counts_ok<std::uint8_t>();
    const int c64 = counts_ok<std::uint64_t>();
    const int ru = round_ok(0xA5A5A5A5u);
    const int r8 = round_ok(static_cast<std::uint8_t>(0xA5));
    const int r64 = round_ok(static_cast<std::uint64_t>(0x0123456789ABCDEFULL));
    std::printf("counts %d %d %d\n", cu, c8, c64);
    std::printf("round %d %d %d\n", ru, r8, r64);
    std::printf("endian %d %d %d\n", static_cast<int>(std::endian::little), static_cast<int>(std::endian::big),
                static_cast<int>(std::endian::native));
    // Runtime values, so the byte moves are executed and not only folded.
    volatile std::uint8_t b8 = 0xAB;
    volatile std::uint16_t b16 = 0x1234;
    volatile std::uint32_t b32 = 0x12345678u;
    volatile std::uint32_t b1 = 1;
    volatile std::uint64_t b64 = 0x0123456789ABCDEFULL;
    volatile std::uint32_t z32 = 0;
    std::printf("swap0 %x\n", static_cast<unsigned>(std::byteswap(z32)));
    std::printf("swap8 %x\n", static_cast<unsigned>(std::byteswap(b8)));
    std::printf("swap16 %x\n", static_cast<unsigned>(std::byteswap(b16)));
    std::printf("swap32 %x\n", static_cast<unsigned>(std::byteswap(b32)));
    std::printf("swap1 %x\n", static_cast<unsigned>(std::byteswap(b1)));
    std::printf("swap64 %llx\n", static_cast<unsigned long long>(std::byteswap(b64)));
    const int rt32 = std::byteswap(std::byteswap(b32)) == b32;
    const int rt8 = std::byteswap(std::byteswap(b8)) == b8;
    const int rt16 = std::byteswap(std::byteswap(b16)) == b16;
    const int rt64 = std::byteswap(std::byteswap(b64)) == b64;
    std::printf("swaprt %d %d %d %d\n", rt32, rt8, rt16, rt64);
    if (std::byteswap(z32) != 0u) return 1;
    if (std::byteswap(b8) != static_cast<std::uint8_t>(0xAB)) return 1;
    if (std::byteswap(b16) != static_cast<std::uint16_t>(0x3412)) return 1;
    if (std::byteswap(b32) != static_cast<std::uint32_t>(0x78563412u)) return 1;
    if (std::byteswap(b1) != static_cast<std::uint32_t>(0x01000000u)) return 1;
    if (std::byteswap(b64) != static_cast<std::uint64_t>(0xEFCDAB8967452301ULL)) return 1;
    if (rt32 != 1 || rt8 != 1 || rt16 != 1 || rt64 != 1) return 1;
    if (std::countl_zero(0u) != digits) return 1;
    if (std::popcount(0xAu) != 2) return 1;
    if (cu != 1 || c8 != 1 || c64 != 1) return 1;
    if (ru != 1 || r8 != 1 || r64 != 1) return 1;
    return 0;
}
