// Differential trace of std::optional::and_then
// (tests/cpp/test_cxx_optional.cpp): compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/optional) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical.
//
// Engaged and_then calls the function with the stored T and returns what it
// returns. Disengaged and_then does not call the function and returns a
// disengaged optional. It does not return T{}.
//
// emplace is not called: the model does not declare it.
// transform and or_else are declared; this trace does not call them.
#include <cstdio>
#include <cstdlib>
#include <optional>

// Native stand-in for the PIR intrinsic. value() on an engaged optional calls
// it and continues. A failing check aborts before a fake T can be printed.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

// No default constructor: a disengaged and_then must not value-initialise Box.
struct Box {
    int v;
    Box() = delete;
    explicit Box(int x) : v(x) {}
};

// One overload per value category, so each and_then ref-qualifier is visible.
struct Cat {
    std::optional<int> operator()(int&) {
        std::printf("cat-lv\n");
        return std::optional<int>(1);
    }
    std::optional<int> operator()(const int&) {
        std::printf("cat-clv\n");
        return std::optional<int>(3);
    }
    std::optional<int> operator()(int&&) {
        std::printf("cat-rv\n");
        return std::optional<int>(2);
    }
    std::optional<int> operator()(const int&&) {
        std::printf("cat-crv\n");
        return std::optional<int>(4);
    }
};

static int calls = 0;

static std::optional<int> add1(int v) {
    ++calls;
    return std::optional<int>(v + 1);
}

static std::optional<int> make_empty(int v) {
    ++calls;
    std::printf("make_empty %d\n", v);
    return std::optional<int>();
}

static std::optional<Box> to_box(int v) {
    ++calls;
    return std::optional<Box>(Box(v));
}

int main() {
    std::optional<int> engaged(42);
    std::optional<int> empty;

    auto a = engaged.and_then(add1);
    std::printf("engaged %d %d\n", static_cast<int>(a.has_value()), a.value());
    auto b = empty.and_then(add1);
    std::printf("empty %d\n", static_cast<int>(b.has_value()));
    std::printf("calls %d\n", calls);

    calls = 0;
    auto c = engaged.and_then(make_empty);
    std::printf("cleared %d\n", static_cast<int>(c.has_value()));
    auto d = empty.and_then(make_empty);
    std::printf("still %d\n", static_cast<int>(d.has_value()));
    std::printf("calls2 %d\n", calls);

    auto e = engaged.and_then(to_box);
    std::printf("held %d %d\n", static_cast<int>(e.has_value()), e.value().v);
    auto f = empty.and_then(to_box);
    std::printf("none %d\n", static_cast<int>(f.has_value()));

    auto z = engaged.and_then([](int) { return std::optional<int>(0); });
    std::printf("zero %d %d\n", static_cast<int>(z.has_value()), z.value());
    auto ze = empty.and_then([](int v) {
        std::printf("badzero %d\n", v);
        return std::optional<int>(0);
    });
    std::printf("zeroE %d\n", static_cast<int>(ze.has_value()));

    calls = 0;
    auto chain = std::optional<int>(42).and_then(add1).and_then(add1);
    std::printf("chain %d %d\n", static_cast<int>(chain.has_value()), chain.value());
    auto chain_e = std::optional<int>().and_then(add1).and_then(add1);
    std::printf("chainE %d\n", static_cast<int>(chain_e.has_value()));
    std::printf("calls3 %d\n", calls);

    Cat cat;
    std::optional<int> lv(42);
    std::printf("lv %d\n", lv.and_then(cat).value());
    const std::optional<int> clv(42);
    std::printf("clv %d\n", clv.and_then(cat).value());
    std::printf("rv %d\n", std::optional<int>(42).and_then(cat).value());
    const std::optional<int> crv(42);
    std::printf("crv %d\n", static_cast<const std::optional<int>&&>(crv).and_then(cat).value());

    std::optional<int> elv;
    std::printf("elv %d\n", static_cast<int>(elv.and_then(cat).has_value()));
    const std::optional<int> eclv;
    std::printf("eclv %d\n", static_cast<int>(eclv.and_then(cat).has_value()));
    std::printf("erv %d\n", static_cast<int>(std::optional<int>().and_then(cat).has_value()));
    const std::optional<int> ecrv;
    std::printf("ecrv %d\n",
                static_cast<int>(static_cast<const std::optional<int>&&>(ecrv).and_then(cat).has_value()));

    std::optional<Box> held(Box(42));
    std::optional<Box> nobox;
    auto hb = held.and_then([](const Box& bx) { return std::optional<int>(bx.v + 1); });
    std::printf("box %d %d\n", static_cast<int>(hb.has_value()), hb.value());
    auto nb = nobox.and_then([](const Box& bx) {
        std::printf("badbox %d\n", bx.v);
        return std::optional<int>(0);
    });
    std::printf("boxE %d\n", static_cast<int>(nb.has_value()));
    return 0;
}
