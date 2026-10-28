// Trace of std::begin / std::end and std::distance on pointers.
// `#include <iterator>` is the model when its directory is first on the
// include path, and libstdc++ otherwise. begin is a pointer to the first
// element. end is one past the last. distance of those two pointers is the
// element count. An empty array compares those pointers and does not
// dereference them. A pointer argument is not an overload of begin.
// argv "huge" asks distance of two char* whose count does not fit in
// ptrdiff_t. The model must __prism_check and must not print a wrapped
// value. This is not a proof that the two pointers address the same array,
// and it is not a proof that an iterator stays in range.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

template <class T>
constexpr bool prism_accepts_begin = requires(T& r) { std::begin(r); };

static_assert(prism_accepts_begin<int[3]>);
static_assert(!prism_accepts_begin<int*>);
static_assert(!prism_accepts_begin<const int*>);

template <class T>
constexpr bool prism_accepts_distance = requires(T a, T b) { std::distance(a, b); };

static_assert(prism_accepts_distance<int*>);
static_assert(prism_accepts_distance<const int*>);

struct NotAPointer {};
static_assert(!prism_accepts_distance<NotAPointer>);

constexpr int ak[3] = {4, 5, 6};
static_assert(std::distance(std::begin(ak), std::end(ak)) == 3);
static_assert(std::distance(std::end(ak), std::begin(ak)) == -3);

// The model declares this PIR intrinsic and does not define it. libstdc++
// <iterator> does not call it on a count that fits. A failing check aborts
// before a wrapped value can be printed.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "huge") == 0) {
        volatile std::uintptr_t lo = 0;
        volatile std::uintptr_t hi =
            static_cast<std::uintptr_t>(PTRDIFF_MAX) + static_cast<std::uintptr_t>(1);
        char* first = reinterpret_cast<char*>(static_cast<std::uintptr_t>(lo));
        char* last = reinterpret_cast<char*>(static_cast<std::uintptr_t>(hi));
        std::ptrdiff_t d = std::distance(first, last);
        std::printf("wrapped=%lld\n", static_cast<long long>(d));
        return d < 0 ? 2 : 0;
    }

    int a[3] = {4, 5, 6};
    int* b = std::begin(a);
    int* e = std::end(a);
    std::printf("len %td first %d last %d\n", std::distance(b, e), *b, *(e - 1));
    std::printf("neg %td\n", std::distance(e, b));

    int none[0] = {};
    int* nb = std::begin(none);
    int* ne = std::end(none);
    std::printf("empty %d\n", (nb == ne && std::distance(nb, ne) == 0) ? 1 : 0);

    const int c[2] = {8, 9};
    const int* cb = std::begin(c);
    const int* ce = std::end(c);
    std::printf("const %td %d\n", std::distance(cb, ce), *cb);

    int x = 1;
    int y = 2;
    int* ps[2] = {&x, &y};
    int** pb = std::begin(ps);
    int** pe = std::end(ps);
    std::printf("ptrs %td %d\n", std::distance(pb, pe), **pb);

    const bool ok = (std::distance(b, e) == 3) && (std::distance(e, b) == -3) && (*b == 4) && (*(e - 1) == 6) &&
                    (nb == ne) && (std::distance(nb, ne) == 0) && (std::distance(cb, ce) == 2) && (*cb == 8) &&
                    (std::distance(pb, pe) == 2) && (**pb == 1);
    return ok ? 0 : 1;
}
