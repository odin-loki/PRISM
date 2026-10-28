// Trace of std::begin / std::end, std::distance, std::next, and std::prev
// on pointers. `#include <iterator>` is the model when its directory is
// first on the include path, and libstdc++ otherwise. begin is a pointer
// to the first element. end is one past the last. distance of those two
// pointers is the element count. An empty array compares those pointers
// and does not dereference them. A pointer argument is not an overload of
// begin. argv "huge" asks distance of two char* whose count does not fit
// in ptrdiff_t. The model must __prism_check and must not print a wrapped
// value. argv "neg" asks next(p, n) with n < 0. argv "pneg" asks prev(p, n)
// with n < 0. The model must __prism_check and must not print a moved
// pointer. next(p) is p + 1. next(p, 0) is p. next(p, n) for n > 0 is
// p + n. prev(p) is p - 1. prev(p, 0) is p. prev(p, n) for n > 0 is p - n.
// advance is not declared. This is not a proof that the pointer addresses
// an array, and it is not a proof that an iterator stays in range.
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

template <class T>
constexpr bool prism_accepts_next = requires(T p) { std::next(p); };

static_assert(prism_accepts_next<int*>);
static_assert(prism_accepts_next<const int*>);
static_assert(!prism_accepts_next<NotAPointer>);

template <class T>
constexpr bool prism_accepts_next_n = requires(T p, std::ptrdiff_t n) { std::next(p, n); };

static_assert(prism_accepts_next_n<int*>);
static_assert(prism_accepts_next_n<const int*>);
static_assert(!prism_accepts_next_n<NotAPointer>);

template <class T, class U>
constexpr bool prism_accepts_next_nu = requires(T p, U n) { std::next(p, n); };

static_assert(prism_accepts_next_nu<int*, std::ptrdiff_t>);
static_assert(prism_accepts_next_nu<const int*, int>);
static_assert(!prism_accepts_next_nu<int*, NotAPointer>);
static_assert(!prism_accepts_next_nu<NotAPointer, std::ptrdiff_t>);

template <class T>
constexpr bool prism_accepts_prev = requires(T p) { std::prev(p); };

static_assert(prism_accepts_prev<int*>);
static_assert(prism_accepts_prev<const int*>);
static_assert(!prism_accepts_prev<NotAPointer>);

template <class T>
constexpr bool prism_accepts_prev_n = requires(T p, std::ptrdiff_t n) { std::prev(p, n); };

static_assert(prism_accepts_prev_n<int*>);
static_assert(prism_accepts_prev_n<const int*>);
static_assert(!prism_accepts_prev_n<NotAPointer>);

template <class T, class U>
constexpr bool prism_accepts_prev_nu = requires(T p, U n) { std::prev(p, n); };

static_assert(prism_accepts_prev_nu<int*, std::ptrdiff_t>);
static_assert(prism_accepts_prev_nu<const int*, int>);
static_assert(!prism_accepts_prev_nu<int*, NotAPointer>);
static_assert(!prism_accepts_prev_nu<NotAPointer, std::ptrdiff_t>);

constexpr int ak[3] = {4, 5, 6};
static_assert(std::distance(std::begin(ak), std::end(ak)) == 3);
static_assert(std::distance(std::end(ak), std::begin(ak)) == -3);
static_assert(std::next(std::begin(ak)) == ak + 1);
static_assert(std::next(std::begin(ak), 0) == std::begin(ak));
static_assert(std::next(std::begin(ak), 1) == ak + 1);
static_assert(std::next(std::begin(ak), 2) == ak + 2);
static_assert(std::next(std::begin(ak), 3) == std::end(ak));
static_assert(std::next(std::end(ak), 0) == std::end(ak));
static_assert(std::prev(std::end(ak)) == ak + 2);
static_assert(std::prev(std::end(ak), 0) == std::end(ak));
static_assert(std::prev(std::end(ak), 1) == ak + 2);
static_assert(std::prev(std::end(ak), 2) == ak + 1);
static_assert(std::prev(std::end(ak), 3) == std::begin(ak));
static_assert(std::prev(std::begin(ak), 0) == std::begin(ak));

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

    if (argc > 1 && std::strcmp(argv[1], "neg") == 0) {
        int a[3] = {4, 5, 6};
        int* p = a;
        volatile std::ptrdiff_t n = -1;
        int* q = std::next(p, n);
        std::printf("moved=%p\n", static_cast<void*>(q));
        return 0;
    }

    if (argc > 1 && std::strcmp(argv[1], "pneg") == 0) {
        int a[3] = {4, 5, 6};
        int* p = a;
        volatile std::ptrdiff_t n = -1;
        int* q = std::prev(p, n);
        std::printf("moved=%p\n", static_cast<void*>(q));
        return 0;
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
    std::printf("next %d %d %d %d\n", *std::next(b), std::next(b, 0) == b ? 1 : 0, *std::next(b, 2),
                std::next(b, 3) == e ? 1 : 0);
    std::printf("next0 %d\n", std::next(nb, 0) == nb ? 1 : 0);
    std::printf("nextc %d\n", *std::next(cb));
    volatile std::ptrdiff_t step0 = 0;
    volatile std::ptrdiff_t step2 = 2;
    std::printf("nextrt %d %d\n", std::next(b, step0) == b ? 1 : 0, *std::next(b, step2));
    std::printf("prev %d %d %d %d\n", *std::prev(e), std::prev(e, 0) == e ? 1 : 0, *std::prev(e, 2),
                std::prev(e, 3) == b ? 1 : 0);
    std::printf("prev0 %d\n", std::prev(ne, 0) == ne ? 1 : 0);
    std::printf("prevc %d\n", *std::prev(ce));
    volatile std::ptrdiff_t back0 = 0;
    volatile std::ptrdiff_t back2 = 2;
    std::printf("prevrt %d %d\n", std::prev(e, back0) == e ? 1 : 0, *std::prev(e, back2));

    const bool next_ok = (std::next(b) == b + 1) && (std::next(b, 1) == b + 1) && (std::next(b, 0) == b) &&
                         (std::next(b, 2) == b + 2) && (std::next(b, 3) == e) && (std::next(nb, 0) == nb) &&
                         (std::next(cb) == cb + 1) && (std::next(cb, 0) == cb) && (*std::next(b) == 5) &&
                         (*std::next(b, 2) == 6) && (*std::next(cb) == 9) && (std::next(b, step0) == b) &&
                         (std::next(b, step2) == b + 2);
    const bool prev_ok = (std::prev(e) == e - 1) && (std::prev(e, 1) == e - 1) && (std::prev(e, 0) == e) &&
                         (std::prev(e, 2) == e - 2) && (std::prev(e, 3) == b) && (std::prev(ne, 0) == ne) &&
                         (std::prev(ce) == ce - 1) && (std::prev(ce, 0) == ce) && (*std::prev(e) == 6) &&
                         (*std::prev(e, 2) == 5) && (*std::prev(ce) == 9) && (std::prev(e, back0) == e) &&
                         (std::prev(e, back2) == e - 2);
    const bool ok = (std::distance(b, e) == 3) && (std::distance(e, b) == -3) && (*b == 4) && (*(e - 1) == 6) &&
                    (nb == ne) && (std::distance(nb, ne) == 0) && (std::distance(cb, ce) == 2) && (*cb == 8) &&
                    (std::distance(pb, pe) == 2) && (**pb == 1) && next_ok && prev_ok;
    return ok ? 0 : 1;
}
