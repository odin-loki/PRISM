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
// transform_error calls the function with the stored E and returns an
// expected that holds the function's result as the error. On a value it
// does not call the function; the result holds that same T and does not
// construct an E or return E{} or T{}.
// operator== compares two expected<T, E> of the same type. Both values
// compare the stored T. Both errors compare the stored E. A value compared
// with an error is false and does not call either operator==. operator!=
// is the negation. This is not PROVED and not CLEAN.
// emplace takes one argument. When the expected holds a T, that T is
// destroyed first and E is not read. When it holds an E, that E is destroyed
// first and T is not read. Then T is constructed in the value storage.
// operator< is declared; this trace does not call it. libstdc++ does not
// provide operator< for expected, so a call here would not match.
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

// No default constructor. operator== prints, so a read of the inactive
// alternative shows up. T{} or E{} would not compile.
struct EqVal {
    int v;
    EqVal() = delete;
    explicit EqVal(int x) : v(x) {}
    friend bool operator==(const EqVal& a, const EqVal& b) {
        std::printf("eqv %d %d\n", a.v, b.v);
        return a.v == b.v;
    }
};

struct EqErr {
    int v;
    EqErr() = delete;
    explicit EqErr(int x) : v(x) {}
    friend bool operator==(const EqErr& a, const EqErr& b) {
        std::printf("eqe %d %d\n", a.v, b.v);
        return a.v == b.v;
    }
};

// Move-only. No default constructor, so emplace cannot build EmMove{}. An
// engaged emplace destroys the stored EmMove, then move-constructs the new one.
// A value emplace must not run ~EmErr.
struct EmMove {
    int v;
    EmMove() = delete;
    explicit EmMove(int x) noexcept : v(x) { std::printf("mvc%d\n", v); }
    EmMove(const EmMove&) = delete;
    EmMove(EmMove&& o) noexcept : v(o.v) {
        o.v = -1;
        std::printf("mvm%d\n", v);
    }
    ~EmMove() { std::printf("mvd%d\n", v); }
};

// Move-only error. No default constructor, so emplace cannot build EmErr{}.
// An error emplace destroys this EmErr and must not run ~EmMove first.
struct EmErr {
    int v;
    EmErr() = delete;
    explicit EmErr(int x) noexcept : v(x) { std::printf("erc%d\n", v); }
    EmErr(const EmErr&) = delete;
    EmErr(EmErr&& o) noexcept : v(o.v) {
        o.v = -1;
        std::printf("erm%d\n", v);
    }
    ~EmErr() { std::printf("erd%d\n", v); }
};

// Copy and move both print. An lvalue argument is copied; an rvalue is moved.
// No default constructor. The copy constructor is noexcept so libstdc++
// emplace's nothrow constraint accepts it.
struct EmCopy {
    int v;
    EmCopy() = delete;
    explicit EmCopy(int x) noexcept : v(x) { std::printf("cpc%d\n", v); }
    EmCopy(const EmCopy& o) noexcept : v(o.v) { std::printf("cpp%d\n", v); }
    EmCopy(EmCopy&& o) noexcept : v(o.v) {
        o.v = -1;
        std::printf("cpm%d\n", v);
    }
    ~EmCopy() { std::printf("cpd%d\n", v); }
};

// Not movable and not copyable. emplace(int) has to construct in the storage.
struct EmBox {
    int v;
    EmBox() = delete;
    explicit EmBox(int x) noexcept : v(x) { std::printf("bxc%d\n", v); }
    EmBox(const EmBox&) = delete;
    EmBox(EmBox&&) = delete;
    ~EmBox() { std::printf("bxd%d\n", v); }
};

static void show_monadic();
static void show_value_or();
static void show_transform();
static void show_transform_error();
static void show_equal();
static void show_emplace();

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
    show_transform_error();
    show_equal();
    show_emplace();
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

// Passed the stored E in the value category of that overload. The four
// results differ, so a cast to the wrong category cannot match every line.
struct ErrCat {
    int operator()(int& e) const { return e + 10; }
    int operator()(const int& e) const { return e + 20; }
    int operator()(int&& e) const { return e + 30; }
    int operator()(const int&& e) const { return e + 40; }
};

