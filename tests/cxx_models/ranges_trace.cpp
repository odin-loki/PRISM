// Trace of std::ranges::size, std::ranges::ssize, std::ranges::empty,
// std::ranges::begin, std::ranges::end, std::ranges::data,
// std::ranges::cbegin, std::ranges::cend, std::ranges::cdata,
// std::ranges::distance, the find family, and std::ranges::count_if (from
// <algorithm>). `#include <ranges>` is the model when its directory
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
// find of T(&)[N] and a const T& value returns a pointer to the first equal
// element, or one past the last. find_if and find_if_not do the same for a
// function pointer bool (*)(const T&): true, or false. A null predicate
// traps before a call. N == 0 returns the decayed pointer and does not load
// and does not call. count_if of T(&)[N] and bool (*)(const T&) is how
// many elements make the predicate true, as a ptrdiff_t. It calls the
// predicate once per element. A null predicate traps before a call and
// before a load. N == 0 returns 0 and does not load and does not call.
// argv "countnull" passes a null predicate. A pointer argument is not
// called: it is not an overload, and it is not given a length. This is
// not a proof that an
// iterator stays in range or that a pointer has a length. This is not
// PROVED and not CLEAN.
#include <ranges>
// std::ranges::count_if is declared by <algorithm>, in libstdc++ and in the
// model directory. <ranges> alone does not declare it.
#include <algorithm>

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

constexpr bool fi_is_one(const int& x) { return x == 1; }
constexpr bool fi_is_two(const int& x) { return x == 2; }
constexpr bool fi_is_three(const int& x) { return x == 3; }
constexpr bool fi_is_four(const int& x) { return x == 4; }
constexpr bool fi_is_five(const int& x) { return x == 5; }
constexpr bool fi_is_seven(const int& x) { return x == 7; }
constexpr bool fi_is_nine(const int& x) { return x == 9; }
constexpr bool fi_always(const int&) { return true; }

int g_if_calls = 0;
int g_if_want = 0;
bool if_watch(const int& x) {
    ++g_if_calls;
    return x == g_if_want;
}

int g_nf_calls = 0;
int g_nf_want = 0;
bool nf_watch(const int& x) {
    ++g_nf_calls;
    return x == g_nf_want;
}
bool nf_always(const int&) {
    ++g_nf_calls;
    return true;
}

int g_cf_calls = 0;
int g_cf_want = 0;
bool cf_watch(const int& x) {
    ++g_cf_calls;
    return x == g_cf_want;
}
bool cf_always(const int&) {
    ++g_cf_calls;
    return true;
}

bool if_null_ptr(int* const& p) { return p == nullptr; }

int g_anchor = 0;
bool if_is_anchor(int* const& p) { return p == &g_anchor; }

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
static_assert(std::ranges::find(ak, 1) == ak);
static_assert(std::ranges::find(ak, 3) == ak + 2);
static_assert(std::ranges::find(ak, 4) == ak + 3);
static_assert(std::ranges::find(ak, 9) == ak + 4);
static_assert(std::ranges::find_if(ak, fi_is_one) == ak);
static_assert(std::ranges::find_if(ak, fi_is_three) == ak + 2);
static_assert(std::ranges::find_if(ak, fi_is_four) == ak + 3);
static_assert(std::ranges::find_if(ak, fi_is_nine) == ak + 4);
static_assert(std::ranges::find_if_not(ak, fi_is_one) == ak + 1);
static_assert(std::ranges::find_if_not(ak, fi_is_two) == ak);
static_assert(std::ranges::find_if_not(ak, fi_is_four) == ak);
static_assert(std::ranges::find_if_not(ak, fi_always) == ak + 4);
static_assert(std::ranges::count_if(ak, fi_is_one) == 1);
static_assert(std::ranges::count_if(ak, fi_is_four) == 1);
static_assert(std::ranges::count_if(ak, fi_always) == 4);
constexpr int fours[4] = {4, 1, 4, 2};
static_assert(std::ranges::count_if(fours, fi_is_four) == 2);
static_assert(std::ranges::count_if(fours, fi_is_one) == 1);
static_assert(std::ranges::count_if(fours, fi_is_nine) == 0);

