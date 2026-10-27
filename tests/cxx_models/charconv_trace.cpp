// Differential trace of integer std::from_chars / std::to_chars
// (tests/cpp/test_cxx_charconv.cpp). Compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/charconv) first on the
// include path, both under ASan/UBSan. The safe-path lines must match.
//
// Floating-point from_chars, to_chars, and std::chars_format are not called:
// the model does not declare them.
//
// argv "null" calls from_chars with a null range. argv "invert" calls it with
// last before first. The model must __prism_check and must not print a
// successful zero.
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned value. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

static int ec_kind(std::errc ec) {
    if (ec == std::errc{}) return 1;
    if (ec == std::errc::result_out_of_range) return 2;
    if (ec == std::errc::invalid_argument) return 3;
    if (ec == std::errc::value_too_large) return 4;
    return 5;
}

template <typename T>
static void parse(const char* name, const char* text, T sentinel, int base = 10) {
    T value = sentinel;
    const auto end = text + std::strlen(text);
    const auto r = std::from_chars(text, end, value, base);
    const int off = static_cast<int>(r.ptr - text);
    std::printf("%s ec=%d v=%lld off=%d\n", name, ec_kind(r.ec), static_cast<long long>(value), off);
}

static void safe() {
    parse("small", "42", -1);
    parse("partial", "42x", -1);
    parse("zero", "0", -1);
    parse("neg", "-42", 1);
    parse("max", "2147483647", 1);
    parse("min", "-2147483648", 1);
    parse("over", "2147483648", 12345);
    parse("over0", "4294967296", 77);
    parse("minover", "-2147483649", 3);
    parse("none", "abc", 8);
    parse("empty", "", 8);
    parse("plus", "+42", 8);
    parse("hex", "ff", -1, 16);
    parse("hexup", "FF", -1, 16);
    parse("hex0", "0x10", -1, 16);
    parse<signed char>("scmin", "-128", static_cast<signed char>(9));
    parse<signed char>("scover", "128", static_cast<signed char>(9));
    parse<unsigned char>("ucmax", "255", static_cast<unsigned char>(4));
    parse<unsigned char>("ucover", "256", static_cast<unsigned char>(4));

    unsigned full = 5;
    const char* umax = "4294967295";
    auto uok = std::from_chars(umax, umax + std::strlen(umax), full);
    std::printf("umax ec=%d v=%u off=%d\n", ec_kind(uok.ec), full, static_cast<int>(uok.ptr - umax));
    unsigned wrapped = 5;
    const char* uover = "4294967296";
    auto ubad = std::from_chars(uover, uover + std::strlen(uover), wrapped);
    std::printf("uover ec=%d v=%u off=%d\n", ec_kind(ubad.ec), wrapped, static_cast<int>(ubad.ptr - uover));

    char negb[16];
    auto tn = std::to_chars(negb, negb + 16, -42);
    if (tn.ec == std::errc{}) *tn.ptr = '\0';
    std::printf("to ec=%d s=%s\n", ec_kind(tn.ec), tn.ec == std::errc{} ? negb : "");

    char minb[16];
    auto tm = std::to_chars(minb, minb + 16, std::numeric_limits<int>::min());
    if (tm.ec == std::errc{}) *tm.ptr = '\0';
    std::printf("tomin ec=%d s=%s\n", ec_kind(tm.ec), tm.ec == std::errc{} ? minb : "");

    char hexb[8];
    auto th = std::to_chars(hexb, hexb + 8, 255, 16);
    if (th.ec == std::errc{}) *th.ptr = '\0';
    std::printf("tohex ec=%d s=%s\n", ec_kind(th.ec), th.ec == std::errc{} ? hexb : "");

    char zerob[4];
    auto tz = std::to_chars(zerob, zerob + 4, 0);
    if (tz.ec == std::errc{}) *tz.ptr = '\0';
    std::printf("to0 ec=%d s=%s\n", ec_kind(tz.ec), tz.ec == std::errc{} ? zerob : "");

    char fit[2];
    auto tf = std::to_chars(fit, fit + 2, 42);
    std::printf("fit ec=%d n=%d c0=%c c1=%c\n", ec_kind(tf.ec), static_cast<int>(tf.ptr - fit), fit[0], fit[1]);

    char tiny[4] = {'Q', 'Q', 'Z', 'Z'};
    auto ts = std::to_chars(tiny, tiny + 2, 100000);
    std::printf("short ec=%d success=%d b0=%c b1=%c z2=%c z3=%c\n", ec_kind(ts.ec),
                static_cast<int>(ts.ec == std::errc{}), tiny[0], tiny[1], tiny[2], tiny[3]);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "null") == 0) {
        int value = 99;
        auto r = std::from_chars(nullptr, nullptr, value);
        std::printf("fake success=%d v=%d\n", static_cast<int>(r.ec == std::errc{}), value);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "invert") == 0) {
        char text[] = "12";
        int value = 99;
        auto r = std::from_chars(text + 2, text, value);
        std::printf("fake success=%d v=%d\n", static_cast<int>(r.ec == std::errc{}), value);
        return 0;
    }
    safe();
    return 0;
}