// transform_error. An unexpected is passed to the function; the result holds
// what the function returns, as the error. A value does not call the function
// and holds that same T. Box and R have no default constructor, so E{} or T{}
// on those paths would not compile. A skipped call prints skipZt / skipTeC /
// skipRvT / skipCmvT / skipBox / callTe.
static void show_transform_error() {
    std::expected<int, int> bad(std::unexpected<int>(9));
    auto mapped = bad.transform_error([](int e) { return e * 2; });
    std::printf("transform_error %d %d\n", static_cast<int>(mapped.has_value()), mapped.error());
    std::printf("transform_error_src_err %d\n", bad.error());

    int vcalls = 0;
    std::expected<int, int> ok(42);
    auto kept = ok.transform_error([&](int e) {
        ++vcalls;
        return e;
    });
    std::printf("transform_error_val %d %d %d\n", vcalls, static_cast<int>(kept.has_value()),
                kept.value());
    std::printf("transform_error_src %d\n", ok.value());

    std::expected<int, int> zero(0);
    auto z = zero.transform_error([](int e) {
        std::printf("skipZt %d\n", e);
        return 99;
    });
    std::printf("transform_error_zero %d %d\n", static_cast<int>(z.has_value()), z.value());

    std::expected<int, int> zerr(std::unexpected<int>(0));
    auto zt = zerr.transform_error([](int e) { return e + 99; });
    std::printf("transform_error_err0 %d %d\n", static_cast<int>(zt.has_value()), zt.error());

    std::expected<int, int> five(std::unexpected<int>(5));
    auto zf = five.transform_error([](int) { return 0; });
    std::printf("transform_error_zero_err %d %d\n", static_cast<int>(zf.has_value()), zf.error());

    const std::expected<int, int> cok(42);
    auto cval = cok.transform_error([](int e) {
        std::printf("skipTeC %d\n", e);
        return e;
    });
    std::printf("transform_error_const_val %d\n", cval.value());

    const std::expected<int, int> cbad(std::unexpected<int>(3));
    auto cu = cbad.transform_error([](int e) { return e + 7; });
    std::printf("transform_error_const %d %d\n", static_cast<int>(cu.has_value()), cu.error());

    auto rv = std::expected<int, int>(11).transform_error([](int e) {
        std::printf("skipRvT %d\n", e);
        return e;
    });
    std::printf("transform_error_rv_val %d\n", rv.value());

    auto rve = std::expected<int, int>(std::unexpected<int>(6)).transform_error(
        [](int e) { return e + 1; });
    std::printf("transform_error_rv %d %d\n", static_cast<int>(rve.has_value()), rve.error());

    const std::expected<int, int> cok2(8);
    auto cmvo = static_cast<const std::expected<int, int>&&>(cok2).transform_error([](int e) {
        std::printf("skipCmvT %d\n", e);
        return e;
    });
    std::printf("transform_error_cmv_val %d %d\n", static_cast<int>(cmvo.has_value()), cmvo.value());

    const std::expected<int, int> cok_unex(std::unexpected<int>(4));
    auto cmv = static_cast<const std::expected<int, int>&&>(cok_unex).transform_error(
        [](int e) { return e + 1; });
    std::printf("transform_error_cmv %d %d\n", static_cast<int>(cmv.has_value()), cmv.error());

    std::expected<Box, int> none(std::unexpected<int>(4));
    auto nt = none.transform_error([](int e) { return e + 5; });
    std::printf("transform_error_not %d %d\n", static_cast<int>(nt.has_value()), nt.error());

    std::expected<Box, int> held(Box(6));
    auto ht = held.transform_error([](int e) {
        std::printf("skipBox %d\n", e);
        return e + 1;
    });
    std::printf("transform_error_held %d %d\n", static_cast<int>(ht.has_value()), ht.value().v);

    auto box = bad.transform_error([](int e) { return Box(e); });
    std::printf("transform_error_box %d\n", box.error().v);

    int bcalls = 0;
    auto bval = ok.transform_error([&](int e) {
        ++bcalls;
        return Box(e);
    });
    std::printf("transform_error_box_val %d %d\n", bcalls, bval.value());

    std::expected<int, R> unex_r(std::unexpected<R>(R(3)));
    auto ar = unex_r.transform_error([](const R& r) {
        std::printf("terr %d\n", r.v);
        return r.v + 1;
    });
    std::printf("transform_error_r %d %d\n", static_cast<int>(ar.has_value()), ar.error());

    std::expected<int, R> no_e(42);
    auto ne = no_e.transform_error([](const R& r) {
        std::printf("callTe %d\n", r.v);
        return R(r.v);
    });
    std::printf("transform_error_kept %d %d\n", static_cast<int>(ne.has_value()), ne.value());

    std::expected<int, int> cat(std::unexpected<int>(1));
    auto cl = cat.transform_error(ErrCat{});
    std::printf("transform_error_cat %d\n", cl.error());

    const std::expected<int, int> ccat(std::unexpected<int>(1));
    auto cc = ccat.transform_error(ErrCat{});
    std::printf("transform_error_cat_const %d\n", cc.error());

    auto cr = std::expected<int, int>(std::unexpected<int>(1)).transform_error(ErrCat{});
    std::printf("transform_error_cat_rv %d\n", cr.error());

    const std::expected<int, int> ccmv(std::unexpected<int>(1));
    auto cm = static_cast<const std::expected<int, int>&&>(ccmv).transform_error(ErrCat{});
    std::printf("transform_error_cat_cmv %d\n", cm.error());
}

