// Trace of std::ranges::size, std::ranges::ssize, std::ranges::empty,
// std::ranges::begin, std::ranges::end, std::ranges::data,
// std::ranges::cbegin, std::ranges::cend, std::ranges::cdata, and
// std::ranges::distance. `#include <ranges>` is the model when its directory
// is first on the include path, and libstdc++ otherwise. An array's size is
// N. An lvalue with a const size() member returns that size(). ssize of a
// value that fits is that value as ptrdiff_t. argv "huge" asks ssize of a
// size that does not fit. The model must __prism_check and must not print a
// wrapped negative value. empty of T(&)[N] is true only when N == 0, and it
// does not load an element. An lvalue with a const empty() member returns
// that empty(). begin of T(&)[N] is a pointer to the first element and does
// not load it. end is one past the last and does not load it. N == 0 returns
// the same pointer from both. data of T(&)[N] is that same first-element
// pointer by array-to-pointer decay and does not load it. N == 0 returns the
// decayed pointer and does not load it. cbegin, cend, and cdata of T(&)[N]
// with N >= 1 are const T* by that same decay. cbegin is the first element
// and does not load it. cend is one past the last and does not load it.
// cdata is the decayed pointer and does not load it. N == 0 does not match
// that parameter. A second overload returns the decayed pointer and does not
// load it. distance of T(&)[N] with N >= 1 is the bound as ptrdiff_t and
// does not load an element. N == 0 returns 0 and does not load an element.
// A pointer argument is not called: it is not an overload, and it is not
// given a length. This is not a proof that an iterator stays in range or
// that a pointer has a length. This is not PROVED and not CLEAN.
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
static_assert(std::ranges::data(ak) == ak);
static_assert(std::ranges::data(ak) == std::ranges::begin(ak));
static_assert(*std::ranges::data(ak) == 1);
static_assert(std::ranges::cbegin(ak) == ak);
static_assert(std::ranges::cend(ak) == ak + 4);
static_assert(*std::ranges::cbegin(ak) == 1);
static_assert(*(std::ranges::cend(ak) - 1) == 4);
static_assert(std::ranges::cbegin(ak) == std::ranges::begin(ak));
static_assert(std::ranges::cdata(ak) == ak);
static_assert(std::ranges::cdata(ak) == std::ranges::cbegin(ak));
static_assert(*std::ranges::cdata(ak) == 1);
static_assert(std::ranges::distance(ak) == 4);
static_assert(std::ranges::distance(ak) == std::ranges::ssize(ak));

constexpr Count ck{3};
static_assert(std::ranges::size(ck) == 3);
static_assert(std::ranges::ssize(ck) == 3);

static_assert(!std::ranges::empty(ak));
constexpr int zk[0] = {};
static_assert(std::ranges::empty(zk));
static_assert(std::ranges::begin(zk) == std::ranges::end(zk));
static_assert(std::ranges::data(zk) == zk);
static_assert(std::ranges::data(zk) == std::ranges::begin(zk));
static_assert(std::ranges::cbegin(zk) == std::ranges::cend(zk));
static_assert(std::ranges::cdata(zk) == zk);
static_assert(std::ranges::cdata(zk) == std::ranges::cbegin(zk));
static_assert(std::ranges::distance(zk) == 0);
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

template <class T>
constexpr bool prism_ranges_data = requires(T& r) { std::ranges::data(r); };

template <class T>
constexpr bool prism_ranges_cbegin = requires(T& r) { std::ranges::cbegin(r); };

template <class T>
constexpr bool prism_ranges_cend = requires(T& r) { std::ranges::cend(r); };

template <class T>
constexpr bool prism_ranges_cdata = requires(T& r) { std::ranges::cdata(r); };

template <class T>
constexpr bool prism_ranges_distance = requires(T& r) { std::ranges::distance(r); };

template <class A, class B>
constexpr bool prism_same = false;
template <class A>
constexpr bool prism_same<A, A> = true;

template <class T>
constexpr T& prism_lref() noexcept;

