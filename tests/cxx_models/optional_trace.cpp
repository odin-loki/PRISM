// Differential trace of std::optional (tests/cpp/test_cxx_optional.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/optional) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// emplace and hash are not called: the model does not declare them.
// value_or, and_then, or_else, transform, operator==, and operator< are
// declared; this trace does not call them.
//
// argv "empty" / "star" / "arrow" call value(), operator*, or operator-> on an
// empty optional. The model must __prism_check and must not print a value.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned T. A passing check returns.
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

static void show() {
    std::optional<int> empty;
    std::printf("empty %d %d\n", static_cast<int>(empty.has_value()),
                static_cast<int>(static_cast<bool>(empty)));

    std::optional<int> o(42);
    std::printf("engaged %d %d %d %d\n", static_cast<int>(o.has_value()),
                static_cast<int>(static_cast<bool>(o)), *o, o.value());
    std::optional<int> copy(o);
    std::printf("copy %d\n", copy.value());

    P p{7, 8};
    std::optional<P> op(p);
    std::printf("arrow %d %d\n", op->a, (*op).b);

    o.reset();
    std::printf("reset %d\n", static_cast<int>(o.has_value()));

    std::optional<E> e(E(5));
    std::printf("held %d\n", e.value().v);
    e.reset();
    std::printf("resetE %d\n", static_cast<int>(e.has_value()));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "empty") == 0) {
        std::optional<int> o;
        std::printf("fake=%d\n", o.value());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "star") == 0) {
        std::optional<int> o;
        std::printf("fake=%d\n", *o);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "arrow") == 0) {
        std::optional<P> o;
        std::printf("fake=%d\n", o->a);
        return 0;
    }
    show();
    return 0;
}