// Same type only. Both values compare T and do not call E's operator==.
// Both errors compare E and do not call T's operator==. A value against an
// error is false and calls neither. operator!= is the negation, so it calls
// operator== once more on the pairs that share an alternative.
static void show_equal() {
    std::expected<int, int> a(42);
    std::expected<int, int> b(42);
    std::expected<int, int> c(7);
    std::printf("eq_val %d %d %d\n", static_cast<int>(a == b), static_cast<int>(a == c),
                static_cast<int>(a == a));
    std::printf("ne_val %d %d %d\n", static_cast<int>(a != b), static_cast<int>(a != c),
                static_cast<int>(a != a));

    std::expected<int, int> z(0);
    std::expected<int, int> z2(0);
    std::printf("eq_zero %d %d\n", static_cast<int>(z == z2), static_cast<int>(z != z2));

    std::expected<int, int> e1(std::unexpected<int>(7));
    std::expected<int, int> e2(std::unexpected<int>(7));
    std::expected<int, int> e3(std::unexpected<int>(3));
    std::printf("eq_err %d %d %d\n", static_cast<int>(e1 == e2), static_cast<int>(e1 == e3),
                static_cast<int>(e1 == e1));
    std::printf("ne_err %d %d %d\n", static_cast<int>(e1 != e2), static_cast<int>(e1 != e3),
                static_cast<int>(e1 != e1));

    std::expected<int, int> ez(std::unexpected<int>(0));
    std::expected<int, int> ez2(std::unexpected<int>(0));
    std::printf("eq_err0 %d %d\n", static_cast<int>(ez == ez2), static_cast<int>(ez != ez2));

    std::printf("eq_mix %d %d\n", static_cast<int>(a == e1), static_cast<int>(e1 == a));
    std::printf("ne_mix %d %d\n", static_cast<int>(a != e1), static_cast<int>(e1 != a));
    std::printf("eq_mix0 %d %d\n", static_cast<int>(z == ez), static_cast<int>(ez == z));

    const std::expected<int, int> ca(42);
    const std::expected<int, int> cb(7);
    const std::expected<int, int> ce(std::unexpected<int>(7));
    const std::expected<int, int> ce2(std::unexpected<int>(7));
    std::printf("eq_const %d %d %d %d\n", static_cast<int>(ca == ca), static_cast<int>(ca == cb),
                static_cast<int>(ce == ce2), static_cast<int>(ca == ce));

    std::expected<EqVal, EqErr> va(EqVal(3));
    std::expected<EqVal, EqErr> vb(EqVal(3));
    std::expected<EqVal, EqErr> vc(EqVal(4));
    const int same = static_cast<int>(va == vb);
    const int diff = static_cast<int>(va == vc);
    std::printf("eq_t %d %d\n", same, diff);

    std::expected<EqVal, EqErr> ea(std::unexpected<EqErr>(EqErr(5)));
    std::expected<EqVal, EqErr> eb(std::unexpected<EqErr>(EqErr(5)));
    std::expected<EqVal, EqErr> ec(std::unexpected<EqErr>(EqErr(6)));
    const int es = static_cast<int>(ea == eb);
    const int ed = static_cast<int>(ea == ec);
    std::printf("eq_e %d %d\n", es, ed);

    std::expected<EqVal, EqErr> mix_v(EqVal(8));
    std::expected<EqVal, EqErr> mix_e(std::unexpected<EqErr>(EqErr(2)));
    const int mv = static_cast<int>(mix_v == mix_e);
    const int me = static_cast<int>(mix_e == mix_v);
    std::printf("eq_cross %d %d\n", mv, me);

    const int ne_same = static_cast<int>(va != vb);
    std::printf("ne_t %d\n", ne_same);
    const int ne_diff = static_cast<int>(ea != ec);
    std::printf("ne_e %d\n", ne_diff);
    const int ne_mix = static_cast<int>(mix_v != mix_e);
    std::printf("ne_cross %d\n", ne_mix);
}

