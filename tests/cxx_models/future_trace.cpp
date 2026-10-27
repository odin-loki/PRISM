// Differential trace of std::future (tests/cpp/test_cxx_future.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/future) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The happy-path traces must be identical.
//
// wait, wait_for, wait_until, async, and packaged_task are not called: the
// model does not declare them.
//
// argv "empty" calls get() on a default future. argv "twice" calls set_value
// a second time. argv "get2" calls get() a second time. The model must
// __prism_check and must not print a value.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print a returned T. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

// No default constructor and no copy: a model that invents a T cannot
// value-initialise E, and set_value must move it.
struct E {
    int v;
    explicit E(int x) : v(x) {}
    E(const E&) = delete;
    E(E&& o) noexcept : v(o.v) { o.v = -1; }
};

static void show() {
    std::future<int> empty;
    std::printf("empty %d\n", static_cast<int>(empty.valid()));

    std::promise<int> p;
    std::future<int> f = p.get_future();
    std::printf("live %d\n", static_cast<int>(f.valid()));
    p.set_value(42);
    int got = f.get();
    std::printf("got %d %d\n", got, static_cast<int>(f.valid()));

    std::promise<E> pe;
    std::future<E> fe = pe.get_future();
    pe.set_value(E(5));
    E e = fe.get();
    std::printf("held %d\n", e.v);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "empty") == 0) {
        std::future<int> f;
        std::printf("fake=%d\n", f.get());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "twice") == 0) {
        std::promise<int> p;
        std::future<int> f = p.get_future();
        p.set_value(1);
        p.set_value(2);
        std::printf("fake=%d\n", f.get());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "get2") == 0) {
        std::promise<int> p;
        std::future<int> f = p.get_future();
        p.set_value(7);
        (void)f.get();
        std::printf("fake=%d\n", f.get());
        return 0;
    }
    show();
    return 0;
}
