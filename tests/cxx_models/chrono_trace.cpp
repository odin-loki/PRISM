// Trace of the partial <chrono> model (src/prism/pir/models/cxx/chrono).
// Compile with that directory as -isystem so this include is the model.
// Clocks and sleep are not called: they are not declared. A same-period
// duration_cast keeps the count. A different period is not named: that
// cast is not declared. Same-type += and -= update the stored count.
// `ovf` adds the maximum signed 64-bit count to 1. `plus_eq_ovf` and
// `minus_eq_ovf` do the same through compound assignment. The model must
// __prism_check and must not print a wrapped negative count.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
