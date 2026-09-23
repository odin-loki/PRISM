// PIR tasks: virtual dispatch and function pointers (docs/PIR.md "Indirect
// calls"). A call through a vtable slot dispatches over the virtual
// functions in the module's vtables (class hierarchy analysis); any other
// pointer value is the havoc fallback (NEEDS-HARNESS when reachable).
#include <cassert>

struct Shape {
    virtual ~Shape() = default;
    virtual int sides() const = 0;
};
struct Tri : Shape {
    int sides() const override { return 3; }
};
struct Quad : Shape {
    int sides() const override { return 4; }
};

static int count(const Shape& s) { return s.sides(); }

int virt_ok(int x) {
    Tri t;
    Quad q;
    int n = x > 0 ? count(t) : count(q);
    assert(n == 3 || n == 4);
    return 12 / n;
}

int virt_bad(int x) {
    Tri t;
    Quad q;
    int n = x > 0 ? count(t) : count(q);
    return 12 / (n - 4);  // x <= 0: Quad has 4 sides, division by zero
}

static int twice(int v) { return 2 * v; }
static int negate(int v) { return -v; }

int fnptr_ok(int x) {
    int (*f)(int) = x > 0 ? twice : negate;
    if (x > 1000 || x < -1000) return 0;
    return f(x);
}

int fnptr_bad(int x) {
    int (*f)(int) = x > 0 ? twice : negate;
    return f(x);  // twice(x) overflows for x > INT_MAX / 2
}
