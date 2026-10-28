// Differential trace of std::function (tests/cpp/test_cxx_functional.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/functional) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// The program uses std::function and std::invoke on a function pointer.
// bind, mem_fn, ref, cref, not_fn, bind_front, and reference_wrapper are not
// named: the model does not declare them. A callable that is not a function
// pointer is not passed to std::invoke.
//
// argv "empty" calls operator() on an empty std::function. The model must
// __prism_check and must not print a return value. argv "nullfn" calls
// std::invoke on a null function pointer. That path must __prism_check and
// must not print a return value.
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
    show();
    return 0;
}
