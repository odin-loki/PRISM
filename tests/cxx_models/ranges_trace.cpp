// Trace of std::ranges::size, std::ranges::ssize, std::ranges::empty,
// std::ranges::begin, and std::ranges::end. `#include <ranges>` is the model
// when its directory is first on the include path, and libstdc++ otherwise.
// An array's size is N. An lvalue with a const size() member returns that
// size(). ssize of a value that fits is that value as ptrdiff_t. argv "huge"
// asks ssize of a size that does not fit. The model must __prism_check and
// must not print a wrapped negative value. empty of T(&)[N] is true only
// when N == 0, and it does not load an element. An lvalue with a const
// empty() member returns that empty(). begin of T(&)[N] is a pointer to the
// first element and does not load it. end is one past the last and does not
// load it. N == 0 returns the same pointer from both. A pointer argument is
// not called: it is not an overload. This is not a proof that an iterator
// stays in range.
#include <ranges>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <ranges> does not call it. A failing check aborts before a wrapped value
// can be printed.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

struct Count {
    int n;
    constexpr int size() const { return n; }
};

struct Wide {
    unsigned long long n;
    constexpr unsigned long long size() const { return n; }
};

struct Flag {
    bool e;
    constexpr bool empty() const { return e; }
};

// empty() is true and size() is not zero, so empty must call empty().
struct Prefer {
    constexpr bool empty() const { return true; }
    constexpr int size() const { return 4; }
};

constexpr int ak[4] = {1, 2, 3, 4};
static_assert(std::ranges::size(ak) == 4);
static_assert(std::ranges::ssize(ak) == 4);
static_assert(std::ranges::begin(ak) == ak);
static_assert(std::ranges::end(ak) == ak + 4);
static_assert(*std::ranges::begin(ak) == 1);
static_assert(*(std::ranges::end(ak) - 1) == 4);

constexpr Count ck{3};
static_assert(std::ranges::size(ck) == 3);
static_assert(std::ranges::ssize(ck) == 3);

static_assert(!std::ranges::empty(ak));
constexpr int zk[0] = {};
static_assert(std::ranges::empty(zk));
static_assert(std::ranges::begin(zk) == std::ranges::end(zk));
constexpr Flag fk{true};
constexpr Flag fn{false};
static_assert(std::ranges::empty(fk));
static_assert(!std::ranges::empty(fn));
constexpr Prefer pk{};
static_assert(std::ranges::empty(pk));
static_assert(std::ranges::size(pk) == 4);

template <class T>
constexpr bool prism_ranges_begin = requires(T& r) { std::ranges::begin(r); };

template <class T>
constexpr bool prism_ranges_end = requires(T& r) { std::ranges::end(r); };

static_assert(prism_ranges_begin<int[4]>);
static_assert(prism_ranges_end<int[4]>);
static_assert(prism_ranges_begin<int[0]>);
static_assert(prism_ranges_end<int[0]>);
static_assert(!prism_ranges_begin<int*>);
static_assert(!prism_ranges_end<int*>);
static_assert(!prism_ranges_begin<const int*>);
static_assert(!prism_ranges_end<const int*>);
static_assert(!prism_ranges_begin<int (*)[4]>);
static_assert(!prism_ranges_end<int (*)[4]>);
static_assert(!prism_ranges_begin<Count>);
static_assert(!prism_ranges_end<Count>);

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "huge") == 0) {
        Wide w;
        volatile unsigned long long n = static_cast<unsigned long long>(PTRDIFF_MAX) + 1ull;
        w.n = n;
        std::ptrdiff_t s = std::ranges::ssize(w);
        std::printf("wrapped=%lld\n", static_cast<long long>(s));
        return s < 0 ? 2 : 0;
    }
    int a[4] = {9, 0, 0, 6};
    Count c;
    c.n = 7;
    const Count k{3};
    std::printf("arr %d\n", std::ranges::size(a) == 4 ? 4 : -1);
    std::printf("sarr %d\n", std::ranges::ssize(a) == 4 ? 4 : -1);
    std::printf("box %d\n", std::ranges::size(c) == 7 ? 7 : -1);
    std::printf("sbox %d\n", std::ranges::ssize(c) == 7 ? 7 : -1);
    std::printf("cbox %d\n", std::ranges::size(k) == 3 ? 3 : -1);
    std::printf("scbox %d\n", std::ranges::ssize(k) == 3 ? 3 : -1);
    std::printf("earr %d\n", std::ranges::empty(a) ? 1 : 0);
    int z[0] = {};
    std::printf("zempty %d\n", std::ranges::empty(z) ? 1 : 0);
    const int ca[4] = {2, 0, 0, 5};
    std::printf("cearr %d\n", std::ranges::empty(ca) ? 1 : 0);
    int inner[4][0] = {};
    int outer[0][4] = {};
    std::printf("inner %d\n", std::ranges::empty(inner) ? 1 : 0);
    std::printf("outer %d\n", std::ranges::empty(outer) ? 1 : 0);
    Flag tf;
    tf.e = true;
    Flag ff;
    ff.e = false;
    const Flag cf{true};
    Prefer pr;
    std::printf("eflag %d\n", std::ranges::empty(tf) ? 1 : 0);
    std::printf("enflag %d\n", std::ranges::empty(ff) ? 1 : 0);
    std::printf("ecflag %d\n", std::ranges::empty(cf) ? 1 : 0);
    std::printf("eprefer %d\n", std::ranges::empty(pr) ? 1 : 0);
    int* ps[2] = {};
    std::printf("eptrs %d\n", std::ranges::empty(ps) ? 1 : 0);
    std::printf("bdist %d\n", std::ranges::end(a) - std::ranges::begin(a) == 4 ? 4 : -1);
    std::printf("bfirst %d\n", *std::ranges::begin(a) == 9 ? 9 : -1);
    std::printf("blast %d\n", *(std::ranges::end(a) - 1) == 6 ? 6 : -1);
    std::printf("zsame %d\n", std::ranges::begin(z) == std::ranges::end(z) ? 1 : 0);
    std::printf("cbdist %d\n", std::ranges::end(ca) - std::ranges::begin(ca) == 4 ? 4 : -1);
    std::printf("cbfirst %d\n", *std::ranges::begin(ca) == 2 ? 2 : -1);
    std::printf("cblast %d\n", *(std::ranges::end(ca) - 1) == 5 ? 5 : -1);
    std::printf("bpdist %d\n", std::ranges::end(ps) - std::ranges::begin(ps) == 2 ? 2 : -1);
    std::printf("bpsame %d\n", std::ranges::begin(ps) == ps ? 1 : 0);
    std::printf("isame %d\n", std::ranges::begin(inner) == inner ? 1 : 0);
    std::printf("osame %d\n", std::ranges::begin(outer) == std::ranges::end(outer) ? 1 : 0);
    return 0;
}
