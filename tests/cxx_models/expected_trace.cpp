// Differential trace of std::expected (tests/cpp/test_cxx_expected.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/expected) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// and_then calls the function with the stored T and returns what it returns.
// On an unexpected it does not call the function; the result holds that same
// E and does not invent a T. or_else calls the function with the stored E.
// On a value it does not call the function; the result holds that stored T.
// value_or returns the stored T when that alternative is active, and the
// argument converted to T when an E is stored. It does not return T{}.
// transform calls the function with the stored T and returns an expected
// that holds the function's result. On an unexpected it does not call the
// function; the result holds that same E and does not construct a T or
// return T{}.
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

// No default constructor: value_or(99) has to convert 99. T{} would not compile.
struct Box {
    int v;
    Box() = delete;
    Box(int x) : v(x) {}
};

static void show_monadic();
static void show_value_or();
static void show_transform();

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

    show_monadic();
    show_value_or();
    show_transform();
}

// and_then / or_else. Lines are the contract: a value calls and_then and does
// not call or_else; an unexpected calls or_else and does not call and_then.
// E and R have no default constructor, so inventing a T or an E would not
// compile (and a call that should be skipped prints callT / callR).
static void show_monadic() {
    std::expected<int, int> ok(42);
    auto doubled = ok.and_then([](int v) { return std::expected<int, int>(v * 2); });
    std::printf("and_then %d %d\n", static_cast<int>(doubled.has_value()), doubled.value());

    int and_calls = 0;
    std::expected<int, int> bad(std::unexpected<int>(9));
    auto skipped = bad.and_then([&](int v) {
        ++and_calls;
        return std::expected<int, int>(v);
    });
    std::printf("and_then_unex %d %d %d\n", and_calls, static_cast<int>(skipped.has_value()),
                skipped.error());

    int or_calls = 0;
    auto kept = ok.or_else([&](int e) {
        ++or_calls;
        return std::expected<int, int>(std::unexpected<int>(e));
    });
    std::printf("or_else %d %d %d\n", or_calls, static_cast<int>(kept.has_value()), kept.value());

    auto recovered = bad.or_else([](int e) { return std::expected<int, int>(e + 2); });
    std::printf("or_else_unex %d %d\n", static_cast<int>(recovered.has_value()), recovered.value());

    std::expected<E, int> no_t(std::unexpected<int>(4));
    auto nt = no_t.and_then([](const E& x) {
        std::printf("callT %d\n", x.v);
        return std::expected<E, int>(x);
    });
    std::printf("and_then_not %d %d\n", static_cast<int>(nt.has_value()), nt.error());

    std::expected<int, R> no_e(42);
    auto ne = no_e.or_else([](const R& r) {
        std::printf("callR %d\n", r.v);
        return std::expected<int, R>(std::unexpected<R>(r));
    });
    std::printf("or_else_not %d %d\n", static_cast<int>(ne.has_value()), ne.value());

    std::expected<E, int> held_m(E(6));
    auto ht = held_m.and_then([](const E& x) {
        std::printf("got %d\n", x.v);
        return std::expected<int, int>(x.v);
    });
    std::printf("and_then_val %d\n", ht.value());

    std::expected<E, int> ok_e(E(2));
    auto oe = ok_e.or_else([](int err) {
        std::printf("callEo %d\n", err);
        return std::expected<E, int>(std::unexpected<int>(err));
    });
    std::printf("or_else_e %d %d\n", static_cast<int>(oe.has_value()), oe.value().v);

    std::expected<int, R> unex_r(std::unexpected<R>(R(3)));
    auto ar = unex_r.and_then([](int v) {
        std::printf("callRu %d\n", v);
        return std::expected<int, R>(v);
    });
    std::printf("and_then_r %d %d\n", static_cast<int>(ar.has_value()), ar.error().v);

    const std::expected<int, int> cok(42);
    auto ca = cok.and_then([](int v) { return std::expected<int, int>(v + 3); });
    std::printf("and_then_const %d\n", ca.value());

    const std::expected<int, int> cbad(std::unexpected<int>(2));
    auto co = cbad.or_else([](int e) { return std::expected<int, int>(e + 8); });
    std::printf("or_else_const %d\n", co.value());

    auto rv = std::expected<int, int>(11).and_then(
        [](int v) { return std::expected<int, int>(v + 4); });
    std::printf("and_then_rv %d\n", rv.value());

    auto rvb = std::expected<int, int>(std::unexpected<int>(6)).or_else(
        [](int e) { return std::expected<int, int>(e + 1); });
    std::printf("or_else_rv %d\n", rvb.value());

    auto rve = std::expected<int, int>(std::unexpected<int>(6)).and_then([](int v) {
        std::printf("callRv %d\n", v);
        return std::expected<int, int>(v);
    });
    std::printf("and_then_rv_unex %d %d\n", static_cast<int>(rve.has_value()), rve.error());

    auto rvo = std::expected<int, int>(8).or_else([](int e) {
        std::printf("callOr %d\n", e);
        return std::expected<int, int>(std::unexpected<int>(e));
    });
    std::printf("or_else_rv_val %d %d\n", static_cast<int>(rvo.has_value()), rvo.value());

    const std::expected<int, int> cok_unex(std::unexpected<int>(4));
    auto cmv = static_cast<const std::expected<int, int>&&>(cok_unex).and_then([](int v) {
        std::printf("callCmv %d\n", v);
        return std::expected<int, int>(v);
    });
    std::printf("and_then_cmv %d %d\n", static_cast<int>(cmv.has_value()), cmv.error());

    const std::expected<int, int> cok2(8);
    auto cmvo = static_cast<const std::expected<int, int>&&>(cok2).or_else([](int e) {
        std::printf("callCOr %d\n", e);
        return std::expected<int, int>(std::unexpected<int>(e));
    });
    std::printf("or_else_cmv %d %d\n", static_cast<int>(cmvo.has_value()), cmvo.value());
}

