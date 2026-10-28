// Trace of std::unique_ptr and std::make_unique. Include <memory> and use
// those two from it. Compiled against libstdc++ and against
// src/prism/pir/models/cxx (that directory first on the include path), both
// under ASan/UBSan, the lines should match. argv "huge" asks for an array
// count that does not fit in the byte size. The model must __prism_check
// and must not print a wrapped element. tests/cpp/test_cxx_memory.cpp checks
// the model text; it does not shell out to this file.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <memory> never calls it, so a successful run prints nothing from here.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "%s: %s\n", cls, msg);
        std::abort();
    }
}

struct E {
    int v;
    explicit E(int x) : v(x) { std::printf(" c%d", v); }
    ~E() { std::printf(" d%d", v); }
};

struct Z {
    int v;
    Z() : v(0) { std::printf(" z"); }
    ~Z() { std::printf(" Z"); }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "huge") == 0) {
        auto a = std::make_unique<int[]>(SIZE_MAX);
        std::printf("wrapped=%d\n", a[0]);
        return 0;
    }

    std::unique_ptr<int> empty;
    std::printf("empty %d\n", empty.get() == nullptr ? 1 : 0);

    {
        std::unique_ptr<int> p(new int(42));
        std::printf("own %d\n", *p);
    }

    {
        std::unique_ptr<E> e(new E(1));
        e.reset(new E(2));
        std::printf(" reset %d\n", e->v);
    }
    std::printf("\n");

    {
        std::unique_ptr<E> src(new E(3));
        std::unique_ptr<E> dst(std::move(src));
        std::printf(" move src_empty=%d dst=%d\n", src.get() == nullptr ? 1 : 0, dst->v);
    }
    std::printf("\n");

    {
        std::unique_ptr<E> src(new E(4));
        std::unique_ptr<E> dst(new E(5));
        dst = std::move(src);
        std::printf(" move-assign src_empty=%d dst=%d\n", src.get() == nullptr ? 1 : 0, dst->v);
    }
    std::printf("\n");

    {
        std::unique_ptr<E> p(new E(6));
        E* raw = p.release();
        std::printf(" release empty=%d\n", p.get() == nullptr ? 1 : 0);
        delete raw;
    }
    std::printf("\n");

    {
        int* raw = new int[3];
        raw[0] = 7;
        raw[1] = 8;
        raw[2] = 9;
        std::unique_ptr<int[]> a(raw);
        std::printf("arr %d %d %d\n", a[0], a[1], a[2]);
        std::unique_ptr<int[]> b(std::move(a));
        std::printf("arr-move src_empty=%d dst=%d\n", a.get() == nullptr ? 1 : 0, b[0]);
        b.reset();
        std::printf("arr-reset empty=%d\n", b.get() == nullptr ? 1 : 0);
    }

    {
        auto p = std::make_unique<int>(11);
        std::printf("mk %d\n", *p);
    }
    {
        auto z = std::make_unique<int>();
        std::printf("mk0 %d\n", *z);
    }
    {
        auto e = std::make_unique<E>(12);
        std::printf(" mkE %d\n", e->v);
    }
    std::printf("\n");
    {
        auto a = std::make_unique<int[]>(3);
        std::printf("mkarr %d %d %d\n", a[0], a[1], a[2]);
    }
    {
        auto none = std::make_unique<Z[]>(0);
        std::printf("mkzero empty=%d\n", none.get() == nullptr ? 1 : 0);
    }
    {
        auto two = std::make_unique<Z[]>(2);
        std::printf(" mktwo %d %d\n", two[0].v, two[1].v);
    }
    std::printf("\n");
    return 0;
}
