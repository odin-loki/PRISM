// Differential trace of std::format_to_n (tests/cpp/test_cxx_format.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/format) first on the include path. Both under
// ASan/UBSan. The happy-path lines must match.
//
// std::format and format_to are declared later in this header. This trace
// does not call them.
//
// argv "null" passes a null output pointer with n > 0 for an int. The model
// must __prism_check and must not print a size. argv "nullll" is that case
// for long long (FUNC-CONTRACT). argv "nullull" is that case for unsigned
// long long. argv "nullu" is that case for unsigned. argv "nullb" is that
// case for bool. argv "nullc" is that case for char. argv "nulls" is that
// case for short: the int overload's PTR-NULL-DEREF check, and no write.
// argv "nullus" is that case for unsigned short: the unsigned overload's
// FUNC-CONTRACT check, and no write. argv "nulllong" is that case for long:
// the long long overload's FUNC-CONTRACT check, and no write. argv
// "nullulong" is that case for unsigned long: the unsigned long long
// overload's FUNC-CONTRACT check, and no write. argv "nullsc" is that case
// for signed char: the int overload's PTR-NULL-DEREF check, and no write.
// argv "nulluc" is that case for unsigned char: the unsigned overload's
// FUNC-CONTRACT check, and no write. argv "bad", "badull", "badu", "badb",
// "badc", "bads", "badus", "badlong", "badulong", "badsc", and "baduc" are
// a format other than "{}". true writes t r u e and the size is 4. false
// writes f a l s e and the size is 5. A char value writes that one character
// and the size is 1. short and signed char are decimal through int. unsigned
// char and unsigned short are decimal through unsigned. long is decimal
// through long long. unsigned long is decimal through unsigned long long.
// n == 0 returns the caller's pointer. This is not PROVED and not CLEAN.
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
    if (argc > 1 && std::strcmp(argv[1], "nullull") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<unsigned long long>(42));
        std::printf("nullull size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badull") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<unsigned long long>(42));
        std::printf("badull size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullu") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<unsigned>(42));
        std::printf("nullu size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badu") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<unsigned>(42));
        std::printf("badu size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullb") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}", true);
        std::printf("nullb size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badb") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", false);
        std::printf("badb size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullc") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<char>('A'));
        std::printf("nullc size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badc") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<char>('A'));
        std::printf("badc size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nulls") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<short>(42));
        std::printf("nulls size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "bads") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<short>(42));
        std::printf("bads size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullus") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<unsigned short>(42));
        std::printf("nullus size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badus") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<unsigned short>(42));
        std::printf("badus size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nulllong") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<long>(42));
        std::printf("nulllong size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badlong") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<long>(42));
        std::printf("badlong size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullulong") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<unsigned long>(42));
        std::printf("nullulong size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badulong") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<unsigned long>(42));
        std::printf("badulong size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullsc") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<signed char>(42));
        std::printf("nullsc size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badsc") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<signed char>(42));
        std::printf("badsc size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nulluc") == 0) {
        auto r = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(4), "{}",
                                  static_cast<unsigned char>(42));
        std::printf("nulluc size=%lld\n", static_cast<long long>(r.size));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "baduc") == 0) {
        char buf[8];
        std::memset(buf, 'x', sizeof buf);
        auto r = std::format_to_n(buf, sizeof buf, "nope", static_cast<unsigned char>(42));
        std::printf("baduc size=%lld b0=%c\n", static_cast<long long>(r.size), buf[0]);
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

    char ubig[8];
    std::memset(ubig, 'x', sizeof ubig);
    auto ullfull = std::format_to_n(ubig, sizeof ubig, "{}", static_cast<unsigned long long>(42));
    std::printf("ullfull size=%lld d0=%c d1=%c\n", static_cast<long long>(ullfull.size), ubig[0], ubig[1]);

    char utiny[8];
    std::memset(utiny, 'x', sizeof utiny);
    auto ulltiny =
        std::format_to_n(utiny, static_cast<std::size_t>(1), "{}", static_cast<unsigned long long>(42));
    std::printf("ulltiny size=%lld d0=%c rest=%c\n", static_cast<long long>(ulltiny.size), utiny[0], utiny[1]);

    char uz[4];
    std::memset(uz, 'x', sizeof uz);
    auto ullz = std::format_to_n(uz, static_cast<std::size_t>(0), "{}", static_cast<unsigned long long>(42));
    std::printf("ullzero size=%lld untouched=%c same=%d\n", static_cast<long long>(ullz.size), uz[0],
                ullz.out == uz ? 1 : 0);

    auto ulln0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                  static_cast<unsigned long long>(42));
    std::printf("ullnull0 size=%lld same=%d\n", static_cast<long long>(ulln0.size), ulln0.out == nullptr ? 1 : 0);

    char uone[4];
    std::memset(uone, 'x', sizeof uone);
    auto ullnz = std::format_to_n(uone, sizeof uone, "{}", static_cast<unsigned long long>(7));
    std::printf("ullnz size=%lld d0=%c\n", static_cast<long long>(ullnz.size), uone[0]);

    char udig[4];
    std::memset(udig, 'x', sizeof udig);
    auto ullzero_val = std::format_to_n(udig, sizeof udig, "{}", static_cast<unsigned long long>(0));
    std::printf("ullz size=%lld d0=%c\n", static_cast<long long>(ullzero_val.size), udig[0]);

    char uwide[8];
    std::memset(uwide, 'x', sizeof uwide);
    auto ullwide = std::format_to_n(uwide, static_cast<std::size_t>(4), "{}", 10000000000ull);
    std::printf("ullwide size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(ullwide.size), uwide[0],
                uwide[1], uwide[2], uwide[3], uwide[4]);

    char ucut[8];
    std::memset(ucut, 'x', sizeof ucut);
    auto ullcut = std::format_to_n(ucut, static_cast<std::size_t>(4), "{}", 18446744073709551615ull);
    std::printf("ullcut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(ullcut.size), ucut[0],
                ucut[1], ucut[2], ucut[3], ucut[4]);

    char umax[24];
    std::memset(umax, 'x', sizeof umax);
    auto ullmax = std::format_to_n(umax, sizeof umax, "{}", 18446744073709551615ull);
    std::printf("ullmax size=%lld text=", static_cast<long long>(ullmax.size));
    for (long long i = 0; i < ullmax.size && i < 24; ++i) std::putchar(umax[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char usmall[8];
    std::memset(usmall, 'x', sizeof usmall);
    auto ufull = std::format_to_n(usmall, sizeof usmall, "{}", static_cast<unsigned>(42));
    std::printf("ufull size=%lld d0=%c d1=%c\n", static_cast<long long>(ufull.size), usmall[0], usmall[1]);

    char utinyb[8];
    std::memset(utinyb, 'x', sizeof utinyb);
    auto utiny_r = std::format_to_n(utinyb, static_cast<std::size_t>(1), "{}", static_cast<unsigned>(42));
    std::printf("utiny size=%lld d0=%c rest=%c\n", static_cast<long long>(utiny_r.size), utinyb[0], utinyb[1]);

    char uzerob[4];
    std::memset(uzerob, 'x', sizeof uzerob);
    auto uzero = std::format_to_n(uzerob, static_cast<std::size_t>(0), "{}", static_cast<unsigned>(42));
    std::printf("uzero size=%lld untouched=%c same=%d\n", static_cast<long long>(uzero.size), uzerob[0],
                uzero.out == uzerob ? 1 : 0);

    auto unull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                   static_cast<unsigned>(42));
    std::printf("unull0 size=%lld same=%d\n", static_cast<long long>(unull0.size), unull0.out == nullptr ? 1 : 0);

    char useven[4];
    std::memset(useven, 'x', sizeof useven);
    auto unz = std::format_to_n(useven, sizeof useven, "{}", static_cast<unsigned>(7));
    std::printf("unz size=%lld d0=%c\n", static_cast<long long>(unz.size), useven[0]);

    char udigit[4];
    std::memset(udigit, 'x', sizeof udigit);
    auto uzero_val = std::format_to_n(udigit, sizeof udigit, "{}", static_cast<unsigned>(0));
    std::printf("uz size=%lld d0=%c\n", static_cast<long long>(uzero_val.size), udigit[0]);

    char uwideb[8];
    std::memset(uwideb, 'x', sizeof uwideb);
    auto uwide_r = std::format_to_n(uwideb, static_cast<std::size_t>(4), "{}", 3000000000u);
    std::printf("uwide size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(uwide_r.size), uwideb[0],
                uwideb[1], uwideb[2], uwideb[3], uwideb[4]);

    char ucutb[8];
    std::memset(ucutb, 'x', sizeof ucutb);
    auto ucut_r = std::format_to_n(ucutb, static_cast<std::size_t>(4), "{}", 4294967295u);
    std::printf("ucut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(ucut_r.size), ucutb[0],
                ucutb[1], ucutb[2], ucutb[3], ucutb[4]);

    char umaxb[16];
    std::memset(umaxb, 'x', sizeof umaxb);
    auto umaxu = std::format_to_n(umaxb, sizeof umaxb, "{}", 4294967295u);
    std::printf("umax size=%lld text=", static_cast<long long>(umaxu.size));
    for (long long i = 0; i < umaxu.size && i < 16; ++i) std::putchar(umaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char bt[8];
    std::memset(bt, 'x', sizeof bt);
    auto btrue = std::format_to_n(bt, sizeof bt, "{}", true);
    std::printf("btrue size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(btrue.size), bt[0], bt[1],
                bt[2], bt[3], bt[4]);

    char bf[8];
    std::memset(bf, 'x', sizeof bf);
    auto bfalse = std::format_to_n(bf, sizeof bf, "{}", false);
    std::printf("bfalse size=%lld c0=%c c1=%c c2=%c c3=%c c4=%c rest=%c\n", static_cast<long long>(bfalse.size), bf[0],
                bf[1], bf[2], bf[3], bf[4], bf[5]);

    char btinyc[8];
    std::memset(btinyc, 'x', sizeof btinyc);
    auto btruetiny = std::format_to_n(btinyc, static_cast<std::size_t>(1), "{}", true);
    std::printf("btruetiny size=%lld d0=%c rest=%c\n", static_cast<long long>(btruetiny.size), btinyc[0], btinyc[1]);

    char bftinyc[8];
    std::memset(bftinyc, 'x', sizeof bftinyc);
    auto bfalsetiny = std::format_to_n(bftinyc, static_cast<std::size_t>(1), "{}", false);
    std::printf("bfalsetiny size=%lld d0=%c rest=%c\n", static_cast<long long>(bfalsetiny.size), bftinyc[0],
                bftinyc[1]);

    char bz[4];
    std::memset(bz, 'x', sizeof bz);
    auto bzero = std::format_to_n(bz, static_cast<std::size_t>(0), "{}", true);
    std::printf("bzero size=%lld untouched=%c same=%d\n", static_cast<long long>(bzero.size), bz[0],
                bzero.out == bz ? 1 : 0);

    char bzfalse[4];
    std::memset(bzfalse, 'x', sizeof bzfalse);
    auto bfalse0 = std::format_to_n(bzfalse, static_cast<std::size_t>(0), "{}", false);
    std::printf("bfalse0 size=%lld untouched=%c same=%d\n", static_cast<long long>(bfalse0.size), bzfalse[0],
                bfalse0.out == bzfalse ? 1 : 0);

    auto bnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}", true);
    std::printf("bnull0 size=%lld same=%d\n", static_cast<long long>(bnull0.size), bnull0.out == nullptr ? 1 : 0);

    auto bfalsenull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}", false);
    std::printf("bfalsenull0 size=%lld same=%d\n", static_cast<long long>(bfalsenull0.size),
                bfalsenull0.out == nullptr ? 1 : 0);

    char bcutc[8];
    std::memset(bcutc, 'x', sizeof bcutc);
    auto bcut = std::format_to_n(bcutc, static_cast<std::size_t>(4), "{}", false);
    std::printf("bcut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(bcut.size), bcutc[0],
                bcutc[1], bcutc[2], bcutc[3], bcutc[4]);

    char btrue3c[8];
    std::memset(btrue3c, 'x', sizeof btrue3c);
    auto btrue3 = std::format_to_n(btrue3c, static_cast<std::size_t>(3), "{}", true);
    std::printf("btrue3 size=%lld c0=%c c1=%c c2=%c rest=%c\n", static_cast<long long>(btrue3.size), btrue3c[0],
                btrue3c[1], btrue3c[2], btrue3c[3]);

    char cbuf[8];
    std::memset(cbuf, 'x', sizeof cbuf);
    auto cfull = std::format_to_n(cbuf, sizeof cbuf, "{}", static_cast<char>('A'));
    std::printf("cfull size=%lld c0=%c rest=%c adv=%d\n", static_cast<long long>(cfull.size), cbuf[0], cbuf[1],
                cfull.out == cbuf + 1 ? 1 : 0);

    char cqbuf[8];
    std::memset(cqbuf, 'x', sizeof cqbuf);
    auto cQ = std::format_to_n(cqbuf, sizeof cqbuf, "{}", static_cast<char>('Q'));
    std::printf("cQ size=%lld c0=%c rest=%c\n", static_cast<long long>(cQ.size), cqbuf[0], cqbuf[1]);

    char c1[4];
    std::memset(c1, 'x', sizeof c1);
    auto cn1 = std::format_to_n(c1, static_cast<std::size_t>(1), "{}", static_cast<char>('A'));
    std::printf("cn1 size=%lld c0=%c rest=%c\n", static_cast<long long>(cn1.size), c1[0], c1[1]);

    char czc[4];
    std::memset(czc, 'x', sizeof czc);
    auto czero = std::format_to_n(czc, static_cast<std::size_t>(0), "{}", static_cast<char>('A'));
    std::printf("czero size=%lld untouched=%c same=%d\n", static_cast<long long>(czero.size), czc[0],
                czero.out == czc ? 1 : 0);

    char cq0b[4];
    std::memset(cq0b, 'x', sizeof cq0b);
    auto cQ0 = std::format_to_n(cq0b, static_cast<std::size_t>(0), "{}", static_cast<char>('Q'));
    std::printf("cQ0 size=%lld untouched=%c same=%d\n", static_cast<long long>(cQ0.size), cq0b[0],
                cQ0.out == cq0b ? 1 : 0);

    auto cnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                   static_cast<char>('A'));
    std::printf("cnull0 size=%lld same=%d\n", static_cast<long long>(cnull0.size), cnull0.out == nullptr ? 1 : 0);

    char shbuf[8];
    std::memset(shbuf, 'x', sizeof shbuf);
    auto shfull = std::format_to_n(shbuf, sizeof shbuf, "{}", static_cast<short>(42));
    std::printf("shfull size=%lld d0=%c d1=%c rest=%c\n", static_cast<long long>(shfull.size), shbuf[0], shbuf[1],
                shbuf[2]);

    char shtinyb[8];
    std::memset(shtinyb, 'x', sizeof shtinyb);
    auto shtiny = std::format_to_n(shtinyb, static_cast<std::size_t>(1), "{}", static_cast<short>(42));
    std::printf("shtiny size=%lld d0=%c rest=%c\n", static_cast<long long>(shtiny.size), shtinyb[0], shtinyb[1]);

    char shzbuf[4];
    std::memset(shzbuf, 'x', sizeof shzbuf);
    auto shzero = std::format_to_n(shzbuf, static_cast<std::size_t>(0), "{}", static_cast<short>(42));
    std::printf("shzero size=%lld untouched=%c same=%d\n", static_cast<long long>(shzero.size), shzbuf[0],
                shzero.out == shzbuf ? 1 : 0);

    auto shnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                    static_cast<short>(42));
    std::printf("shnull0 size=%lld same=%d\n", static_cast<long long>(shnull0.size), shnull0.out == nullptr ? 1 : 0);

    char shnegb[8];
    std::memset(shnegb, 'x', sizeof shnegb);
    auto shneg = std::format_to_n(shnegb, sizeof shnegb, "{}", static_cast<short>(-3));
    std::printf("shneg size=%lld d0=%c d1=%c\n", static_cast<long long>(shneg.size), shnegb[0], shnegb[1]);

    char shzeroval[4];
    std::memset(shzeroval, 'x', sizeof shzeroval);
    auto shz = std::format_to_n(shzeroval, sizeof shzeroval, "{}", static_cast<short>(0));
    std::printf("shz size=%lld d0=%c\n", static_cast<long long>(shz.size), shzeroval[0]);

    char shminb[8];
    std::memset(shminb, 'x', sizeof shminb);
    auto shmin = std::format_to_n(shminb, sizeof shminb, "{}", static_cast<short>(-32768));
    std::printf("shmin size=%lld text=", static_cast<long long>(shmin.size));
    for (long long i = 0; i < shmin.size && i < 8; ++i) std::putchar(shminb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char shmaxb[8];
    std::memset(shmaxb, 'x', sizeof shmaxb);
    auto shmax = std::format_to_n(shmaxb, sizeof shmaxb, "{}", static_cast<short>(32767));
    std::printf("shmax size=%lld text=", static_cast<long long>(shmax.size));
    for (long long i = 0; i < shmax.size && i < 8; ++i) std::putchar(shmaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char shcutb[8];
    std::memset(shcutb, 'x', sizeof shcutb);
    auto shcut = std::format_to_n(shcutb, static_cast<std::size_t>(4), "{}", static_cast<short>(-32768));
    std::printf("shcut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(shcut.size), shcutb[0],
                shcutb[1], shcutb[2], shcutb[3], shcutb[4]);

    char usbuf[8];
    std::memset(usbuf, 'x', sizeof usbuf);
    auto usfull = std::format_to_n(usbuf, sizeof usbuf, "{}", static_cast<unsigned short>(42));
    std::printf("usfull size=%lld d0=%c d1=%c rest=%c\n", static_cast<long long>(usfull.size), usbuf[0], usbuf[1],
                usbuf[2]);

    char ustinyb[8];
    std::memset(ustinyb, 'x', sizeof ustinyb);
    auto ustiny = std::format_to_n(ustinyb, static_cast<std::size_t>(1), "{}", static_cast<unsigned short>(42));
    std::printf("ustiny size=%lld d0=%c rest=%c\n", static_cast<long long>(ustiny.size), ustinyb[0], ustinyb[1]);

    char uszbuf[4];
    std::memset(uszbuf, 'x', sizeof uszbuf);
    auto uszero = std::format_to_n(uszbuf, static_cast<std::size_t>(0), "{}", static_cast<unsigned short>(42));
    std::printf("uszero size=%lld untouched=%c same=%d\n", static_cast<long long>(uszero.size), uszbuf[0],
                uszero.out == uszbuf ? 1 : 0);

    auto usnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                    static_cast<unsigned short>(42));
    std::printf("usnull0 size=%lld same=%d\n", static_cast<long long>(usnull0.size), usnull0.out == nullptr ? 1 : 0);

    char uszeroval[4];
    std::memset(uszeroval, 'x', sizeof uszeroval);
    auto usz = std::format_to_n(uszeroval, sizeof uszeroval, "{}", static_cast<unsigned short>(0));
    std::printf("usz size=%lld d0=%c\n", static_cast<long long>(usz.size), uszeroval[0]);

    char usmaxb[8];
    std::memset(usmaxb, 'x', sizeof usmaxb);
    auto usmax = std::format_to_n(usmaxb, sizeof usmaxb, "{}", static_cast<unsigned short>(65535));
    std::printf("usmax size=%lld text=", static_cast<long long>(usmax.size));
    for (long long i = 0; i < usmax.size && i < 8; ++i) std::putchar(usmaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char uscutb[8];
    std::memset(uscutb, 'x', sizeof uscutb);
    auto uscut = std::format_to_n(uscutb, static_cast<std::size_t>(4), "{}", static_cast<unsigned short>(65535));
    std::printf("uscut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(uscut.size), uscutb[0],
                uscutb[1], uscutb[2], uscutb[3], uscutb[4]);

    char lgbuf[16];
    std::memset(lgbuf, 'x', sizeof lgbuf);
    auto lgfull = std::format_to_n(lgbuf, sizeof lgbuf, "{}", static_cast<long>(42));
    std::printf("lgfull size=%lld d0=%c d1=%c rest=%c\n", static_cast<long long>(lgfull.size), lgbuf[0], lgbuf[1],
                lgbuf[2]);

    char lgtinyb[8];
    std::memset(lgtinyb, 'x', sizeof lgtinyb);
    auto lgtiny = std::format_to_n(lgtinyb, static_cast<std::size_t>(1), "{}", static_cast<long>(42));
    std::printf("lgtiny size=%lld d0=%c rest=%c\n", static_cast<long long>(lgtiny.size), lgtinyb[0], lgtinyb[1]);

    char lgzbuf[4];
    std::memset(lgzbuf, 'x', sizeof lgzbuf);
    auto lgzero = std::format_to_n(lgzbuf, static_cast<std::size_t>(0), "{}", static_cast<long>(42));
    std::printf("lgzero size=%lld untouched=%c same=%d\n", static_cast<long long>(lgzero.size), lgzbuf[0],
                lgzero.out == lgzbuf ? 1 : 0);

    auto lgnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                    static_cast<long>(42));
    std::printf("lgnull0 size=%lld same=%d\n", static_cast<long long>(lgnull0.size), lgnull0.out == nullptr ? 1 : 0);

    char lgnegb[8];
    std::memset(lgnegb, 'x', sizeof lgnegb);
    auto lgneg = std::format_to_n(lgnegb, sizeof lgnegb, "{}", static_cast<long>(-3));
    std::printf("lgneg size=%lld d0=%c d1=%c\n", static_cast<long long>(lgneg.size), lgnegb[0], lgnegb[1]);

    char lgzeroval[4];
    std::memset(lgzeroval, 'x', sizeof lgzeroval);
    auto lgz = std::format_to_n(lgzeroval, sizeof lgzeroval, "{}", static_cast<long>(0));
    std::printf("lgz size=%lld d0=%c\n", static_cast<long long>(lgz.size), lgzeroval[0]);

    char lgminb[16];
    std::memset(lgminb, 'x', sizeof lgminb);
    auto lgmin = std::format_to_n(lgminb, sizeof lgminb, "{}", static_cast<long>(-2147483647 - 1));
    std::printf("lgmin size=%lld text=", static_cast<long long>(lgmin.size));
    for (long long i = 0; i < lgmin.size && i < 16; ++i) std::putchar(lgminb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char lgmaxb[16];
    std::memset(lgmaxb, 'x', sizeof lgmaxb);
    auto lgmax = std::format_to_n(lgmaxb, sizeof lgmaxb, "{}", static_cast<long>(2147483647));
    std::printf("lgmax size=%lld text=", static_cast<long long>(lgmax.size));
    for (long long i = 0; i < lgmax.size && i < 16; ++i) std::putchar(lgmaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char lgcutb[8];
    std::memset(lgcutb, 'x', sizeof lgcutb);
    auto lgcut = std::format_to_n(lgcutb, static_cast<std::size_t>(4), "{}", static_cast<long>(-2147483647 - 1));
    std::printf("lgcut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(lgcut.size), lgcutb[0],
                lgcutb[1], lgcutb[2], lgcutb[3], lgcutb[4]);

    char ulbuf[16];
    std::memset(ulbuf, 'x', sizeof ulbuf);
    auto ulfull = std::format_to_n(ulbuf, sizeof ulbuf, "{}", static_cast<unsigned long>(42));
    std::printf("ulfull size=%lld d0=%c d1=%c rest=%c\n", static_cast<long long>(ulfull.size), ulbuf[0], ulbuf[1],
                ulbuf[2]);

    char ultinyb[8];
    std::memset(ultinyb, 'x', sizeof ultinyb);
    auto ultiny = std::format_to_n(ultinyb, static_cast<std::size_t>(1), "{}", static_cast<unsigned long>(42));
    std::printf("ultiny size=%lld d0=%c rest=%c\n", static_cast<long long>(ultiny.size), ultinyb[0], ultinyb[1]);

    char ulzbuf[4];
    std::memset(ulzbuf, 'x', sizeof ulzbuf);
    auto ulzero = std::format_to_n(ulzbuf, static_cast<std::size_t>(0), "{}", static_cast<unsigned long>(42));
    std::printf("ulzero size=%lld untouched=%c same=%d\n", static_cast<long long>(ulzero.size), ulzbuf[0],
                ulzero.out == ulzbuf ? 1 : 0);

    auto ulnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                    static_cast<unsigned long>(42));
    std::printf("ulnull0 size=%lld same=%d\n", static_cast<long long>(ulnull0.size), ulnull0.out == nullptr ? 1 : 0);

    char ulzeroval[4];
    std::memset(ulzeroval, 'x', sizeof ulzeroval);
    auto ulz = std::format_to_n(ulzeroval, sizeof ulzeroval, "{}", static_cast<unsigned long>(0));
    std::printf("ulz size=%lld d0=%c\n", static_cast<long long>(ulz.size), ulzeroval[0]);

    char ulmaxb[16];
    std::memset(ulmaxb, 'x', sizeof ulmaxb);
    auto ulmax = std::format_to_n(ulmaxb, sizeof ulmaxb, "{}", static_cast<unsigned long>(4294967295ul));
    std::printf("ulmax size=%lld text=", static_cast<long long>(ulmax.size));
    for (long long i = 0; i < ulmax.size && i < 16; ++i) std::putchar(ulmaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char ulcutb[8];
    std::memset(ulcutb, 'x', sizeof ulcutb);
    auto ulcut = std::format_to_n(ulcutb, static_cast<std::size_t>(4), "{}", static_cast<unsigned long>(4294967295ul));
    std::printf("ulcut size=%lld c0=%c c1=%c c2=%c c3=%c rest=%c\n", static_cast<long long>(ulcut.size), ulcutb[0],
                ulcutb[1], ulcutb[2], ulcutb[3], ulcutb[4]);

    char scbuf[8];
    std::memset(scbuf, 'x', sizeof scbuf);
    auto scfull = std::format_to_n(scbuf, sizeof scbuf, "{}", static_cast<signed char>(42));
    std::printf("scfull size=%lld d0=%c d1=%c rest=%c\n", static_cast<long long>(scfull.size), scbuf[0], scbuf[1],
                scbuf[2]);

    char sctinyb[8];
    std::memset(sctinyb, 'x', sizeof sctinyb);
    auto sctiny = std::format_to_n(sctinyb, static_cast<std::size_t>(1), "{}", static_cast<signed char>(42));
    std::printf("sctiny size=%lld d0=%c rest=%c\n", static_cast<long long>(sctiny.size), sctinyb[0], sctinyb[1]);

    char sczbuf[4];
    std::memset(sczbuf, 'x', sizeof sczbuf);
    auto sczero = std::format_to_n(sczbuf, static_cast<std::size_t>(0), "{}", static_cast<signed char>(42));
    std::printf("sczero size=%lld untouched=%c same=%d\n", static_cast<long long>(sczero.size), sczbuf[0],
                sczero.out == sczbuf ? 1 : 0);

    auto scnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                    static_cast<signed char>(42));
    std::printf("scnull0 size=%lld same=%d\n", static_cast<long long>(scnull0.size), scnull0.out == nullptr ? 1 : 0);

    char scnegb[8];
    std::memset(scnegb, 'x', sizeof scnegb);
    auto scneg = std::format_to_n(scnegb, sizeof scnegb, "{}", static_cast<signed char>(-3));
    std::printf("scneg size=%lld d0=%c d1=%c\n", static_cast<long long>(scneg.size), scnegb[0], scnegb[1]);

    char sczeroval[4];
    std::memset(sczeroval, 'x', sizeof sczeroval);
    auto scz = std::format_to_n(sczeroval, sizeof sczeroval, "{}", static_cast<signed char>(0));
    std::printf("scz size=%lld d0=%c\n", static_cast<long long>(scz.size), sczeroval[0]);

    char scminb[8];
    std::memset(scminb, 'x', sizeof scminb);
    auto scmin = std::format_to_n(scminb, sizeof scminb, "{}", static_cast<signed char>(-128));
    std::printf("scmin size=%lld text=", static_cast<long long>(scmin.size));
    for (long long i = 0; i < scmin.size && i < 8; ++i) std::putchar(scminb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char scmaxb[8];
    std::memset(scmaxb, 'x', sizeof scmaxb);
    auto scmax = std::format_to_n(scmaxb, sizeof scmaxb, "{}", static_cast<signed char>(127));
    std::printf("scmax size=%lld text=", static_cast<long long>(scmax.size));
    for (long long i = 0; i < scmax.size && i < 8; ++i) std::putchar(scmaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char sccutb[8];
    std::memset(sccutb, 'x', sizeof sccutb);
    auto sccut = std::format_to_n(sccutb, static_cast<std::size_t>(3), "{}", static_cast<signed char>(-128));
    std::printf("sccut size=%lld c0=%c c1=%c c2=%c rest=%c\n", static_cast<long long>(sccut.size), sccutb[0],
                sccutb[1], sccutb[2], sccutb[3]);

    char ucbuf[8];
    std::memset(ucbuf, 'x', sizeof ucbuf);
    auto ucfull = std::format_to_n(ucbuf, sizeof ucbuf, "{}", static_cast<unsigned char>(42));
    std::printf("ucfull size=%lld d0=%c d1=%c rest=%c\n", static_cast<long long>(ucfull.size), ucbuf[0], ucbuf[1],
                ucbuf[2]);

    char uctinyb[8];
    std::memset(uctinyb, 'x', sizeof uctinyb);
    auto uctiny = std::format_to_n(uctinyb, static_cast<std::size_t>(1), "{}", static_cast<unsigned char>(42));
    std::printf("uctiny size=%lld d0=%c rest=%c\n", static_cast<long long>(uctiny.size), uctinyb[0], uctinyb[1]);

    char uczbuf[4];
    std::memset(uczbuf, 'x', sizeof uczbuf);
    auto uczero = std::format_to_n(uczbuf, static_cast<std::size_t>(0), "{}", static_cast<unsigned char>(42));
    std::printf("uczero size=%lld untouched=%c same=%d\n", static_cast<long long>(uczero.size), uczbuf[0],
                uczero.out == uczbuf ? 1 : 0);

    auto ucnull0 = std::format_to_n(static_cast<char*>(nullptr), static_cast<std::size_t>(0), "{}",
                                    static_cast<unsigned char>(42));
    std::printf("ucnull0 size=%lld same=%d\n", static_cast<long long>(ucnull0.size), ucnull0.out == nullptr ? 1 : 0);

    char uczeroval[4];
    std::memset(uczeroval, 'x', sizeof uczeroval);
    auto ucz = std::format_to_n(uczeroval, sizeof uczeroval, "{}", static_cast<unsigned char>(0));
    std::printf("ucz size=%lld d0=%c\n", static_cast<long long>(ucz.size), uczeroval[0]);

    char ucmaxb[8];
    std::memset(ucmaxb, 'x', sizeof ucmaxb);
    auto ucmax = std::format_to_n(ucmaxb, sizeof ucmaxb, "{}", static_cast<unsigned char>(255));
    std::printf("ucmax size=%lld text=", static_cast<long long>(ucmax.size));
    for (long long i = 0; i < ucmax.size && i < 8; ++i) std::putchar(ucmaxb[static_cast<std::size_t>(i)]);
    std::printf("\n");

    char uccutb[8];
    std::memset(uccutb, 'x', sizeof uccutb);
    auto uccut = std::format_to_n(uccutb, static_cast<std::size_t>(2), "{}", static_cast<unsigned char>(255));
    std::printf("uccut size=%lld c0=%c c1=%c rest=%c\n", static_cast<long long>(uccut.size), uccutb[0], uccutb[1],
                uccutb[2]);
    return 0;
}