// One argument. A stored T is destroyed before the new T is constructed, and
// E is not read. A stored E is destroyed before the new T is constructed, and
// the previous T is not read. The reference is the stored T.
static void show_emplace() {
    {
        std::expected<EmMove, EmErr> o(EmMove(1));
        std::printf("start %d\n", static_cast<int>(o.has_value()));
        EmMove& r = o.emplace(EmMove(2));
        std::printf("one %d %d %d\n", static_cast<int>(o.has_value()), r.v,
                    static_cast<int>(&r == &*o));
        r.v = 6;
        std::printf("mut %d\n", (*o).v);
        EmMove& r2 = o.emplace(EmMove(3));
        std::printf("two %d %d %d\n", static_cast<int>(o.has_value()), r2.v,
                    static_cast<int>(&r2 == &*o));
    }
    {
        std::expected<EmMove, EmErr> o(std::unexpected<EmErr>(EmErr(4)));
        std::printf("emheld %d %d\n", static_cast<int>(o.has_value()), o.error().v);
        EmMove& r = o.emplace(EmMove(5));
        std::printf("from_err %d %d %d\n", static_cast<int>(o.has_value()), r.v,
                    static_cast<int>(&r == &*o));
    }
    {
        EmCopy src(3);
        std::expected<EmCopy, EmErr> o(src);
        EmCopy& r = o.emplace(src);
        std::printf("lv %d %d %d\n", src.v, r.v, static_cast<int>(&r == &*o));
        EmCopy& r2 = o.emplace(static_cast<EmCopy&&>(src));
        std::printf("rv %d %d %d\n", src.v, r2.v, static_cast<int>(&r2 == &*o));
    }
    {
        std::expected<int, int> e(std::unexpected<int>(7));
        int& ir = e.emplace(0);
        std::printf("zero %d %d %d\n", static_cast<int>(e.has_value()), ir,
                    static_cast<int>(&ir == &*e));
        ir = 4;
        std::printf("zmut %d\n", *e);
        int& ir2 = e.emplace(9);
        std::printf("nine %d %d\n", ir2, e.value());
    }
    {
        std::expected<EmBox, EmErr> b(std::unexpected<EmErr>(EmErr(8)));
        EmBox& br = b.emplace(42);
        std::printf("box %d %d %d\n", static_cast<int>(b.has_value()), br.v,
                    static_cast<int>(&br == &*b));
        EmBox& br2 = b.emplace(7);
        std::printf("box2 %d %d %d\n", static_cast<int>(b.has_value()), br2.v,
                    static_cast<int>(&br2 == &*b));
    }
    {
        std::expected<int, int> n(42);
        int& nr = n.emplace(3);
        std::printf("wasnow %d %d\n", nr, n.value());
    }
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