static_assert(prism_same<decltype(std::ranges::cbegin(prism_lref<int[4]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cend(prism_lref<int[4]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cdata(prism_lref<int[4]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cbegin(prism_lref<const int[4]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cend(prism_lref<const int[4]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cdata(prism_lref<const int[4]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cbegin(prism_lref<int[0]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cend(prism_lref<int[0]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::cdata(prism_lref<int[0]>())), const int*>);
static_assert(prism_same<decltype(std::ranges::distance(prism_lref<int[4]>())), std::ptrdiff_t>);
static_assert(prism_same<decltype(std::ranges::distance(prism_lref<const int[4]>())), std::ptrdiff_t>);
static_assert(prism_same<decltype(std::ranges::distance(prism_lref<int[0]>())), std::ptrdiff_t>);

static_assert(prism_ranges_begin<int[4]>);
static_assert(prism_ranges_end<int[4]>);
static_assert(prism_ranges_data<int[4]>);
static_assert(prism_ranges_data<const int[4]>);
static_assert(prism_ranges_begin<int[0]>);
static_assert(prism_ranges_end<int[0]>);
static_assert(prism_ranges_data<int[0]>);
static_assert(!prism_ranges_begin<int*>);
static_assert(!prism_ranges_end<int*>);
static_assert(!prism_ranges_data<int*>);
static_assert(!prism_ranges_begin<const int*>);
static_assert(!prism_ranges_end<const int*>);
static_assert(!prism_ranges_data<const int*>);
static_assert(!prism_ranges_begin<int (*)[4]>);
static_assert(!prism_ranges_end<int (*)[4]>);
static_assert(!prism_ranges_data<int (*)[4]>);
static_assert(!prism_ranges_begin<Count>);
static_assert(!prism_ranges_end<Count>);
static_assert(!prism_ranges_data<Count>);
static_assert(prism_ranges_cbegin<int[4]>);
static_assert(prism_ranges_cend<int[4]>);
static_assert(prism_ranges_cdata<int[4]>);
static_assert(prism_ranges_cdata<const int[4]>);
static_assert(prism_ranges_cbegin<int[0]>);
static_assert(prism_ranges_cend<int[0]>);
static_assert(prism_ranges_cdata<int[0]>);
static_assert(!prism_ranges_cbegin<int*>);
static_assert(!prism_ranges_cend<int*>);
static_assert(!prism_ranges_cdata<int*>);
static_assert(!prism_ranges_cbegin<const int*>);
static_assert(!prism_ranges_cend<const int*>);
static_assert(!prism_ranges_cdata<const int*>);
static_assert(!prism_ranges_cbegin<int (*)[4]>);
static_assert(!prism_ranges_cend<int (*)[4]>);
static_assert(!prism_ranges_cdata<int (*)[4]>);
static_assert(!prism_ranges_cbegin<Count>);
static_assert(!prism_ranges_cend<Count>);
static_assert(!prism_ranges_cdata<Count>);
static_assert(prism_ranges_distance<int[4]>);
static_assert(prism_ranges_distance<const int[4]>);
static_assert(prism_ranges_distance<int[0]>);
static_assert(!prism_ranges_distance<int*>);
static_assert(!prism_ranges_distance<const int*>);
static_assert(!prism_ranges_distance<int (*)[4]>);
static_assert(!prism_ranges_distance<Count>);

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
    std::printf("dfirst %d\n", *std::ranges::data(a) == 9 ? 9 : -1);
    std::printf("dsame %d\n",
                std::ranges::data(a) == a && std::ranges::data(a) == std::ranges::begin(a) ? 1 : 0);
    std::printf("cdfirst %d\n", *std::ranges::data(ca) == 2 ? 2 : -1);
    std::printf("cdsame %d\n", std::ranges::data(ca) == ca ? 1 : 0);
    std::printf("zdata %d\n",
                std::ranges::data(z) == z && std::ranges::data(z) == std::ranges::begin(z) ? 1 : 0);
    std::printf("dpsame %d\n", std::ranges::data(ps) == ps ? 1 : 0);
    std::printf("idsame %d\n", std::ranges::data(inner) == inner ? 1 : 0);
    std::printf("odsame %d\n",
                std::ranges::data(outer) == outer && std::ranges::data(outer) == std::ranges::begin(outer) ? 1 : 0);
    std::printf("kbdist %d\n", std::ranges::cend(a) - std::ranges::cbegin(a) == 4 ? 4 : -1);
    std::printf("kbfirst %d\n", *std::ranges::cbegin(a) == 9 ? 9 : -1);
    std::printf("kblast %d\n", *(std::ranges::cend(a) - 1) == 6 ? 6 : -1);
    std::printf("kbsame %d\n", std::ranges::cbegin(a) == std::ranges::begin(a) ? 1 : 0);
    std::printf("kzsame %d\n", std::ranges::cbegin(z) == std::ranges::cend(z) ? 1 : 0);
    std::printf("kcbdist %d\n", std::ranges::cend(ca) - std::ranges::cbegin(ca) == 4 ? 4 : -1);
    std::printf("kcbfirst %d\n", *std::ranges::cbegin(ca) == 2 ? 2 : -1);
    std::printf("kcblast %d\n", *(std::ranges::cend(ca) - 1) == 5 ? 5 : -1);
    std::printf("kbpdist %d\n", std::ranges::cend(ps) - std::ranges::cbegin(ps) == 2 ? 2 : -1);
    std::printf("kbpsame %d\n", std::ranges::cbegin(ps) == ps ? 1 : 0);
    std::printf("kisame %d\n", std::ranges::cbegin(inner) == inner ? 1 : 0);
    std::printf("kosame %d\n", std::ranges::cbegin(outer) == std::ranges::cend(outer) ? 1 : 0);
    std::printf("kdfirst %d\n", *std::ranges::cdata(a) == 9 ? 9 : -1);
    std::printf("kdsame %d\n",
                std::ranges::cdata(a) == a && std::ranges::cdata(a) == std::ranges::cbegin(a) ? 1 : 0);
    std::printf("kcdfirst %d\n", *std::ranges::cdata(ca) == 2 ? 2 : -1);
    std::printf("kcdsame %d\n", std::ranges::cdata(ca) == ca ? 1 : 0);
    std::printf("kzdata %d\n",
                std::ranges::cdata(z) == z && std::ranges::cdata(z) == std::ranges::cbegin(z) ? 1 : 0);
    std::printf("kdpsame %d\n", std::ranges::cdata(ps) == ps ? 1 : 0);
    std::printf("kidsame %d\n", std::ranges::cdata(inner) == inner ? 1 : 0);
    std::printf("kodsame %d\n",
                std::ranges::cdata(outer) == outer && std::ranges::cdata(outer) == std::ranges::cbegin(outer) ? 1 : 0);
    std::printf("dist %d\n", std::ranges::distance(a) == 4 ? 4 : -1);
    std::printf("zdist %d\n", std::ranges::distance(z) == 0 ? 0 : -1);
    std::printf("cdist %d\n", std::ranges::distance(ca) == 4 ? 4 : -1);
    std::printf("idist %d\n", std::ranges::distance(inner) == 4 ? 4 : -1);
    std::printf("odist %d\n", std::ranges::distance(outer) == 0 ? 0 : -1);
    std::printf("pdist %d\n", std::ranges::distance(ps) == 2 ? 2 : -1);
    return 0;
}
