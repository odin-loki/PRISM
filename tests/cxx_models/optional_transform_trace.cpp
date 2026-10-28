// Differential trace of std::optional::transform
// (tests/cpp/test_cxx_optional.cpp): compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/optional) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical.
//
// Engaged transform calls the function with the stored T and returns an
// optional that holds what the function returns. Disengaged transform does
// not call the function and returns a disengaged optional. It does not
// return T{}.
//
// emplace is not called: the model does not declare it.
// and_then and or_else are declared; this trace does not call them.
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

// No default constructor: a disengaged transform must not value-initialise Box.
// The move constructor marks the source so an rvalue argument is visible.
struct Box {
    int v;
    Box() = delete;
    explicit Box(int x) : v(x) {}
    Box(const Box& o) : v(o.v) {}
    Box(Box&& o) noexcept : v(o.v) { o.v = -1; }
};

// One overload per value category, so each transform ref-qualifier is visible.
// The result is an int, not an optional: transform has to wrap it.
struct Cat {
    int operator()(int&) {
        std::printf("cat-lv\n");
        return 1;
    }
    int operator()(const int&) {
        std::printf("cat-clv\n");
        return 3;
    }
    int operator()(int&&) {
        std::printf("cat-rv\n");
        return 2;
    }
    int operator()(const int&&) {
        std::printf("cat-crv\n");
        return 4;
    }
};

static int calls = 0;

static int add1(int v) {
    ++calls;
    return v + 1;
}

static Box to_box(int v) {
    std::printf("to_box %d\n", v);
    return Box(v);
}

int main() {
    std::optional<int> engaged(42);
    std::optional<int> empty;

    auto a = engaged.transform(add1);
    std::printf("engaged %d %d\n", static_cast<int>(a.has_value()), a.value());
    auto b = empty.transform(add1);
    std::printf("empty %d\n", static_cast<int>(b.has_value()));
    std::printf("calls %d\n", calls);

    calls = 0;
    auto z0 = std::optional<int>(0).transform(add1);
    std::printf("zeroIn %d %d\n", static_cast<int>(z0.has_value()), z0.value());
    std::printf("calls2 %d\n", calls);

    auto z = engaged.transform([](int) { return 0; });
    std::printf("zero %d %d\n", static_cast<int>(z.has_value()), z.value());
    auto ze = empty.transform([](int v) {
        std::printf("badzero %d\n", v);
        return 0;
    });
    std::printf("zeroE %d\n", static_cast<int>(ze.has_value()));

    auto np = engaged.transform([](int) -> int* { return nullptr; });
    std::printf("nullp %d %d\n", static_cast<int>(np.has_value()),
                static_cast<int>(np.value() == nullptr));
    auto npe = empty.transform([](int v) -> int* {
        std::printf("badnull %d\n", v);
        return nullptr;
    });
    std::printf("nullpE %d\n", static_cast<int>(npe.has_value()));

    auto e = engaged.transform(to_box);
    std::printf("held %d %d\n", static_cast<int>(e.has_value()), e.value().v);
    auto f = empty.transform(to_box);
    std::printf("none %d\n", static_cast<int>(f.has_value()));

    auto cv = engaged.transform([](int v) -> const int { return v + 1; });
    int& cvref = cv.value();
    std::printf("cv %d\n", cvref);
    auto cve = empty.transform([](int v) -> const int {
        std::printf("badcv %d\n", v);
        return 0;
    });
    std::printf("cvE %d\n", static_cast<int>(cve.has_value()));

    auto w = engaged.transform([](int v) { return std::optional<int>(v + 1); });
    std::printf("wrap %d %d %d\n", static_cast<int>(w.has_value()),
                static_cast<int>(w.value().has_value()), w.value().value());
    auto we = empty.transform([](int v) {
        std::printf("badwrap %d\n", v);
        return std::optional<int>(7);
    });
    std::printf("wrapE %d\n", static_cast<int>(we.has_value()));

    calls = 0;
    auto chain = std::optional<int>(42).transform(add1).transform(add1);
    std::printf("chain %d %d\n", static_cast<int>(chain.has_value()), chain.value());
    auto chain_e = std::optional<int>().transform(add1).transform(add1);
    std::printf("chainE %d\n", static_cast<int>(chain_e.has_value()));
    std::printf("calls3 %d\n", calls);

    Cat cat;
    std::optional<int> lv(42);
    std::printf("lv %d\n", lv.transform(cat).value());
    const std::optional<int> clv(42);
    std::printf("clv %d\n", clv.transform(cat).value());
    std::printf("rv %d\n", std::optional<int>(42).transform(cat).value());
    const std::optional<int> crv(42);
    std::printf("crv %d\n", static_cast<const std::optional<int>&&>(crv).transform(cat).value());

    std::optional<int> elv;
    std::printf("elv %d\n", static_cast<int>(elv.transform(cat).has_value()));
    const std::optional<int> eclv;
    std::printf("eclv %d\n", static_cast<int>(eclv.transform(cat).has_value()));
    std::printf("erv %d\n", static_cast<int>(std::optional<int>().transform(cat).has_value()));
    const std::optional<int> ecrv;
    std::printf("ecrv %d\n",
                static_cast<int>(static_cast<const std::optional<int>&&>(ecrv).transform(cat).has_value()));

    std::optional<Box> held(Box(42));
    auto hb = held.transform([](Box& bx) { return bx.v + 1; });
    std::printf("box %d %d %d\n", static_cast<int>(hb.has_value()), hb.value(), held.value().v);

    std::optional<Box> nobox;
    auto nb = nobox.transform([](Box& bx) {
        std::printf("badbox %d\n", bx.v);
        return 0;
    });
    std::printf("boxE %d\n", static_cast<int>(nb.has_value()));

    auto moved = static_cast<std::optional<Box>&&>(held).transform([](Box&& bx) {
        int n = bx.v;
        Box sink(static_cast<Box&&>(bx));
        return n;
    });
    std::printf("moved %d %d\n", moved.value(), held.value().v);

    const std::optional<Box> cheld(Box(5));
    auto cm = static_cast<const std::optional<Box>&&>(cheld).transform([](const Box&& bx) { return bx.v; });
    std::printf("cmove %d %d\n", cm.value(), cheld.value().v);
    return 0;
}
