// Differential trace of std::any (tests/cpp/test_cxx_any.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/any) first on the include path, both under
// ASan/UBSan. The no-argument trace must match. make_any, type(), and
// comparison are not called: the model does not declare them. Pointer
// any_cast is called. A null any*, an empty any, and a wrong type are null.
// The matching type addresses the stored int, so a write through that
// pointer is the value the next any_cast reads.
//
// __prism_check is the model's precondition intrinsic. libstdc++ never calls
// it. The definition is a no-op on success so the happy-path stdout matches;
// a failed any_cast increments the counter and then throws bad_any_cast.
#include <any>
#include <cstdio>
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

static void nl(const char* what) { std::printf("%s\n", what); }

static int happy() {
    {
        std::any empty;
        std::printf("empty %d\n", empty.has_value() ? 1 : 0);
        empty = Big(1);
        nl(" after assign");
        std::printf("cast %d\n", std::any_cast<Big&>(empty).v);
        std::any copied(empty);
        nl(" after copy");
        std::printf("copy %d src %d\n", std::any_cast<const Big&>(copied).v, std::any_cast<Big&>(empty).v);
        std::any moved(std::move(copied));
        nl(" after move");
        std::printf("moved %d srcempty %d\n", std::any_cast<Big&>(moved).v, copied.has_value() ? 1 : 0);
        std::any other(Big(2));
        nl(" after other");
        other = std::move(moved);
        nl(" after move-assign");
        std::printf("other %d srcempty %d\n", std::any_cast<Big&>(other).v, moved.has_value() ? 1 : 0);
        other.reset();
        nl(" after reset");
        std::printf("reset %d\n", other.has_value() ? 1 : 0);
        std::any placed(std::in_place_type<Big>, 3);
        nl(" after inplace");
        placed.emplace<Big>(4);
        nl(" after emplace");
        std::any peer(Big(5));
        placed.swap(peer);
        nl(" after swap");
        std::printf("swap %d %d\n", std::any_cast<Big&>(placed).v, std::any_cast<Big&>(peer).v);
        std::any n(42);
        std::printf("int %d\n", std::any_cast<int>(n));
        std::any n2(n);
        n.reset();
        std::printf("intcopy %d empty %d\n", std::any_cast<int>(n2), n.has_value() ? 1 : 0);
        std::any self(Big(6));
        self = std::move(self);
        std::printf("self %d %d\n", self.has_value() ? 1 : 0, std::any_cast<Big&>(self).v);
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
            if (p) *p = 9;
            std::printf("mut %d\n", std::any_cast<int>(held));
        }
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
    return 2;
}