constexpr Count ck{3};
static_assert(std::ranges::size(ck) == 3);
static_assert(std::ranges::ssize(ck) == 3);

static_assert(!std::ranges::empty(ak));
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

template <class T, class V>
constexpr bool prism_ranges_find = requires(T& r, const V& v) { std::ranges::find(r, v); };

template <class T, class P>
constexpr bool prism_ranges_find_if = requires(T& r, P p) { std::ranges::find_if(r, p); };

template <class T, class P>
constexpr bool prism_ranges_find_if_not = requires(T& r, P p) { std::ranges::find_if_not(r, p); };

template <class T, class P>
constexpr bool prism_ranges_count_if = requires(T& r, P p) { std::ranges::count_if(r, p); };

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
static_assert(prism_same<decltype(std::ranges::distance(prism_lref<int[4]>())), std::ptrdiff_t>);
static_assert(prism_same<decltype(std::ranges::distance(prism_lref<const int[4]>())), std::ptrdiff_t>);
static_assert(prism_same<decltype(std::ranges::find(prism_lref<int[4]>(), 0)), int*>);
static_assert(prism_same<decltype(std::ranges::find(prism_lref<const int[4]>(), 0)), const int*>);
static_assert(prism_same<decltype(std::ranges::find_if(prism_lref<int[4]>(), fi_is_one)), int*>);
static_assert(prism_same<decltype(std::ranges::find_if(prism_lref<const int[4]>(), fi_is_one)), const int*>);
static_assert(prism_same<decltype(std::ranges::find_if_not(prism_lref<int[4]>(), fi_is_one)), int*>);
static_assert(prism_same<decltype(std::ranges::find_if_not(prism_lref<const int[4]>(), fi_is_one)), const int*>);
static_assert(prism_same<decltype(std::ranges::count_if(prism_lref<int[4]>(), fi_is_one)), std::ptrdiff_t>);
static_assert(prism_same<decltype(std::ranges::count_if(prism_lref<const int[4]>(), fi_is_one)), std::ptrdiff_t>);

