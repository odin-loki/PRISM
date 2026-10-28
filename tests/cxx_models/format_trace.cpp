// Differential trace of std::format_to_n (tests/cpp/test_cxx_format.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/format) first on the include path. Both under
// ASan/UBSan. The happy-path lines must match.
//
// std::format and format_to are not called. This header does not declare
// them; a use fails to compile and pir falls back to libstdc++.
//
// argv "null" passes a null output pointer with n > 0 for an int. The model
// must __prism_check and must not print a size. argv "nullll" is that case
// for long long (FUNC-CONTRACT). argv "bad" is a format other than "{}".
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
    if (argc > 1 && std::strcmp(argv[1], "nullll") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<long long>(42));
        std::printf("nullll size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "bad") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<long long>(42));
        std::printf("bad size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
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

    char bigll[8];
    std::memset(bigll, 'x', sizeof bigll);
    auto llfull = std::format_to_n(bigll, sizeof bigll, "{}", static_cast<long long>(42));
    std::printf("llfull size=%lld d0=%c d1=%c\n", static_cast<long long>(llfull.size), bigll[0], bigll[1]);

    char cut[8];
    std::memset(cut, 'x', sizeof cut);
    auto lltiny = std::format_to_n(cut, static_cast<std::size_t>(1), "{}", static_cast<long long>(42));
    std::printf("lltiny size=%lld d0=%c rest=%c\n", static_cast<long long>(lltiny.size), cut[0], cut[1]);

    char zll[4];
    std::memset(zll, 'x', sizeof zll);
    auto llz = std::format_to_n(zll, static_cast<std::size_t>(0), "{}", static_cast<long long>(42));
    std::printf("llzero size=%lld untouched=%c\n", static_cast<long long>(llz.size), zll[0]);

    auto lln0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                 static_cast<long long>(42));
    std::printf("llnull0 size=%lld\n", static_cast<long long>(lln0.size));

    char negll[8];
    std::memset(negll, 'x', sizeof negll);
    auto llng = std::format_to_n(negll, sizeof negll, "{}", static_cast<long long>(-3));
    std::printf("llneg size=%lld d0=%c d1=%c\n", static_cast<long long>(llng.size), negll[0], negll[1]);

    char one[4];
    std::memset(one, 'x', sizeof one);
    auto llnz = std::format_to_n(one, sizeof one, "{}", static_cast<long long>(7));
    std::printf("llnz size=%lld d0=%c\n", static_cast<long long>(llnz.size), one[0]);

    char tail[8];
    std::memset(tail, 'x', sizeof tail);
    auto llcut = std::format_to_n(tail, static_cast<std::size_t>(4), "{}", -9223372036854775807LL - 1);
    std::printf("llcut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(llcut.size), tail[0],
                tail[1], tail[2], tail[3], tail[4]);

    char mnll[24];
    std::memset(mnll, 'x', sizeof mnll);
    auto llmin = std::format_to_n(mnll, sizeof mnll, "{}", -9223372036854775807LL - 1);
    std::printf("llmin size=%lld text=", static_cast<long long>(llmin.size));
    for (long long i = 0; i < llmin.size && i < 24; ++i) std::putchar(mnll[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char mxll[24];
    std::memset(mxll, 'x', sizeof mxll);
    auto llmax = std::format_to_n(mxll, sizeof mxll, "{}", 9223372036854775807LL);
    std::printf("llmax size=%lld text=", static_cast<long long>(llmax.size));
    for (long long i = 0; i < llmax.size && i < 24; ++i) std::putchar(mxll[static_cast<std::size_t>(i)]);
    std::printf("\n");
    return 0;
}
