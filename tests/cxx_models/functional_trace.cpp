// Differential trace of std::function (tests/cpp/test_cxx_functional.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/functional) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// The program uses std::function, std::invoke, and std::not_fn on a function
// pointer. bind, mem_fn, ref, cref, bind_front, and reference_wrapper are not
// named: the model does not declare them. A callable that is not a function
// pointer is not passed to std::invoke or std::not_fn.
// std::identity returns the argument. std::equal_to, std::not_equal_to,
// std::less, and std::greater compare with == and <.
// std::plus, std::minus, std::multiplies, std::divides, std::modulus, and
// std::negate are function objects for one type. The happy path uses values
// that fit, so libstdc++ and the model print the same lines.
// std::logical_and, std::logical_or, and std::logical_not are function
// objects for one type. The happy path uses the operators, so libstdc++
// and the model print the same lines. A result that is not a bool is not
// on this trace.
//
// argv "empty" calls operator() on an empty std::function. The model must
// __prism_check and must not print a return value. argv "nullfn" calls
// std::invoke on a null function pointer. argv "nullnot" calls the wrapper
// from std::not_fn on a null function pointer. Those paths must __prism_check
// and must not print a return value.
// argv plus-ovf, plus-min, plus-uovf, plus-ll, plus-sc, minus-ovf, minus-pos,
// minus-uovf, mul-min, mul-max, mul-neg, mul-uovf, mul-sc, div-zero, div-min,
// div-sc, div-uzero, div-fzero, div-bzero, mod-zero, mod-min, mod-uzero,
// mod-bzero, neg-min, and neg-sc are the trap paths. Each prints fake= only
// if a result is returned.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned value. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

static int g_calls = 0;

static int target(int x) {
    ++g_calls;
    return x + 40;
}

static int other(int x) {
    ++g_calls;
    return x + 1;
}

// No default constructor: a model that invents a return value cannot
// value-initialise E.
struct E {
    int v;
    explicit E(int x) : v(x) {}
};

static E make_e() { return E(5); }

static int add2(int a, int b) {
    ++g_calls;
    return a + b;
}

static int bump(int& x) {
    ++g_calls;
    x += 10;
    return x;
}

static int take_rv(int&& x) {
    ++g_calls;
    return x + 7;
}

static void mark() { ++g_calls; }

static int plus_nx(int x) noexcept {
    ++g_calls;
    return x + 40;
}

static int g_slot = 9;

static int& slot() { return g_slot; }

static bool ready() {
    ++g_calls;
    return false;
}

static bool even(int x) {
    ++g_calls;
    return x % 2 == 0;
}

static int flag(int x) {
    ++g_calls;
    return x;
}

static bool bump_pos(int& x) {
    ++g_calls;
    x += 3;
    return x > 0;
}

static bool take_pos(int&& x) {
    ++g_calls;
    return x > 0;
}

static bool both_nz(int a, int b) {
    ++g_calls;
    return a != 0 && b != 0;
}

static bool pos_nx(int x) noexcept {
    ++g_calls;
    return x > 0;
}

static int* id_ptr(int* p) {
    ++g_calls;
    return p;
}

// explicit operator bool: usable as a bool, and not a default-constructed result.
struct Flag {
    int v;
    explicit operator bool() const { return v != 0; }
};

static Flag make_flag(int x) {
    ++g_calls;
    return Flag{x};
}

static bool g_bit = false;

static bool& bit() {
    ++g_calls;
    return g_bit;
}

// == and < only are enough for the model. != and > are the same results, so
// libstdc++'s operators agree.
struct Num {
    int v;
    friend constexpr bool operator==(const Num& a, const Num& b) { return a.v == b.v; }
    friend constexpr bool operator!=(const Num& a, const Num& b) { return !(a == b); }
    friend constexpr bool operator<(const Num& a, const Num& b) { return a.v < b.v; }
    friend constexpr bool operator>(const Num& a, const Num& b) { return b < a; }
};

struct Sum {
    int v;
    friend constexpr Sum operator+(Sum a, Sum b) { return Sum{a.v + b.v}; }
};

struct Quot {
    int v;
    friend constexpr Quot operator/(Quot a, Quot b) { return Quot{a.v / b.v}; }
    friend constexpr Quot operator%(Quot a, Quot b) { return Quot{a.v % b.v}; }
};

struct BoolOp {
    int v;
    friend constexpr bool operator&&(BoolOp a, BoolOp b) { return a.v && b.v; }
    friend constexpr bool operator||(BoolOp a, BoolOp b) { return a.v || b.v; }
    friend constexpr bool operator!(BoolOp a) { return !a.v; }
};