static_assert(prism_ranges_begin<int[4]>);
static_assert(prism_ranges_end<int[4]>);
static_assert(prism_ranges_data<int[4]>);
static_assert(prism_ranges_data<const int[4]>);
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
static_assert(!prism_ranges_distance<int*>);
static_assert(!prism_ranges_distance<const int*>);
static_assert(!prism_ranges_distance<int (*)[4]>);
static_assert(!prism_ranges_distance<Count>);
static_assert(prism_ranges_find<int[4], int>);
static_assert(prism_ranges_find<const int[4], int>);
static_assert(!prism_ranges_find<int*, int>);
static_assert(!prism_ranges_find<const int*, int>);
static_assert(!prism_ranges_find<int (*)[4], int>);
static_assert(!prism_ranges_find<Count, int>);
static_assert(prism_ranges_find_if<int[4], bool (*)(const int&)>);
static_assert(prism_ranges_find_if<const int[4], bool (*)(const int&)>);
static_assert(!prism_ranges_find_if<int*, bool (*)(const int&)>);
static_assert(!prism_ranges_find_if<const int*, bool (*)(const int&)>);
static_assert(!prism_ranges_find_if<int (*)[4], bool (*)(const int&)>);
static_assert(!prism_ranges_find_if<Count, bool (*)(const int&)>);
static_assert(prism_ranges_find_if_not<int[4], bool (*)(const int&)>);
static_assert(prism_ranges_find_if_not<const int[4], bool (*)(const int&)>);
static_assert(!prism_ranges_find_if_not<int*, bool (*)(const int&)>);
static_assert(!prism_ranges_find_if_not<const int*, bool (*)(const int&)>);
static_assert(!prism_ranges_find_if_not<int (*)[4], bool (*)(const int&)>);
static_assert(!prism_ranges_find_if_not<Count, bool (*)(const int&)>);
static_assert(prism_ranges_count_if<int[4], bool (*)(const int&)>);
static_assert(prism_ranges_count_if<const int[4], bool (*)(const int&)>);
static_assert(!prism_ranges_count_if<int*, bool (*)(const int&)>);
static_assert(!prism_ranges_count_if<const int*, bool (*)(const int&)>);
static_assert(!prism_ranges_count_if<int (*)[4], bool (*)(const int&)>);
static_assert(!prism_ranges_count_if<Count, bool (*)(const int&)>);

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "huge") == 0) {
        Wide w;
        volatile unsigned long long n = static_cast<unsigned long long>(PTRDIFF_MAX) + 1ull;
        w.n = n;
        std::ptrdiff_t s = std::ranges::ssize(w);
        std::printf("wrapped=%lld\n", static_cast<long long>(s));
        return s < 0 ? 2 : 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null") == 0) {
        int b[3] = {1, 2, 3};
        int* p = std::ranges::find_if(b, static_cast<bool (*)(const int&)>(nullptr));
        std::printf("called %d\n", p == b ? 1 : 0);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "notnull") == 0) {
        int b[3] = {1, 2, 3};
        int* p = std::ranges::find_if_not(b, static_cast<bool (*)(const int&)>(nullptr));
        std::printf("called %d\n", p == b ? 1 : 0);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "countnull") == 0) {
        int b[3] = {1, 2, 3};
        std::ptrdiff_t n = std::ranges::count_if(b, static_cast<bool (*)(const int&)>(nullptr));
        std::printf("called %lld\n", static_cast<long long>(n));
        return 0;
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
    const int ca[4] = {2, 0, 0, 5};
    std::printf("cearr %d\n", std::ranges::empty(ca) ? 1 : 0);
    int inner[4][0] = {};
    std::printf("inner %d\n", std::ranges::empty(inner) ? 1 : 0);
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
    std::printf("cbdist %d\n", std::ranges::end(ca) - std::ranges::begin(ca) == 4 ? 4 : -1);
    std::printf("cbfirst %d\n", *std::ranges::begin(ca) == 2 ? 2 : -1);
    std::printf("cblast %d\n", *(std::ranges::end(ca) - 1) == 5 ? 5 : -1);
    std::printf("bpdist %d\n", std::ranges::end(ps) - std::ranges::begin(ps) == 2 ? 2 : -1);
    std::printf("bpsame %d\n", std::ranges::begin(ps) == ps ? 1 : 0);
    std::printf("isame %d\n", std::ranges::begin(inner) == inner ? 1 : 0);
    std::printf("dfirst %d\n", *std::ranges::data(a) == 9 ? 9 : -1);
    std::printf("dsame %d\n",
                std::ranges::data(a) == a && std::ranges::data(a) == std::ranges::begin(a) ? 1 : 0);
    std::printf("cdfirst %d\n", *std::ranges::data(ca) == 2 ? 2 : -1);
    std::printf("cdsame %d\n", std::ranges::data(ca) == ca ? 1 : 0);
    std::printf("dpsame %d\n", std::ranges::data(ps) == ps ? 1 : 0);
    std::printf("idsame %d\n", std::ranges::data(inner) == inner ? 1 : 0);
    std::printf("kbdist %d\n", std::ranges::cend(a) - std::ranges::cbegin(a) == 4 ? 4 : -1);
    std::printf("kbfirst %d\n", *std::ranges::cbegin(a) == 9 ? 9 : -1);
    std::printf("kblast %d\n", *(std::ranges::cend(a) - 1) == 6 ? 6 : -1);
    std::printf("kbsame %d\n", std::ranges::cbegin(a) == std::ranges::begin(a) ? 1 : 0);
    std::printf("kcbdist %d\n", std::ranges::cend(ca) - std::ranges::cbegin(ca) == 4 ? 4 : -1);
    std::printf("kcbfirst %d\n", *std::ranges::cbegin(ca) == 2 ? 2 : -1);
    std::printf("kcblast %d\n", *(std::ranges::cend(ca) - 1) == 5 ? 5 : -1);
    std::printf("kbpdist %d\n", std::ranges::cend(ps) - std::ranges::cbegin(ps) == 2 ? 2 : -1);
    std::printf("kbpsame %d\n", std::ranges::cbegin(ps) == ps ? 1 : 0);
    std::printf("kisame %d\n", std::ranges::cbegin(inner) == inner ? 1 : 0);
    std::printf("kdfirst %d\n", *std::ranges::cdata(a) == 9 ? 9 : -1);
    std::printf("kdsame %d\n",
                std::ranges::cdata(a) == a && std::ranges::cdata(a) == std::ranges::cbegin(a) ? 1 : 0);
    std::printf("kcdfirst %d\n", *std::ranges::cdata(ca) == 2 ? 2 : -1);
    std::printf("kcdsame %d\n", std::ranges::cdata(ca) == ca ? 1 : 0);
    std::printf("kdpsame %d\n", std::ranges::cdata(ps) == ps ? 1 : 0);
    std::printf("kidsame %d\n", std::ranges::cdata(inner) == inner ? 1 : 0);
    std::printf("dist %d\n", std::ranges::distance(a) == 4 ? 4 : -1);
    std::printf("cdist %d\n", std::ranges::distance(ca) == 4 ? 4 : -1);
    std::printf("idist %d\n", std::ranges::distance(inner) == 4 ? 4 : -1);
    std::printf("pdist %d\n", std::ranges::distance(ps) == 2 ? 2 : -1);
    std::printf("ffirst %d\n", std::ranges::find(a, 9) == a ? 9 : -1);
    std::printf("fnext %d\n", std::ranges::find(a, 0) == a + 1 ? 1 : 0);
    std::printf("flast %d\n", std::ranges::find(a, 6) == a + 3 ? 6 : -1);
    std::printf("fmiss %d\n", std::ranges::find(a, 7) == std::ranges::end(a) ? 1 : 0);
    std::printf("fcfirst %d\n", std::ranges::find(ca, 2) == ca ? 2 : -1);
    std::printf("fclast %d\n", std::ranges::find(ca, 5) == ca + 3 ? 5 : -1);
    std::printf("fcmiss %d\n", std::ranges::find(ca, 7) == std::ranges::end(ca) ? 1 : 0);
    std::printf("fpfind %d\n", std::ranges::find(ps, nullptr) == ps ? 1 : 0);
    int anchor = 0;
    std::printf("fpmiss %d\n", std::ranges::find(ps, &anchor) == ps + 2 ? 1 : 0);
    int seq[4] = {9, 4, 1, 4};
    int kept[4] = {9, 4, 1, 4};
    g_if_calls = 0;
    g_if_want = 1;
    int* ifhit = std::ranges::find_if(seq, if_watch);
    std::printf("ifhit %d %d %d\n", (*ifhit == 1 && ifhit == seq + 2) ? 1 : 0, static_cast<int>(ifhit - seq),
                g_if_calls);
    g_if_calls = 0;
    g_if_want = 4;
    int* iffirst = std::ranges::find_if(seq, if_watch);
    std::printf("iffirst %d %d\n", static_cast<int>(iffirst - seq), g_if_calls);
    g_if_calls = 0;
    g_if_want = 9;
    int* iffront = std::ranges::find_if(seq, if_watch);
    std::printf("iffront %d %d\n", static_cast<int>(iffront - seq), g_if_calls);
    g_if_calls = 0;
    g_if_want = 7;
    int* ifmiss = std::ranges::find_if(seq, if_watch);
    std::printf("ifmiss %d %d\n", ifmiss == seq + 4 ? 1 : 0, g_if_calls);
    g_if_calls = 0;
    g_if_want = 9;
    int ties[2] = {2, 2};
    g_if_calls = 0;
    g_if_want = 2;
    int* iftie = std::ranges::find_if(ties, if_watch);
    std::printf("iftie %d %d\n", (iftie == ties && g_if_calls == 1) ? 1 : 0, g_if_calls);
    int one[1] = {5};
    g_if_calls = 0;
    g_if_want = 5;
    int* ifone = std::ranges::find_if(one, if_watch);
    int ifone_hit_calls = g_if_calls;
    g_if_calls = 0;
    g_if_want = 0;
    int* ifone_miss = std::ranges::find_if(one, if_watch);
    std::printf("ifone %d\n",
                (ifone == one && ifone_hit_calls == 1 && ifone_miss == one + 1 && g_if_calls == 1) ? 1 : 0);
    std::printf("ifkeep %d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    std::printf("ifcfirst %d\n", std::ranges::find_if(ca, fi_is_two) == ca ? 2 : -1);
    std::printf("ifclast %d\n", std::ranges::find_if(ca, fi_is_five) == ca + 3 ? 5 : -1);
    std::printf("ifcmiss %d\n", std::ranges::find_if(ca, fi_is_seven) == std::ranges::end(ca) ? 1 : 0);
    g_if_calls = 0;
    std::printf("ifpfind %d\n", std::ranges::find_if(ps, if_null_ptr) == ps ? 1 : 0);
    std::printf("ifpmiss %d\n", std::ranges::find_if(ps, if_is_anchor) == ps + 2 ? 1 : 0);
    g_nf_calls = 0;
    g_nf_want = 1;
    int* nfhit = std::ranges::find_if_not(seq, nf_watch);
    std::printf("nfhit %d %d %d\n", (*nfhit == 9 && nfhit == seq) ? 1 : 0, static_cast<int>(nfhit - seq), g_nf_calls);
    g_nf_calls = 0;
    g_nf_want = 9;
    int* nfnext = std::ranges::find_if_not(seq, nf_watch);
    std::printf("nfnext %d %d\n", static_cast<int>(nfnext - seq), g_nf_calls);
    int run[4] = {1, 1, 1, 2};
    g_nf_calls = 0;
    g_nf_want = 1;
    int* nflast = std::ranges::find_if_not(run, nf_watch);
    std::printf("nflast %d %d\n", static_cast<int>(nflast - run), g_nf_calls);
    g_nf_calls = 0;
    int* nfmiss = std::ranges::find_if_not(seq, nf_always);
    std::printf("nfmiss %d %d\n", nfmiss == seq + 4 ? 1 : 0, g_nf_calls);
    g_nf_calls = 0;
    g_nf_want = 9;
    g_nf_calls = 0;
    g_nf_want = 7;
    int* nftie = std::ranges::find_if_not(ties, nf_watch);
    std::printf("nftie %d %d\n", (nftie == ties && g_nf_calls == 1) ? 1 : 0, g_nf_calls);
    g_nf_calls = 0;
    g_nf_want = 2;
    int* nftie_all = std::ranges::find_if_not(ties, nf_watch);
    std::printf("nftieall %d %d\n", (nftie_all == ties + 2 && g_nf_calls == 2) ? 1 : 0, g_nf_calls);
    g_nf_calls = 0;
    g_nf_want = 0;
    int* nfone = std::ranges::find_if_not(one, nf_watch);
    int nfone_hit_calls = g_nf_calls;
    g_nf_calls = 0;
    g_nf_want = 5;
    int* nfone_miss = std::ranges::find_if_not(one, nf_watch);
    std::printf("nfone %d\n",
                (nfone == one && nfone_hit_calls == 1 && nfone_miss == one + 1 && g_nf_calls == 1) ? 1 : 0);
    std::printf("nfkeep %d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    std::printf("nfcfirst %d\n", std::ranges::find_if_not(ca, fi_is_five) == ca ? 2 : -1);
    std::printf("nfcnext %d\n", std::ranges::find_if_not(ca, fi_is_two) == ca + 1 ? 1 : 0);
    std::printf("nfcmiss %d\n", std::ranges::find_if_not(ca, fi_always) == std::ranges::end(ca) ? 1 : 0);
    g_nf_calls = 0;
    std::printf("nfpfind %d\n", std::ranges::find_if_not(ps, if_is_anchor) == ps ? 1 : 0);
    std::printf("nfpmiss %d\n", std::ranges::find_if_not(ps, if_null_ptr) == ps + 2 ? 1 : 0);
    g_cf_calls = 0;
    g_cf_want = 1;
    auto cfhit = std::ranges::count_if(seq, cf_watch);
    std::printf("cfhit %d %d\n", static_cast<int>(cfhit), g_cf_calls);
    g_cf_calls = 0;
    g_cf_want = 4;
    auto cftwo = std::ranges::count_if(seq, cf_watch);
    std::printf("cftwo %d %d\n", static_cast<int>(cftwo), g_cf_calls);
    g_cf_calls = 0;
    g_cf_want = 9;
    auto cffront = std::ranges::count_if(seq, cf_watch);
    std::printf("cffront %d %d\n", static_cast<int>(cffront), g_cf_calls);
    g_cf_calls = 0;
    g_cf_want = 7;
    auto cfmiss = std::ranges::count_if(seq, cf_watch);
    std::printf("cfmiss %d %d\n", static_cast<int>(cfmiss), g_cf_calls);
    g_cf_calls = 0;
    g_cf_want = 9;
    g_cf_calls = 0;
    g_cf_want = 2;
    auto cftie = std::ranges::count_if(ties, cf_watch);
    std::printf("cftie %d %d\n", static_cast<int>(cftie), g_cf_calls);
    g_cf_calls = 0;
    g_cf_want = 7;
    auto cftiemiss = std::ranges::count_if(ties, cf_watch);
    std::printf("cftiemiss %d %d\n", static_cast<int>(cftiemiss), g_cf_calls);
    g_cf_calls = 0;
    g_cf_want = 5;
    auto cfone = std::ranges::count_if(one, cf_watch);
    int cfone_hit_calls = g_cf_calls;
    g_cf_calls = 0;
    g_cf_want = 0;
    auto cfone_miss = std::ranges::count_if(one, cf_watch);
    std::printf("cfone %d\n",
                (cfone == 1 && cfone_hit_calls == 1 && cfone_miss == 0 && g_cf_calls == 1) ? 1 : 0);
    std::printf("cfkeep %d\n", std::memcmp(seq, kept, sizeof seq) == 0 ? 1 : 0);
    std::printf("cfcfirst %d\n", static_cast<int>(std::ranges::count_if(ca, fi_is_two)));
    g_cf_calls = 0;
    g_cf_want = 0;
    auto cfczero = std::ranges::count_if(ca, cf_watch);
    std::printf("cfczero %d %d\n", static_cast<int>(cfczero), g_cf_calls);
    g_cf_calls = 0;
    auto cfcall = std::ranges::count_if(ca, cf_always);
    std::printf("cfcall %d %d\n", static_cast<int>(cfcall), g_cf_calls);
    std::printf("cfcmiss %d\n", static_cast<int>(std::ranges::count_if(ca, fi_is_seven)));
    g_cf_calls = 0;
    std::printf("cfpnull %d\n", static_cast<int>(std::ranges::count_if(ps, if_null_ptr)));
    std::printf("cfpmiss %d\n", static_cast<int>(std::ranges::count_if(ps, if_is_anchor)));
    return 0;
}
