// Trace of the partial <chrono> model (src/prism/pir/models/cxx/chrono).
// Compile with that directory as -isystem so this include is the model.
// Clocks and sleep are not called: they are not declared. `ovf` adds the
// maximum signed 64-bit count to 1; the model must __prism_check and must
// not print a wrapped negative count.
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
    std::chrono::duration<long long> d(5);
    std::chrono::duration<long long> e(1);
    std::printf("diff=%lld\n", static_cast<long long>((d - e).count()));
    std::printf("zero_sum=%lld\n", static_cast<long long>((zero + zero).count()));
    if (argc > 1 && std::strcmp(argv[1], "ovf") == 0) {
        std::chrono::duration<long long> hi(9223372036854775807LL);
        std::chrono::duration<long long> one(1);
        const auto n = (hi + one).count();
        std::printf("wrapped=%lld\n", static_cast<long long>(n));
        return 0;
    }
    return 0;
}