static void show() {
    {
        std::function<int(int)> empty;
        std::printf("empty %d\n", static_cast<int>(static_cast<bool>(empty)));
    }
    std::printf("dtor %d\n", g_calls);

    std::function<int(int)> f = target;
    std::printf("bool %d\n", static_cast<int>(static_cast<bool>(f)));
    std::printf("call %d calls=%d\n", f(2), g_calls);

    std::function<int(int)> moved(std::move(f));
    std::printf("move src=%d dst=%d calls=%d\n", static_cast<int>(static_cast<bool>(f)),
                static_cast<int>(static_cast<bool>(moved)), g_calls);
    std::printf("move call %d calls=%d\n", moved(2), g_calls);

    std::function<E()> held = make_e;
    std::printf("held %d\n", held().v);

    std::function<int(int)> replaced = target;
    replaced = other;
    std::printf("reassign %d calls=%d\n", replaced(3), g_calls);

    std::function<int(int)> copy = moved;
    std::printf("copy %d %d calls=%d\n", static_cast<int>(static_cast<bool>(copy)),
                static_cast<int>(static_cast<bool>(moved)), g_calls);
    std::printf("copy call %d calls=%d\n", copy(2), g_calls);

    {
        std::function<int(int)> live = target;
        (void)live;
    }
    std::printf("livedtor %d\n", g_calls);

    copy = nullptr;
    std::printf("null %d\n", static_cast<int>(static_cast<bool>(copy)));

    const int invoked = std::invoke(target, 2);
    std::printf("invoke %d calls=%d\n", invoked, g_calls);
    int (*p)(int) = target;
    const int invoked_ptr = std::invoke(p, 2);
    std::printf("invoke ptr %d calls=%d\n", invoked_ptr, g_calls);
    const int invoked_add = std::invoke(add2, 20, 22);
    std::printf("invoke add %d calls=%d\n", invoked_add, g_calls);
    int n = 1;
    const int invoked_ref = std::invoke(bump, n);
    std::printf("invoke ref %d n=%d calls=%d\n", invoked_ref, n, g_calls);
    const int invoked_rv = std::invoke(take_rv, 3);
    std::printf("invoke rv %d calls=%d\n", invoked_rv, g_calls);
    std::invoke(mark);
    std::printf("invoke void %d\n", g_calls);
    const int invoked_nx = std::invoke(plus_nx, 2);
    std::printf("invoke nx %d calls=%d\n", invoked_nx, g_calls);
    const int invoked_slot = std::invoke(slot);
    std::printf("invoke slot %d\n", invoked_slot);
    const E invoked_held = std::invoke(make_e);
    std::printf("invoke held %d\n", invoked_held.v);

    std::printf("not zero %d calls=%d\n", static_cast<int>(std::not_fn(ready)()), g_calls);

    const auto ne = std::not_fn(even);
    const auto ne_copy = ne;
    std::printf("not copy %d %d calls=%d\n", static_cast<int>(ne(2)), static_cast<int>(ne_copy(3)), g_calls);

    std::printf("not flag %d %d calls=%d\n", static_cast<int>(std::not_fn(flag)(0)),
                static_cast<int>(std::not_fn(flag)(5)), g_calls);

    int m = 1;
    const int nb = static_cast<int>(std::not_fn(bump_pos)(m));
    std::printf("not ref %d m=%d calls=%d\n", nb, m, g_calls);

    std::printf("not rv %d calls=%d\n", static_cast<int>(std::not_fn(take_pos)(8)), g_calls);
    std::printf("not both %d calls=%d\n", static_cast<int>(std::not_fn(both_nz)(1, 0)), g_calls);
    std::printf("not nx %d calls=%d\n", static_cast<int>(std::not_fn(pos_nx)(-3)), g_calls);

    int live = 7;
    int* zp = nullptr;
    std::printf("not ptr %d %d calls=%d\n", static_cast<int>(std::not_fn(id_ptr)(zp)),
                static_cast<int>(std::not_fn(id_ptr)(&live)), g_calls);

    std::printf("not obj %d %d calls=%d\n", static_cast<int>(std::not_fn(make_flag)(0)),
                static_cast<int>(std::not_fn(make_flag)(2)), g_calls);
    std::printf("not bit %d bit=%d calls=%d\n", static_cast<int>(std::not_fn(bit)()), static_cast<int>(g_bit),
                g_calls);

    bool (*np)(int) = nullptr;
    const auto held_null = std::not_fn(np);
    (void)held_null;
    std::printf("not hold\n");

    int id_x = 7;
    int& id_xr = std::identity{}(id_x);
    std::printf("id lv %d x=%d\n", id_xr, id_x);
    id_xr = 8;
    std::printf("id same %d\n", id_x);
    std::printf("id rv %d\n", std::identity{}(E(8)).v);

    struct MoveOnly {
        int v;
        int* moves;
        explicit MoveOnly(int vv, int* mm) : v(vv), moves(mm) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&& o) noexcept : v(o.v), moves(o.moves) {
            if (moves) ++*moves;
            o.v = -1;
        }
    };
    int moves = 0;
    MoveOnly mo(3, &moves);
    MoveOnly& mor = std::identity{}(mo);
    std::printf("id mo %d %d moves=%d\n", mor.v, mo.v, moves);

    static_assert(std::identity{}(5) == 5);
    static_assert(std::equal_to<int>{}(3, 3));
    static_assert(!std::equal_to<int>{}(3, 4));
    static_assert(!std::not_equal_to<int>{}(3, 3));
    static_assert(std::not_equal_to<int>{}(3, 4));
    static_assert(std::less<int>{}(1, 2));
    static_assert(!std::less<int>{}(2, 2));
    static_assert(std::greater<int>{}(2, 1));
    static_assert(!std::greater<int>{}(1, 1));
    static_assert(std::less<>()(1, 2.5));
    static_assert(std::greater<>()(0.7, 0.5));
    static_assert(std::not_equal_to<>()(0.5, 0.7));
    static_assert(std::equal_to<>()(0.5, 0.5));
    static_assert(!std::equal_to<>()(1, 2));

    std::equal_to<int> eq;
    std::equal_to<int> eq2 = eq;
    std::printf("eq %d %d\n", static_cast<int>(eq(1, 1)), static_cast<int>(eq2(1, 2)));
    std::not_equal_to<int> neq;
    std::printf("ne %d %d\n", static_cast<int>(neq(1, 1)), static_cast<int>(neq(1, 2)));
    std::less<int> lt;
    std::printf("lt %d %d\n", static_cast<int>(lt(1, 2)), static_cast<int>(lt(2, 2)));
    std::greater<int> gt;
    std::greater<int> gt2 = gt;
    std::printf("gt %d %d\n", static_cast<int>(gt(2, 1)), static_cast<int>(gt2(1, 1)));
    std::printf("tr %d %d %d %d\n", static_cast<int>(std::less<>()(1, 2.5)),
                static_cast<int>(std::greater<>()(0.7, 0.5)),
                static_cast<int>(std::not_equal_to<>()(0.5, 0.7)),
                static_cast<int>(std::equal_to<>()(1, 2)));
    int arr[2] = {1, 2};
    std::printf("ptr %d\n", static_cast<int>(std::less<int*>()(&arr[0], &arr[1])));

    const Num n1{1};
    const Num n2{2};
    std::printf("obj %d %d %d %d\n", static_cast<int>(std::equal_to<Num>{}(n1, n1)),
                static_cast<int>(std::not_equal_to<Num>{}(n1, n1)), static_cast<int>(std::less<Num>{}(n1, n2)),
                static_cast<int>(std::greater<Num>{}(n2, n1)));

    static_assert(std::plus<int>{}(2, 3) == 5);
    static_assert(std::plus<int>{}(-2, -3) == -5);
    static_assert(std::plus<int>{}(INT_MAX, 0) == INT_MAX);
    static_assert(std::plus<int>{}(INT_MIN, 1) == INT_MIN + 1);
    static_assert(std::minus<int>{}(INT_MIN, 0) == INT_MIN);
    static_assert(std::minus<int>{}(INT_MAX, 1) == INT_MAX - 1);
    static_assert(std::multiplies<int>{}(0, INT_MIN) == 0);
    static_assert(std::multiplies<int>{}(-1, INT_MAX) == -INT_MAX);
    static_assert(std::multiplies<int>{}(1, INT_MIN) == INT_MIN);
    static_assert(std::divides<int>{}(7, 2) == 3);
    static_assert(std::divides<int>{}(-7, 2) == -3);
    static_assert(std::divides<int>{}(INT_MIN, 1) == INT_MIN);
    static_assert(std::divides<int>{}(INT_MIN, -2) == INT_MIN / -2);
    static_assert(std::modulus<int>{}(7, 2) == 1);
    static_assert(std::modulus<int>{}(-7, 2) == -1);
    static_assert(std::modulus<int>{}(INT_MIN, 2) == 0);
    static_assert(std::negate<int>{}(INT_MAX) == -INT_MAX);
    static_assert(std::negate<int>{}(0) == 0);
    static_assert(std::plus<unsigned>{}(2u, 3u) == 5u);
    static_assert(std::negate<unsigned>{}(1u) == 0u - 1u);
    static_assert(std::plus<signed char>{}(static_cast<signed char>(100), static_cast<signed char>(27)) ==
                  static_cast<signed char>(127));
    static_assert(std::multiplies<signed char>{}(static_cast<signed char>(-2), static_cast<signed char>(-63)) ==
                  static_cast<signed char>(126));
    static_assert(std::multiplies<signed char>{}(static_cast<signed char>(2), static_cast<signed char>(-64)) ==
                  static_cast<signed char>(-128));
    static_assert(std::divides<bool>{}(true, true) == true);
    static_assert(std::plus<Sum>{}(Sum{2}, Sum{3}).v == 5);
    static_assert(std::logical_and<int>{}(1, 1));
    static_assert(!std::logical_and<int>{}(1, 0));
    static_assert(std::logical_or<int>{}(0, 1));
    static_assert(!std::logical_or<int>{}(0, 0));
    static_assert(std::logical_not<int>{}(0));
    static_assert(!std::logical_not<int>{}(1));
    static_assert(std::logical_and<bool>{}(true, true));
    static_assert(!std::logical_or<bool>{}(false, false));
    static_assert(!std::logical_not<bool>{}(true));
    static_assert(std::logical_and<BoolOp>{}(BoolOp{1}, BoolOp{1}));
    static_assert(!std::logical_or<BoolOp>{}(BoolOp{0}, BoolOp{0}));
    static_assert(std::logical_not<BoolOp>{}(BoolOp{0}));

    std::plus<int> padd;
    std::plus<int> padd2 = padd;
    std::printf("add %d %d\n", padd(2, 3), padd2(-2, -3));
    std::printf("sub %d\n", std::minus<int>{}(3, 2));
    std::printf("mul %d\n", std::multiplies<int>{}(-3, 4));
    std::printf("div %d %d\n", std::divides<int>{}(7, 2), std::modulus<int>{}(7, 2));
    std::printf("divn %d %d\n", std::divides<int>{}(-7, 2), std::modulus<int>{}(-7, 2));
    std::negate<int> ng;
    std::negate<int> ng2 = ng;
    std::printf("neg %d %d %d\n", ng(4), ng(-4), ng2(0));
    std::printf("uadd %u\n", std::plus<unsigned>{}(2u, 3u));
    std::printf("usub %u\n", std::minus<unsigned>{}(5u, 2u));
    std::printf("umul %u\n", std::multiplies<unsigned>{}(2u, 3u));
    std::printf("udiv %u %u\n", std::divides<unsigned>{}(7u, 2u), std::modulus<unsigned>{}(7u, 2u));
    std::printf("uneg %u %u\n", std::negate<unsigned>{}(0u), std::negate<unsigned>{}(1u));
    std::printf("ll %lld\n", std::plus<long long>{}(20, 22));
    std::printf("sc %d\n", static_cast<int>(std::plus<signed char>{}(static_cast<signed char>(2),
                                                                      static_cast<signed char>(3))));
    std::printf("fp %.1f\n", std::plus<double>{}(1.5, 2.5));
    std::printf("bdiv %d\n", static_cast<int>(std::divides<bool>{}(true, true)));
    std::printf("objadd %d\n", std::plus<Sum>{}(Sum{2}, Sum{3}).v);
    const Quot q1{7};
    const Quot q2{2};
    std::printf("quot %d %d\n", std::divides<Quot>{}(q1, q2).v, std::modulus<Quot>{}(q1, q2).v);
    std::printf("bit %d\n", std::bit_and<int>{}(6, 3));
    std::printf("land %d\n", static_cast<int>(std::logical_and<int>{}(1, 0)));
    std::logical_and<int> landf;
    std::logical_and<int> landf2 = landf;
    std::printf("landc %d %d\n", static_cast<int>(landf(1, 0)), static_cast<int>(landf2(1, 1)));
    std::logical_or<int> lorf;
    std::logical_or<int> lorf2 = lorf;
    std::printf("lor %d %d\n", static_cast<int>(lorf(0, 1)), static_cast<int>(lorf2(0, 0)));
    std::logical_not<int> lnotf;
    std::logical_not<int> lnotf2 = lnotf;
    std::printf("lnot %d %d\n", static_cast<int>(lnotf(0)), static_cast<int>(lnotf2(1)));
    std::printf("lbool %d %d %d\n", static_cast<int>(std::logical_and<bool>{}(true, false)),
                static_cast<int>(std::logical_or<bool>{}(false, false)),
                static_cast<int>(std::logical_not<bool>{}(true)));
    const BoolOp b1{1};
    const BoolOp b0{0};
    std::printf("blog %d %d %d\n", static_cast<int>(std::logical_and<BoolOp>{}(b1, b0)),
                static_cast<int>(std::logical_or<BoolOp>{}(b0, b1)),
                static_cast<int>(std::logical_not<BoolOp>{}(b0)));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "empty") == 0) {
        std::function<int(int)> f;
        std::printf("fake=%d\n", f(0));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullfn") == 0) {
        E (*p)() = nullptr;
        std::printf("fake=%d\n", std::invoke(p).v);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "nullnot") == 0) {
        bool (*p)(int) = nullptr;
        std::printf("fake=%d\n", static_cast<int>(std::not_fn(p)(0)));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "plus-ovf") == 0) {
        std::printf("fake=%d\n", std::plus<int>{}(INT_MAX, 1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "plus-min") == 0) {
        std::printf("fake=%d\n", std::plus<int>{}(INT_MIN, -1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "plus-uovf") == 0) {
        std::printf("fake=%u\n", std::plus<unsigned>{}(UINT_MAX, 1u));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "plus-ll") == 0) {
        std::printf("fake=%lld\n", std::plus<long long>{}(LLONG_MAX, 1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "plus-sc") == 0) {
        std::printf("fake=%d\n", static_cast<int>(std::plus<signed char>{}(static_cast<signed char>(127),
                                                                            static_cast<signed char>(1))));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "minus-ovf") == 0) {
        std::printf("fake=%d\n", std::minus<int>{}(INT_MIN, 1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "minus-pos") == 0) {
        std::printf("fake=%d\n", std::minus<int>{}(INT_MAX, -1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "minus-uovf") == 0) {
        std::printf("fake=%u\n", std::minus<unsigned>{}(0u, 1u));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul-min") == 0) {
        std::printf("fake=%d\n", std::multiplies<int>{}(INT_MIN, -1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul-max") == 0) {
        std::printf("fake=%d\n", std::multiplies<int>{}(INT_MAX, 2));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul-neg") == 0) {
        std::printf("fake=%d\n", std::multiplies<int>{}(INT_MIN, 2));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul-uovf") == 0) {
        std::printf("fake=%u\n", std::multiplies<unsigned>{}(UINT_MAX, 2u));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mul-sc") == 0) {
        std::printf("fake=%d\n", static_cast<int>(std::multiplies<signed char>{}(static_cast<signed char>(-2),
                                                                                  static_cast<signed char>(-64))));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div-zero") == 0) {
        std::printf("fake=%d\n", std::divides<int>{}(1, 0));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div-min") == 0) {
        std::printf("fake=%d\n", std::divides<int>{}(INT_MIN, -1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div-sc") == 0) {
        std::printf("fake=%d\n", static_cast<int>(std::divides<signed char>{}(static_cast<signed char>(-128),
                                                                               static_cast<signed char>(-1))));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div-uzero") == 0) {
        std::printf("fake=%u\n", std::divides<unsigned>{}(1u, 0u));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div-fzero") == 0) {
        std::printf("fake=%f\n", std::divides<double>{}(1.0, 0.0));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "div-bzero") == 0) {
        std::printf("fake=%d\n", static_cast<int>(std::divides<bool>{}(true, false)));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod-zero") == 0) {
        std::printf("fake=%d\n", std::modulus<int>{}(1, 0));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod-min") == 0) {
        std::printf("fake=%d\n", std::modulus<int>{}(INT_MIN, -1));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod-uzero") == 0) {
        std::printf("fake=%u\n", std::modulus<unsigned>{}(1u, 0u));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "mod-bzero") == 0) {
        std::printf("fake=%d\n", static_cast<int>(std::modulus<bool>{}(true, false)));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neg-min") == 0) {
        std::printf("fake=%d\n", std::negate<int>{}(INT_MIN));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neg-sc") == 0) {
        std::printf("fake=%d\n", static_cast<int>(std::negate<signed char>{}(static_cast<signed char>(-128))));
        return 0;
    }
    show();
    return 0;
}