// value_or. A stored T is that T, including 0. An unexpected is the argument
// converted to T (99), not the stored E and not a value-initialized T. Box
// has no default constructor, so T{} would not compile.
static void show_value_or() {
    std::expected<int, int> ok(42);
    std::printf("value_or %d\n", ok.value_or(99));

    std::expected<int, int> bad(std::unexpected<int>(9));
    std::printf("value_or_unex %d\n", bad.value_or(99));

    std::printf("value_or_rv %d\n", std::expected<int, int>(42).value_or(99));
    std::printf("value_or_rv_unex %d\n",
                std::expected<int, int>(std::unexpected<int>(7)).value_or(99));

    std::expected<int, int> zero(0);
    std::printf("value_or_zero %d\n", zero.value_or(99));

    const std::expected<int, int> cok(42);
    std::printf("value_or_const %d\n", cok.value_or(99));
    const std::expected<int, int> cbad(std::unexpected<int>(3));
    std::printf("value_or_const_unex %d\n", cbad.value_or(99));

    std::expected<Box, int> held(Box(42));
    std::printf("value_or_held %d\n", held.value_or(99).v);
    std::expected<Box, int> none(std::unexpected<int>(4));
    std::printf("value_or_none %d\n", none.value_or(99).v);
}

// transform. A stored T is passed to the function; the result holds what the
// function returns. An unexpected does not call the function and holds that
// same E. E and Box have no default constructor, so a T{} on those paths
// would not compile. A skipped call prints skipEt / skipRv / skipCmv /
// skipZ / skipEr.
static void show_transform() {
    std::expected<int, int> ok(42);
    auto doubled = ok.transform([](int v) { return v * 2; });
    std::printf("transform %d %d\n", static_cast<int>(doubled.has_value()), doubled.value());

    int xcalls = 0;
    std::expected<int, int> bad(std::unexpected<int>(9));
    auto skipped = bad.transform([&](int v) {
        ++xcalls;
        return v;
    });
    std::printf("transform_unex %d %d %d\n", xcalls, static_cast<int>(skipped.has_value()),
                skipped.error());
    std::printf("transform_src %d\n", bad.error());

    std::expected<int, int> zero(0);
    auto z = zero.transform([](int v) { return v; });
    std::printf("transform_zero %d %d\n", static_cast<int>(z.has_value()), z.value());

    std::expected<int, int> zerr(std::unexpected<int>(0));
    auto zt = zerr.transform([](int v) {
        std::printf("skipZ %d\n", v);
        return 99;
    });
    std::printf("transform_err0 %d %d\n", static_cast<int>(zt.has_value()), zt.error());

    const std::expected<int, int> cok(42);
    auto ca = cok.transform([](int v) { return v + 3; });
    std::printf("transform_const %d\n", ca.value());

    int cucalls = 0;
    const std::expected<int, int> cbad(std::unexpected<int>(3));
    auto cu = cbad.transform([&](int v) {
        ++cucalls;
        return v;
    });
    std::printf("transform_const_unex %d %d %d\n", cucalls, static_cast<int>(cu.has_value()),
                cu.error());

    auto rv = std::expected<int, int>(11).transform([](int v) { return v + 4; });
    std::printf("transform_rv %d\n", rv.value());

    auto rve = std::expected<int, int>(std::unexpected<int>(6)).transform([](int v) {
        std::printf("skipRv %d\n", v);
        return v;
    });
    std::printf("transform_rv_unex %d %d\n", static_cast<int>(rve.has_value()), rve.error());

    const std::expected<int, int> cok2(8);
    auto cmvo = static_cast<const std::expected<int, int>&&>(cok2).transform(
        [](int v) { return v + 1; });
    std::printf("transform_cmv_val %d %d\n", static_cast<int>(cmvo.has_value()), cmvo.value());

    const std::expected<int, int> cok_unex(std::unexpected<int>(4));
    auto cmv = static_cast<const std::expected<int, int>&&>(cok_unex).transform([](int v) {
        std::printf("skipCmv %d\n", v);
        return v;
    });
    std::printf("transform_cmv %d %d\n", static_cast<int>(cmv.has_value()), cmv.error());

    std::expected<E, int> no_t(std::unexpected<int>(4));
    auto nt = no_t.transform([](const E& x) {
        std::printf("skipEt %d\n", x.v);
        return x.v;
    });
    std::printf("transform_not %d %d\n", static_cast<int>(nt.has_value()), nt.error());

    std::expected<E, int> held_m(E(6));
    auto ht = held_m.transform([](const E& x) {
        std::printf("xform %d\n", x.v);
        return x.v;
    });
    std::printf("transform_val %d %d\n", static_cast<int>(ht.has_value()), ht.value());

    auto box = ok.transform([](int v) { return Box(v); });
    std::printf("transform_box %d\n", box.value().v);

    int bcalls = 0;
    auto bnone = bad.transform([&](int v) {
        ++bcalls;
        return Box(v);
    });
    std::printf("transform_box_unex %d %d %d\n", bcalls, static_cast<int>(bnone.has_value()),
                bnone.error());

    std::expected<int, R> unex_r(std::unexpected<R>(R(3)));
    auto ar = unex_r.transform([](int v) {
        std::printf("skipEr %d\n", v);
        return v + 1;
    });
    std::printf("transform_r %d %d\n", static_cast<int>(ar.has_value()), ar.error().v);
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
