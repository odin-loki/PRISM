// Differential trace of std::any (tests/cpp/test_cxx_any.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/any) first on the include path, both under
// ASan/UBSan. The no-argument trace must match. make_any<T>(args...) builds
// one T and the any owns it. An empty argument list value-initializes that
// T. type() on an empty any is typeid(void). type() on an any that holds
// T is typeid of that T. It does not throw. Comparison is not called.
// Pointer any_cast is called. A null any*, an empty any, and a wrong type
// are null.
// The matching type addresses the stored int, so a write through that
// pointer is the value the next any_cast reads.
//
// __prism_check is the model's precondition intrinsic. libstdc++ never calls
// it. The definition is a no-op on success so the happy-path stdout matches;
// a failed any_cast increments the counter and then throws bad_any_cast.
#include <any>
#include <cstdio>
#include <typeinfo>
#include <utility>

static int g_fail_checks = 0;

extern "C" __attribute__((noinline)) void __prism_check(int ok, const char*, const char*) {
    if (!ok) ++g_fail_checks;
}

// Larger than a pointer, so libstdc++ uses its heap manager. Move of the any
// then transfers the allocation and does not move this object.
struct Big {
    int v;
    char pad[24];
    explicit Big(int x) : v(x), pad{} { std::printf(" c%d", v); }
    Big(const Big& o) : v(o.v), pad{} { std::printf(" cc%d", v); }
    Big(Big&& o) noexcept : v(o.v), pad{} { std::printf(" m%d", v); }
    Big& operator=(const Big& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    ~Big() { std::printf(" d%d", v); }
};

// Larger than a pointer. Default construction is the value-initialized T.
struct Zero {
    int v;
    char pad[24];
    Zero() : v(0), pad{} { std::printf(" z"); }
    Zero(const Zero& o) : v(o.v), pad{} { std::printf(" zc%d", v); }
    Zero(Zero&& o) noexcept : v(o.v), pad{} { std::printf(" zm%d", v); }
    ~Zero() { std::printf(" zd%d", v); }
};

// Two arguments, no default constructor. Both arguments are the real T.
struct Two {
    int a;
    int b;
    char pad[24];
    Two(int x, int y) : a(x), b(y), pad{} { std::printf(" t%d,%d", a, b); }
    Two(const Two& o) : a(o.a), b(o.b), pad{} { std::printf(" tc%d,%d", a, b); }
    Two(Two&& o) noexcept : a(o.a), b(o.b), pad{} { std::printf(" tm%d,%d", a, b); }
    ~Two() { std::printf(" td%d,%d", a, b); }
};

static void nl(const char* what) { std::printf("%s\n", what); }

static int happy() {
    {
        std::any empty;
        std::printf("empty %d\n", empty.has_value() ? 1 : 0);
        std::printf("typeempty %d\n", empty.type() == typeid(void) ? 1 : 0);
        empty = Big(1);
        nl(" after assign");
        std::printf("typeheld %d %d\n", empty.type() == typeid(Big) ? 1 : 0, empty.type() == typeid(void) ? 1 : 0);
        std::printf("cast %d\n", std::any_cast<Big&>(empty).v);
        std::any copied(empty);
        nl(" after copy");
        std::printf("copy %d src %d\n", std::any_cast<const Big&>(copied).v, std::any_cast<Big&>(empty).v);
        std::printf("typecopy %d %d\n", copied.type() == typeid(Big) ? 1 : 0, empty.type() == typeid(Big) ? 1 : 0);
        std::any moved(std::move(copied));
        nl(" after move");
        std::printf("moved %d srcempty %d\n", std::any_cast<Big&>(moved).v, copied.has_value() ? 1 : 0);
        std::printf("typemove %d %d\n", moved.type() == typeid(Big) ? 1 : 0, copied.type() == typeid(void) ? 1 : 0);
        std::any other(Big(2));
        nl(" after other");
        other = std::move(moved);
        nl(" after move-assign");
        std::printf("other %d srcempty %d\n", std::any_cast<Big&>(other).v, moved.has_value() ? 1 : 0);
        std::printf("typemoveas %d %d\n", other.type() == typeid(Big) ? 1 : 0, moved.type() == typeid(void) ? 1 : 0);
        other.reset();
        nl(" after reset");
        std::printf("reset %d\n", other.has_value() ? 1 : 0);
        std::printf("typereset %d\n", other.type() == typeid(void) ? 1 : 0);
        std::any placed(std::in_place_type<Big>, 3);
        nl(" after inplace");
        placed.emplace<Big>(4);
        nl(" after emplace");
        std::printf("typeemplace %d\n", placed.type() == typeid(Big) ? 1 : 0);
        std::any peer(Big(5));
        placed.swap(peer);
        nl(" after swap");
        std::printf("swap %d %d\n", std::any_cast<Big&>(placed).v, std::any_cast<Big&>(peer).v);
        std::printf("typeswap %d %d\n", placed.type() == typeid(Big) ? 1 : 0, peer.type() == typeid(Big) ? 1 : 0);
        std::any n(42);
        std::printf("int %d\n", std::any_cast<int>(n));
        std::printf("typeint %d %d\n", n.type() == typeid(int) ? 1 : 0, n.type() == typeid(double) ? 1 : 0);
        std::any n2(n);
        n.reset();
        std::printf("intcopy %d empty %d\n", std::any_cast<int>(n2), n.has_value() ? 1 : 0);
        std::printf("typeintempty %d %d\n", n.type() == typeid(void) ? 1 : 0, n2.type() == typeid(int) ? 1 : 0);
        std::any self(Big(6));
        self = std::move(self);
        std::printf("self %d %d\n", self.has_value() ? 1 : 0, std::any_cast<Big&>(self).v);
        std::printf("typeself %d\n", self.type() == typeid(Big) ? 1 : 0);
        {
            std::any held(42);
            int* p = std::any_cast<int>(&held);
            const int* cp = std::any_cast<const int>(&held);
            const std::any* ch = &held;
            const int* cfrom = std::any_cast<int>(ch);
            std::any empty;
            std::any dbl(1.5);
            int* miss_empty = std::any_cast<int>(&empty);
            int* miss_type = std::any_cast<int>(&dbl);
            int* miss_null = std::any_cast<int>(static_cast<std::any*>(nullptr));
            const int* miss_cnull = std::any_cast<int>(static_cast<const std::any*>(nullptr));
            const double* miss_cd = std::any_cast<double>(ch);
            std::printf("ptr %d %d %d\n", p ? *p : -1, cp ? *cp : -1, cfrom ? *cfrom : -1);
            std::printf("miss %d %d %d %d %d\n", miss_empty ? 1 : 0, miss_type ? 1 : 0, miss_null ? 1 : 0,
                        miss_cnull ? 1 : 0, miss_cd ? 1 : 0);
            std::printf("typeptr %d %d %d\n", empty.type() == typeid(void) ? 1 : 0, dbl.type() == typeid(double) ? 1 : 0,
                        held.type() == typeid(int) ? 1 : 0);
            if (p) *p = 9;
            std::printf("mut %d\n", std::any_cast<int>(held));
        }
        std::any made_z = std::make_any<Zero>();
        nl(" after make0");
        std::printf("make0 %d %d\n", made_z.has_value() ? 1 : 0, std::any_cast<Zero&>(made_z).v);
        std::printf("typemake0 %d\n", made_z.type() == typeid(Zero) ? 1 : 0);
        std::any made_b = std::make_any<Big>(7);
        nl(" after make");
        std::printf("made %d\n", std::any_cast<Big&>(made_b).v);
        std::printf("typemade %d\n", made_b.type() == typeid(Big) ? 1 : 0);
        std::any made_i = std::make_any<int>();
        std::printf("madei %d %d\n", made_i.has_value() ? 1 : 0, std::any_cast<int>(made_i));
        std::printf("typemadei %d\n", made_i.type() == typeid(int) ? 1 : 0);
        std::any made_n = std::make_any<int>(8);
        std::printf("maden %d\n", std::any_cast<int>(made_n));
        std::printf("typemaden %d %d\n", made_n.type() == typeid(int) ? 1 : 0, made_n.type() == typeid(void) ? 1 : 0);
        std::any made_two = std::make_any<Two>(1, 2);
        nl(" after maketwo");
        std::printf("two %d %d\n", std::any_cast<Two&>(made_two).a, std::any_cast<Two&>(made_two).b);
        std::printf("typetwo %d\n", made_two.type() == typeid(Two) ? 1 : 0);
        nl(" leaving");
    }
    nl("done");
    return g_fail_checks == 0 ? 0 : 1;
}

// Empty any_cast is the check. A returned int would be an invented value.
static int empty_cast() {
    std::any a;
    if (a.has_value()) return 2;
    try {
        int invented = std::any_cast<int>(a);
        std::printf("invented %d\n", invented);
        return 3;
    } catch (const std::bad_any_cast&) {
        return g_fail_checks == 1 ? 0 : 4;
    }
}

// Wrong type: the double's bytes are not an int.
static int wrong_cast() {
    std::any a(1.5);
    try {
        int invented = std::any_cast<int>(a);
        std::printf("invented %d\n", invented);
        return 3;
    } catch (const std::bad_any_cast&) {
        return g_fail_checks == 1 ? 0 : 4;
    }
}

// type() does not throw. Empty is typeid(void). A held T is that T, including
// after make_any, emplace of a different type, swap, and reset.
static int type_query() {
    std::any empty;
    if (!(empty.type() == typeid(void))) return 2;
    std::any n(42);
    if (!(n.type() == typeid(int))) return 3;
    if (n.type() == typeid(double)) return 4;
    if (n.type() == typeid(void)) return 5;
    const std::any& cn = n;
    if (!(cn.type() == typeid(int))) return 6;
    n.reset();
    if (!(n.type() == typeid(void))) return 7;
    if (n.has_value()) return 8;
    std::any d(1.5);
    if (!(d.type() == typeid(double))) return 9;
    std::any moved(std::move(d));
    if (!(moved.type() == typeid(double))) return 10;
    if (!(d.type() == typeid(void))) return 11;
    std::any made = std::make_any<int>(8);
    if (!(made.type() == typeid(int))) return 12;
    made.emplace<double>(1.5);
    if (!(made.type() == typeid(double))) return 13;
    if (made.type() == typeid(int)) return 14;
    std::any a(7);
    std::any b(1.5);
    a.swap(b);
    if (!(a.type() == typeid(double))) return 15;
    if (!(b.type() == typeid(int))) return 16;
    if (g_fail_checks != 0) return 17;
    return 0;
}

// The int that was stored is the int any_cast returns.
static int stored_int() {
    std::any a(42);
    if (!a.has_value()) return 2;
    if (std::any_cast<int>(a) != 42) return 3;
    if (g_fail_checks != 0) return 4;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return happy();
    const char m = argv[1][0];
    if (m == 'e') return empty_cast();
    if (m == 'w') return wrong_cast();
    if (m == 'i') return stored_int();
    if (m == 't') return type_query();
    return 2;
}
