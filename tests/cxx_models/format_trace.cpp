// Differential trace of std::format_to_n (tests/cpp/test_cxx_format.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/format) first on the include path. Both under
// ASan/UBSan. The happy-path lines must match.
//
// std::format and format_to are not called. This header does not declare
// them; a use fails to compile and pir falls back to libstdc++.
//
// argv "null" passes a null output pointer with n > 0. The model must
// __prism_check and must not print a size.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned size. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "null") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}", 42);
        std::printf("null size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    char big[8];
    std::memset(big, 'x', sizeof big);
    auto full = std::format_to_n(big, sizeof big, "{}", 42);
    std::printf("full size=%lld d0=%c d1=%c\n", static_cast<long long>(full.size), big[0], big[1]);

    char small[8];
    std::memset(small, 'x', sizeof small);
    auto tiny = std::format_to_n(small, static_cast<std::size_t>(1), "{}", 42);
    std::printf("tiny size=%lld d0=%c rest=%c\n", static_cast<long long>(tiny.size), small[0], small[1]);

    char zbuf[4];
    std::memset(zbuf, 'x', sizeof zbuf);
    auto z = std::format_to_n(zbuf, static_cast<std::size_t>(0), "{}", 42);
    std::printf("zeron size=%lld untouched=%c\n", static_cast<long long>(z.size), zbuf[0]);

    auto zn = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}", 42);
    std::printf("null0 size=%lld\n", static_cast<long long>(zn.size));

    char neg[8];
    std::memset(neg, 'x', sizeof neg);
    auto ng = std::format_to_n(neg, sizeof neg, "{}", -3);
    std::printf("neg size=%lld d0=%c d1=%c\n", static_cast<long long>(ng.size), neg[0], neg[1]);

    char mn[16];
    std::memset(mn, 'x', sizeof mn);
    auto mnv = std::format_to_n(mn, sizeof mn, "{}", -2147483647 - 1);
    std::printf("min size=%lld text=", static_cast<long long>(mnv.size));
    for (long long i = 0; i < mnv.size && i < 16; ++i) std::putchar(mn[static_cast<std::size_t>(i)]);
    std::printf("\n");
    return 0;
}
