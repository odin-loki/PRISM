// Differential trace of std::optional::or_else
// (tests/cpp/test_cxx_optional.cpp): compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/optional) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical.
//
// Disengaged or_else calls the function with no arguments and returns that
// optional. Engaged or_else does not call the function and returns an optional
// that holds the stored T. It does not invent a different T.
//
// emplace is not called: the model does not declare it.
// transform is declared; this trace does not call it.
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

// No default constructor: an engaged or_else must copy or move this Box.
// It must not value-initialise one.
struct Box {
    int v;
    Box() = delete;
    explicit Box(int x) : v(x) {}
    Box(const Box& o) : v(o.v) {}
    Box(Box&& o) noexcept : v(o.v) { o.v = -1; }
};

// One overload per value category, so forwarding of the function is visible.
// or_else calls it with no arguments.
struct Call {
    std::optional<int> operator()() & {
        std::printf("call-lv\n");
        return std::optional<int>(1);
    }
    std::optional<int> operator()() const& {
        std::printf("call-clv\n");
        return std::optional<int>(3);
    }
    std::optional<int> operator()() && {
        std::printf("call-rv\n");
        return std::optional<int>(2);
    }
    std::optional<int> operator()() const&& {
        std::printf("call-crv\n");
        return std::optional<int>(4);
    }
};

static int calls = 0;

static std::optional<int> make7() {
    ++calls;
    return std::optional<int>(7);
}

static std::optional<int> make_empty() {
    ++calls;
    std::printf("make_empty\n");
    return std::optional<int>();
}

int main() {
    std::optional<int> engaged(42);
    std::optional<int> empty;

    auto a = engaged.or_else(make7);
    std::printf("engaged %d %d\n", static_cast<int>(a.has_value()), a.value());
    auto b = empty.or_else(make7);
    std::printf("empty %d %d\n", static_cast<int>(b.has_value()), b.value());
    std::printf("calls %d\n", calls);

    // A stored 0 is a T. It is not a reason to call the function.
    calls = 0;
    auto z = std::optional<int>(0).or_else(make7);
    std::printf("zero %d %d\n", static_cast<int>(z.has_value()), z.value());
    auto ze = std::optional<int>().or_else([] {
        std::printf("gotzero\n");
        return std::optional<int>(0);
    });
    std::printf("zeroE %d %d\n", static_cast<int>(ze.has_value()), ze.value());
    std::printf("calls2 %d\n", calls);

    calls = 0;
    auto c = engaged.or_else(make_empty);
    std::printf("kept %d %d\n", static_cast<int>(c.has_value()), c.value());
    auto d = empty.or_else(make_empty);
    std::printf("cleared %d\n", static_cast<int>(d.has_value()));
    std::printf("calls3 %d\n", calls);

    // The recovered optional is engaged, so the second function is not called.
    calls = 0;
    auto chain = std::optional<int>().or_else(make7).or_else([] {
        std::printf("second\n");
        return std::optional<int>(8);
    });
    std::printf("chain %d %d\n", static_cast<int>(chain.has_value()), chain.value());
    auto chain_e = std::optional<int>(42).or_else(make7).or_else(make7);
    std::printf("chainE %d %d\n", static_cast<int>(chain_e.has_value()), chain_e.value());
    std::printf("calls4 %d\n", calls);

    Call call;
    const Call ccall;
    std::optional<int> elv;
    std::printf("lv %d\n", elv.or_else(call).value());
    const std::optional<int> eclv;
    std::printf("clv %d\n", eclv.or_else(ccall).value());
    std::printf("rv %d\n", std::optional<int>().or_else(Call{}).value());
    const std::optional<int> ecrv;
    std::printf("crv %d\n", static_cast<const std::optional<int>&&>(ecrv)
                                .or_else(static_cast<const Call&&>(ccall))
                                .value());

    std::optional<int> lv(42);
    std::printf("keep-lv %d\n", lv.or_else(call).value());
    const std::optional<int> clv(42);
    std::printf("keep-clv %d\n", clv.or_else(ccall).value());
    std::printf("keep-rv %d\n", std::optional<int>(42).or_else(Call{}).value());
    const std::optional<int> crv(42);
    std::printf("keep-crv %d\n", static_cast<const std::optional<int>&&>(crv)
                                     .or_else(static_cast<const Call&&>(ccall))
                                     .value());

    std::optional<Box> held(Box(42));
    auto hb = held.or_else([] {
        std::printf("badbox\n");
        return std::optional<Box>(Box(0));
    });
    std::printf("box %d %d %d\n", static_cast<int>(hb.has_value()), hb.value().v, held.value().v);

    std::optional<Box> nobox;
    auto nb = nobox.or_else([] {
        std::printf("makebox\n");
        return std::optional<Box>(Box(9));
    });
    std::printf("boxE %d %d\n", static_cast<int>(nb.has_value()), nb.value().v);
    auto none = std::optional<Box>().or_else([] { return std::optional<Box>(); });
    std::printf("boxnone %d\n", static_cast<int>(none.has_value()));

    auto moved = static_cast<std::optional<Box>&&>(held).or_else([] {
        std::printf("badmove\n");
        return std::optional<Box>(Box(0));
    });
    std::printf("moved %d %d\n", moved.value().v, held.value().v);

    const std::optional<Box> cheld(Box(5));
    auto cm = static_cast<const std::optional<Box>&&>(cheld).or_else([] {
        std::printf("badcmove\n");
        return std::optional<Box>(Box(0));
    });
    std::printf("cmove %d %d\n", cm.value().v, cheld.value().v);
    return 0;
}
