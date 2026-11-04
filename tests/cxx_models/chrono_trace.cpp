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
// duration::zero, duration::min, and duration::max are the count 0 and the
// most negative and most positive counts. A same-rep product that fits is
// that product. Zero times any count is zero. `mul_ovf` multiplies the
// maximum count by 2. `mul_min_ovf` multiplies the minimum count by -1.
// The model must __prism_check and must not print a wrapped count.
// Unary operator- returns the negated count. Zero stays zero. `neg_ovf`
// negates the minimum count. The model must __prism_check and must not
// print a wrapped count. That path does not return a zero duration.
// A same-rep quotient that fits is the stored count divided by the scalar.
// `div_zero` and `div_min_zero` divide by zero. `div_ovf` divides the
// minimum count by -1. The model must __prism_check and must not print a
// quotient. Those paths do not return a zero duration.
// std::chrono::abs returns the magnitude. Zero stays zero. A positive
// count stays that count. A negative count that is not the minimum is the
// negation. `abs_ovf` takes the minimum count. The model must
// __prism_check and must not print a wrapped count. That path does not
// return a zero duration and does not form the negation.
// A same-rep remainder that fits is the stored count modulo the scalar.
// `mod_zero` and `mod_min_zero` take the remainder by zero. `mod_ovf`
// takes the minimum count modulo -1. The model must __prism_check and
// must not print a remainder. Those paths do not return a zero duration
// and do not form the remainder.
// duration::operator%= assigns that remainder. `mod_eq_zero` and
// `mod_eq_min_zero` take the remainder by zero through %=. `mod_eq_ovf`
// takes the minimum count modulo -1 through %=. Those traps stay inside
// operator%, so %= does not store a duration and does not print a count.
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
static_assert(dur::zero().count() == 0);
static_assert(dur::min().count() == -9223372036854775807LL - 1);
static_assert(dur::max().count() == 9223372036854775807LL);
static_assert(dur::min().count() != 0);
static_assert(dur::max().count() != 0);
static_assert(dur::min().count() < dur::zero().count());
static_assert(dur::zero().count() < dur::max().count());
static_assert(dur::min() < dur::max());
static_assert(dur::zero() == dur(0));
static_assert(!(dur::min() == dur::zero()));
static_assert(!(dur::max() == dur::zero()));

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
    const auto lim0 = dur::zero();
    const auto lim_min = dur::min();
    const auto lim_max = dur::max();
    std::printf("lim_zero=%lld\n", static_cast<long long>(lim0.count()));
    std::printf("lim_min=%lld\n", static_cast<long long>(lim_min.count()));
    std::printf("lim_max=%lld\n", static_cast<long long>(lim_max.count()));
    std::printf("mul=%lld\n", static_cast<long long>((dur(2) * 3LL).count()));
    std::printf("mul_left=%lld\n", static_cast<long long>((3LL * dur(2)).count()));
    std::printf("mul_neg=%lld\n", static_cast<long long>((dur(-2) * 3LL).count()));
    std::printf("mul_neg2=%lld\n", static_cast<long long>((dur(-2) * -3LL).count()));
    std::printf("mul_zero_max=%lld\n", static_cast<long long>((lim0 * lim_max.count()).count()));
    std::printf("mul_zero_min=%lld\n", static_cast<long long>((lim0 * lim_min.count()).count()));
    std::printf("mul_max_zero=%lld\n", static_cast<long long>((lim_max * 0LL).count()));
    std::printf("mul_min_zero=%lld\n", static_cast<long long>((lim_min * 0LL).count()));
    std::printf("mul_left_zero=%lld\n", static_cast<long long>((0LL * lim_min).count()));
    std::printf("mul_max_one=%lld\n", static_cast<long long>((lim_max * 1LL).count()));
    std::printf("mul_max_neg1=%lld\n", static_cast<long long>((lim_max * -1LL).count()));
    std::printf("mul_min_one=%lld\n", static_cast<long long>((lim_min * 1LL).count()));
    std::printf("mul_min_left=%lld\n", static_cast<long long>((1LL * lim_min).count()));
    std::printf("mul_near=%lld\n", static_cast<long long>((dur(4611686018427387903LL) * 2LL).count()));
    std::printf("mul_edge=%lld\n", static_cast<long long>((dur(-4611686018427387904LL) * 2LL).count()));
    std::printf("mul_edge_left=%lld\n", static_cast<long long>((2LL * dur(-4611686018427387904LL)).count()));
    std::printf("neg_zero=%lld\n", static_cast<long long>((-dur(0)).count()));
    std::printf("neg_pos=%lld\n", static_cast<long long>((-dur(5)).count()));
    std::printf("neg_neg=%lld\n", static_cast<long long>((-dur(-5)).count()));
    std::printf("neg_max=%lld\n", static_cast<long long>((-dur::max()).count()));
    std::printf("neg_min1=%lld\n", static_cast<long long>((-dur(-9223372036854775807LL)).count()));
    std::printf("div=%lld\n", static_cast<long long>((dur(10) / 2LL).count()));
    std::printf("div_trunc=%lld\n", static_cast<long long>((dur(5) / 2LL).count()));
    std::printf("div_neg=%lld\n", static_cast<long long>((dur(-5) / 2LL).count()));
    std::printf("div_neg_s=%lld\n", static_cast<long long>((dur(5) / -2LL).count()));
    std::printf("div_neg2=%lld\n", static_cast<long long>((dur(-5) / -2LL).count()));
    std::printf("div_zero_count=%lld\n", static_cast<long long>((dur(0) / 5LL).count()));
    std::printf("div_zero_neg1=%lld\n", static_cast<long long>((dur(0) / -1LL).count()));
    std::printf("div_max_one=%lld\n", static_cast<long long>((lim_max / 1LL).count()));
    std::printf("div_max_neg1=%lld\n", static_cast<long long>((lim_max / -1LL).count()));
    std::printf("div_min_one=%lld\n", static_cast<long long>((lim_min / 1LL).count()));
    std::printf("div_min_two=%lld\n", static_cast<long long>((lim_min / 2LL).count()));
    std::printf("div_min_neg2=%lld\n", static_cast<long long>((lim_min / -2LL).count()));
    std::printf("div_one_two=%lld\n", static_cast<long long>((dur(1) / 2LL).count()));
    std::printf("div_m1_two=%lld\n", static_cast<long long>((dur(-1) / 2LL).count()));
    std::printf("div_max_max=%lld\n", static_cast<long long>((lim_max / lim_max.count()).count()));
    std::printf("div_min_min=%lld\n", static_cast<long long>((lim_min / lim_min.count()).count()));
    std::printf("div_max_min=%lld\n", static_cast<long long>((lim_max / lim_min.count()).count()));
    std::printf("div_min_max=%lld\n", static_cast<long long>((lim_min / lim_max.count()).count()));
    std::printf("abs_zero=%lld\n", static_cast<long long>(std::chrono::abs(dur(0)).count()));
    std::printf("abs_pos=%lld\n", static_cast<long long>(std::chrono::abs(dur(5)).count()));
    std::printf("abs_neg=%lld\n", static_cast<long long>(std::chrono::abs(dur(-5)).count()));
    std::printf("abs_one=%lld\n", static_cast<long long>(std::chrono::abs(dur(1)).count()));
    std::printf("abs_m1=%lld\n", static_cast<long long>(std::chrono::abs(dur(-1)).count()));
    std::printf("abs_max=%lld\n", static_cast<long long>(std::chrono::abs(dur::max()).count()));
    std::printf("abs_min1=%lld\n",
                static_cast<long long>(std::chrono::abs(dur(-9223372036854775807LL)).count()));
    std::printf("mod=%lld\n", static_cast<long long>((dur(10) % 2LL).count()));
    std::printf("mod_rem=%lld\n", static_cast<long long>((dur(10) % 3LL).count()));
    std::printf("mod_trunc=%lld\n", static_cast<long long>((dur(5) % 2LL).count()));
    std::printf("mod_neg=%lld\n", static_cast<long long>((dur(-5) % 2LL).count()));
    std::printf("mod_neg_s=%lld\n", static_cast<long long>((dur(5) % -2LL).count()));
    std::printf("mod_neg2=%lld\n", static_cast<long long>((dur(-5) % -2LL).count()));
    std::printf("mod_zero_count=%lld\n", static_cast<long long>((dur(0) % 5LL).count()));
    std::printf("mod_zero_neg1=%lld\n", static_cast<long long>((dur(0) % -1LL).count()));
    std::printf("mod_max_one=%lld\n", static_cast<long long>((lim_max % 1LL).count()));
    std::printf("mod_max_neg1=%lld\n", static_cast<long long>((lim_max % -1LL).count()));
    std::printf("mod_min_one=%lld\n", static_cast<long long>((lim_min % 1LL).count()));
    std::printf("mod_min_two=%lld\n", static_cast<long long>((lim_min % 2LL).count()));
    std::printf("mod_min_neg2=%lld\n", static_cast<long long>((lim_min % -2LL).count()));
    std::printf("mod_one_two=%lld\n", static_cast<long long>((dur(1) % 2LL).count()));
    std::printf("mod_m1_two=%lld\n", static_cast<long long>((dur(-1) % 2LL).count()));
    std::printf("mod_max_max=%lld\n", static_cast<long long>((lim_max % lim_max.count()).count()));
    std::printf("mod_min_min=%lld\n", static_cast<long long>((lim_min % lim_min.count()).count()));
    std::printf("mod_max_min=%lld\n", static_cast<long long>((lim_max % lim_min.count()).count()));
    std::printf("mod_min_max=%lld\n", static_cast<long long>((lim_min % lim_max.count()).count()));
    dur mod_eq(10);
    mod_eq %= 3LL;
    std::printf("mod_eq=%lld\n", static_cast<long long>(mod_eq.count()));
    dur mod_eq_neg(-5);
    mod_eq_neg %= 2LL;
    std::printf("mod_eq_neg=%lld\n", static_cast<long long>(mod_eq_neg.count()));
    dur mod_eq_pos(5);
    mod_eq_pos %= -2LL;
    std::printf("mod_eq_neg_s=%lld\n", static_cast<long long>(mod_eq_pos.count()));
    dur mod_eq_zero_count(0);
    mod_eq_zero_count %= 5LL;
    std::printf("mod_eq_zero_count=%lld\n", static_cast<long long>(mod_eq_zero_count.count()));
    dur mod_eq_max(lim_max.count());
    mod_eq_max %= -1LL;
    std::printf("mod_eq_max_neg1=%lld\n", static_cast<long long>(mod_eq_max.count()));
    dur mod_eq_min(lim_min.count());
    mod_eq_min %= -2LL;
    std::printf("mod_eq_min_neg2=%lld\n", static_cast<long long>(mod_eq_min.count()));
    dur mod_eq_chain(10);
    (mod_eq_chain %= 3LL) %= 2LL;
    std::printf("mod_eq_chain=%lld\n", static_cast<long long>(mod_eq_chain.count()));
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
    if (argc > 1 && std::strcmp(argv[1], "mul_ovf") == 0) {
        const auto n = (std::chrono::duration<long long>::max() * 2LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul_left_ovf") == 0) {
        const auto n = (2LL * std::chrono::duration<long long>::max()).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul_min_ovf") == 0) {
        const auto n = (std::chrono::duration<long long>::min() * -1LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul_min_left_ovf") == 0) {
        const auto n = (-1LL * std::chrono::duration<long long>::min()).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neg_ovf") == 0) {
        const auto n = (-std::chrono::duration<long long>::min()).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div_zero") == 0) {
        const auto n = (std::chrono::duration<long long>(5) / 0LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div_min_zero") == 0) {
        const auto n = (std::chrono::duration<long long>::min() / 0LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div_ovf") == 0) {
        const auto n = (std::chrono::duration<long long>::min() / -1LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "abs_ovf") == 0) {
        const auto n = std::chrono::abs(std::chrono::duration<long long>::min()).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod_zero") == 0) {
        const auto n = (std::chrono::duration<long long>(5) % 0LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod_min_zero") == 0) {
        const auto n = (std::chrono::duration<long long>::min() % 0LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod_ovf") == 0) {
        const auto n = (std::chrono::duration<long long>::min() % -1LL).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod_eq_zero") == 0) {
        dur z(5);
        z %= 0LL;
        std::printf("wrapped=%lld\n", static_cast<long long>(z.count()));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod_eq_min_zero") == 0) {
        dur z = dur::min();
        z %= 0LL;
        std::printf("wrapped=%lld\n", static_cast<long long>(z.count()));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod_eq_ovf") == 0) {
        dur z = dur::min();
        z %= -1LL;
        std::printf("wrapped=%lld\n", static_cast<long long>(z.count()));
        return 0;
    }
    return 0;
}
