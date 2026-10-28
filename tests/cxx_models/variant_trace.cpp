// Differential trace of std::variant (tests/cpp/test_cxx_variant.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/variant) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// visit, swap, and comparison of variant are not called: the model does not
// declare them. Two std::monostate values compare equal (!= is false).
//
// argv "bad" / "badi" call std::get<T> / std::get<I> on an inactive alternative.
// The model must __prism_check and must not print a value.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <variant>

static_assert(std::is_empty_v<std::monostate>);
static_assert(std::monostate{} == std::monostate{});
static_assert(!(std::monostate{} != std::monostate{}));

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned T. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

// No default constructor: a model that invents a T cannot value-initialise E.
struct E {
    int v;
    explicit E(int x) : v(x) { std::printf(" c%d", v); }
    E(const E& o) : v(o.v) { std::printf(" cc%d", v); }
    E(E&& o) : v(o.v) { std::printf(" m%d", v); }
    E& operator=(const E& o) {
        std::printf(" =%d", o.v);
        v = o.v;
        return *this;
    }
    E& operator=(E&& o) {
        std::printf(" =m%d", o.v);
        v = o.v;
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
};

static void show() {
    std::variant<int, char> v;
    std::printf("def index=%zu valueless=%d get=%d\n", v.index(), static_cast<int>(v.valueless_by_exception()),
                std::get<int>(v));
    std::printf("def_if int=%d char_null=%d\n", std::get_if<int>(&v) ? *std::get_if<int>(&v) : -1,
                std::get_if<char>(&v) == nullptr ? 1 : 0);

    v.emplace<int>(42);
    std::printf("index=%zu stored=%d inactive_null=%d active_ptr=%d valueless=%d\n", v.index(), std::get<int>(v),
                std::get_if<char>(&v) == nullptr ? 1 : 0, std::get_if<int>(&v) ? *std::get_if<int>(&v) : -1,
                static_cast<int>(v.valueless_by_exception()));
    std::printf("holds int=%d char=%d get0=%d\n", static_cast<int>(std::holds_alternative<int>(v)),
                static_cast<int>(std::holds_alternative<char>(v)), std::get<0>(v));

    v.emplace<char>('A');
    std::printf("emc index=%zu get=%d int_null=%d char_ptr=%d\n", v.index(), static_cast<int>(std::get<char>(v)),
                std::get_if<int>(&v) == nullptr ? 1 : 0,
                std::get_if<char>(&v) ? static_cast<int>(*std::get_if<char>(&v)) : -1);

    v = 42;
    std::printf("asi index=%zu stored=%d getT=%d\n", v.index(), std::get<0>(v), std::get<int>(v));

    std::variant<int, char> copy(v);
    std::printf("copy %d %zu\n", std::get<int>(copy), copy.index());
    std::variant<int, char> moved(static_cast<std::variant<int, char>&&>(copy));
    std::printf("moved %d %zu src=%zu\n", std::get<int>(moved), moved.index(), copy.index());

    v.emplace<1>('B');
    std::printf("em1 %d %zu holds_char=%d\n", static_cast<int>(std::get<1>(v)), v.index(),
                static_cast<int>(std::holds_alternative<char>(v)));

    std::printf("traced");
    {
        std::variant<int, E> e;
        e.emplace<E>(5);
        std::printf(" held=%d", std::get<E>(e).v);
        e.emplace<int>(7);
        std::printf(" now=%d %zu", std::get<int>(e), e.index());
        e.emplace<E>(9);
        std::printf(" back=%d", std::get<E>(e).v);
        e = E(4);
        std::printf(" as=%d", std::get<E>(e).v);
    }
    std::printf("\n");

    std::monostate ma;
    std::monostate mb;
    std::printf("mono eq=%d ne=%d\n", static_cast<int>(ma == mb), static_cast<int>(ma != mb));
    {
        std::variant<std::monostate, int> mv;
        std::printf("mono_var index=%zu holds=%d geteq=%d getne=%d valueless=%d\n", mv.index(),
                    static_cast<int>(std::holds_alternative<std::monostate>(mv)),
                    static_cast<int>(std::get<std::monostate>(mv) == ma),
                    static_cast<int>(std::get<std::monostate>(mv) != mb),
                    static_cast<int>(mv.valueless_by_exception()));
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "bad") == 0) {
        std::variant<int, char> v;
        v.emplace<int>(1);
        std::printf("fake=%d\n", static_cast<int>(std::get<char>(v)));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "badi") == 0) {
        std::variant<int, char> v;
        v.emplace<char>('A');
        std::printf("fake=%d\n", std::get<0>(v));
        return 0;
    }
    show();
    return 0;
}
