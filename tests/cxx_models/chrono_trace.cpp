// Trace of the partial <chrono> model (src/prism/pir/models/cxx/chrono).
// Compile with that directory as -isystem so this include is the model.
// Clocks and sleep are not called: they are not declared. A same-period
// duration_cast keeps the count. A different period is not named: that
// cast is not declared. Same-type += and -= update the stored count.
// `ovf` adds the maximum signed 64-bit count to 1. `plus_eq_ovf` and
// `minus_eq_ovf` do the same through compound assignment. The model must
// __prism_check and must not print a wrapped negative count. Same-type
// comparisons order the stored counts. They do not convert a period, do
// not add, and do not sleep. A different period is not named.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using dur = std::chrono::duration<long long>;

// Same type, stored counts. No period conversion and no addition.
static_assert(dur(2) == dur(2));
static_assert(!(dur(2) == dur(3)));
static_assert(dur(2) != dur(3));
static_assert(!(dur(2) != dur(2)));
static_assert(dur(2) < dur(3));
static_assert(!(dur(3) < dur(2)));
static_assert(!(dur(2) < dur(2)));
static_assert(dur(3) > dur(2));
static_assert(!(dur(2) > dur(3)));
static_assert(!(dur(2) > dur(2)));
static_assert(dur(2) <= dur(2));
static_assert(dur(2) <= dur(3));
static_assert(!(dur(3) <= dur(2)));
static_assert(dur(2) >= dur(2));
static_assert(dur(3) >= dur(2));
static_assert(!(dur(2) >= dur(3)));
static_assert(dur{} == dur(0));
static_assert(dur(-1) < dur(0));
static_assert(!(dur(-1) == dur(0)));
static_assert(dur(9223372036854775807LL) > dur(9223372036854775806LL));
static_assert(dur(9223372036854775807LL) == dur(9223372036854775807LL));
static_assert(!(dur(9223372036854775807LL) < dur(9223372036854775807LL)));
static_assert(dur(-9223372036854775807LL - 1) < dur(0));
static_assert(dur(-9223372036854775807LL - 1) < dur(9223372036854775807LL));

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned count. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    std::chrono::duration<long long> zero{};
    std::chrono::duration<long long> a(2);
    std::chrono::duration<long long> b(3);
    std::printf("zero=%lld\n", static_cast<long long>(zero.count()));
    std::printf("sum=%lld\n", static_cast<long long>((a + b).count()));
    std::chrono::duration<long long> a2(2);
    std::chrono::duration<long long> neg(-1);
    std::chrono::duration<long long> hi(9223372036854775807LL);
    std::chrono::duration<long long> almost(9223372036854775806LL);
    std::chrono::duration<long long> lo(-9223372036854775807LL - 1);
    std::printf("eq=%d\n", (a == b) ? 1 : 0);
    std::printf("ne=%d\n", (a != b) ? 1 : 0);
    std::printf("lt=%d\n", (a < b) ? 1 : 0);
    std::printf("gt=%d\n", (a > b) ? 1 : 0);
    std::printf("le=%d\n", (a <= b) ? 1 : 0);
    std::printf("ge=%d\n", (a >= b) ? 1 : 0);
    std::printf("eq_self=%d\n", (a == a) ? 1 : 0);
    std::printf("eq_same=%d\n", (a == a2) ? 1 : 0);
    std::printf("lt_same=%d\n", (a < a2) ? 1 : 0);
    std::printf("le_same=%d\n", (a <= a2) ? 1 : 0);
    std::printf("ge_same=%d\n", (a >= a2) ? 1 : 0);
    std::printf("zero_eq=%d\n", (zero == zero) ? 1 : 0);
    std::printf("zero_lt=%d\n", (zero < a) ? 1 : 0);
    std::printf("neg_lt_zero=%d\n", (neg < zero) ? 1 : 0);
    std::printf("neg_eq_zero=%d\n", (neg == zero) ? 1 : 0);
    std::printf("max_gt=%d\n", (hi > almost) ? 1 : 0);
    std::printf("max_eq=%d\n", (hi == hi) ? 1 : 0);
    std::printf("max_lt_self=%d\n", (hi < hi) ? 1 : 0);
    std::printf("min_lt_zero=%d\n", (lo < zero) ? 1 : 0);
    std::printf("min_lt_max=%d\n", (lo < hi) ? 1 : 0);
    const auto same = std::chrono::duration_cast<std::chrono::duration<long long>>(a);
    std::printf("cast=%lld\n", static_cast<long long>(same.count()));
    std::chrono::duration<long long> d(5);
    std::chrono::duration<long long> e(1);
    std::printf("diff=%lld\n", static_cast<long long>((d - e).count()));
    std::printf("zero_sum=%lld\n", static_cast<long long>((zero + zero).count()));
    std::printf("plus_eq_zero=%lld\n", static_cast<long long>((zero += zero).count()));
    std::printf("plus_eq=%lld\n", static_cast<long long>((a += b).count()));
    std::printf("minus_eq=%lld\n", static_cast<long long>((d -= e).count()));
    if (argc > 1 && std::strcmp(argv[1], "ovf") == 0) {
        std::chrono::duration<long long> hi(9223372036854775807LL);
        std::chrono::duration<long long> one(1);
        const auto n = (hi + one).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "plus_eq_ovf") == 0) {
        std::chrono::duration<long long> hi(9223372036854775807LL);
        hi += std::chrono::duration<long long>(1);
        std::printf("wrapped=%lld\n", static_cast<long long>(hi.count()));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "minus_eq_ovf") == 0) {
        std::chrono::duration<long long> lo(-9223372036854775807LL - 1);
        lo -= std::chrono::duration<long long>(1);
        std::printf("wrapped=%lld\n", static_cast<long long>(lo.count()));
        return 0;
    }
    return 0;
}
