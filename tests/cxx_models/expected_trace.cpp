// Differential trace of std::expected (tests/cpp/test_cxx_expected.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/expected) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// and_then, or_else, transform, and value_or are not called: the model does
// not declare them.
//
// argv "value" calls value() on an unexpected. argv "star" calls operator* on
// an unexpected. argv "error" calls error() on a success value. The model must
// __prism_check and must not print a value.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned T or E. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

struct P {
    int a;
    int b;
};

// No default constructor: a model that invents a T cannot value-initialise E.
struct E {
    int v;
    explicit E(int x) : v(x) { std::printf("c%d\n", v); }
    E(const E& o) : v(o.v) { std::printf("cc%d\n", v); }
    ~E() { std::printf("d%d\n", v); }
};

// No default constructor: a model that invents an error cannot value-initialise R.
struct R {
    int v;
    explicit R(int x) : v(x) { std::printf("e%d\n", v); }
    R(const R& o) : v(o.v) { std::printf("ec%d\n", v); }
    ~R() { std::printf("ed%d\n", v); }
};

static void show() {
    std::expected<int, int> def;
    std::printf("def %d %d\n", static_cast<int>(def.has_value()), def.value());

    std::expected<int, int> ok(42);
    std::printf("engaged %d %d %d %d\n", static_cast<int>(ok.has_value()),
                static_cast<int>(static_cast<bool>(ok)), *ok, ok.value());
    std::expected<int, int> copy(ok);
    std::printf("copy %d\n", copy.value());

    P p{7, 8};
    std::expected<P, int> op(p);
    std::printf("arrow %d %d\n", op->a, (*op).b);

    std::unexpected<int> u(3);
    std::printf("unexpected %d\n", u.error());

    std::expected<int, int> bad(std::unexpected<int>(7));
    std::printf("unex %d %d %d\n", static_cast<int>(bad.has_value()),
                static_cast<int>(static_cast<bool>(bad)), bad.error());

    std::expected<E, int> held(E(5));
    std::printf("held %d\n", held.value().v);

    std::expected<int, R> err(std::unexpected<R>(R(9)));
    std::printf("err %d\n", err.error().v);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "value") == 0) {
        std::expected<int, int> e(std::unexpected<int>(4));
        std::printf("fake=%d\n", e.value());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "star") == 0) {
        std::expected<int, int> e(std::unexpected<int>(4));
        std::printf("fake=%d\n", *e);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "error") == 0) {
        std::expected<int, int> e(42);
        std::printf("fake=%d\n", e.error());
        return 0;
    }
    show();
    return 0;
}
