// Trace of std::unique_ptr. Include <memory> and use only unique_ptr from it.
// Compiled against libstdc++ and against src/prism/pir/models/cxx (that
// directory first on the include path), both under ASan/UBSan, the lines
// should match. tests/cpp/test_cxx_memory.cpp checks the model text; it does
// not shell out to this file.
#include <cstdio>
#include <cstdlib>
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

int main() {
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
    return 0;
}
