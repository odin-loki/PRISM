// Differential trace of std::array. Compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/array) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical. No heap: `oob` is operator[] at size(), which aborts in
// the _GLIBCXX_ASSERTIONS check. operator== is the same N: N == 0 is true
// and does not read an element, and N > 0 stops at the first mismatch.
// operator!= is the negation of that operator==. N == 0 compares not-equal
// as false and does not read an element. A mismatch stops at the first
// difference inside operator== and does not read further elements. Equal
// arrays compare not-equal as false. operator< is the same N: N == 0 is
// false and does not read an element.
// N > 0 returns true at the first element that is less, false when an
// earlier element of the left array is greater, and false when the two
// arrays are equivalent through N. operator> is the same N: N == 0 is
// false and does not read an element. N > 0 is lexicographical
// greater-than: false at the first element that is less, true at the first
// element that is greater, and false when the two arrays are equivalent
// through N. operator>= is the same N: N == 0 is true and does not read
// an element. Two empty arrays are equivalent. N > 0 is lexicographical
// greater-or-equal: false at the first element that is less, true at the
// first element that is greater, and true when the two arrays are
// equivalent through N. operator<= is the same N: N == 0 is true and does
// not read an element. Two empty arrays are equivalent. N > 0 is
// lexicographical less-or-equal: true at the first element that is less,
// false at the first element that is greater, and true when the two arrays
// are equivalent through N. Iterators, operator<=>, std::get, and
// std::to_array are not called.
#include <array>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>

static_assert(std::array<int, 0>{} == std::array<int, 0>{});
static_assert(std::array<int, 3>{1, 2, 3} == std::array<int, 3>{1, 2, 3});
static_assert(!(std::array<int, 3>{1, 2, 3} == std::array<int, 3>{1, 0, 3}));
static_assert(!(std::array<int, 3>{1, 2, 3} == std::array<int, 3>{0, 2, 3}));
static_assert(!(std::array<int, 3>{1, 2, 3} == std::array<int, 3>{1, 2, 4}));
static_assert(std::array<int, 1>{8} == std::array<int, 1>{8});
static_assert(!(std::array<int, 1>{8} == std::array<int, 1>{9}));
static_assert(!(std::array<int, 0>{} != std::array<int, 0>{}));
static_assert(!(std::array<int, 3>{1, 2, 3} != std::array<int, 3>{1, 2, 3}));
static_assert(std::array<int, 3>{1, 2, 3} != std::array<int, 3>{1, 0, 3});
static_assert(std::array<int, 3>{1, 2, 3} != std::array<int, 3>{0, 2, 3});
static_assert(std::array<int, 3>{1, 2, 3} != std::array<int, 3>{1, 2, 4});
static_assert(!(std::array<int, 1>{8} != std::array<int, 1>{8}));
static_assert(std::array<int, 1>{8} != std::array<int, 1>{9});
static_assert(!(std::array<int, 0>{} < std::array<int, 0>{}));
static_assert(!(std::array<int, 3>{1, 2, 3} < std::array<int, 3>{1, 2, 3}));
static_assert(std::array<int, 3>{1, 0, 3} < std::array<int, 3>{1, 2, 3});
static_assert(std::array<int, 3>{0, 2, 3} < std::array<int, 3>{1, 2, 3});
static_assert(std::array<int, 3>{1, 2, 3} < std::array<int, 3>{1, 2, 4});
static_assert(!(std::array<int, 3>{1, 2, 4} < std::array<int, 3>{1, 2, 3}));
static_assert(!(std::array<int, 3>{2, 0, 0} < std::array<int, 3>{1, 9, 9}));
static_assert(std::array<int, 1>{1} < std::array<int, 1>{2});
static_assert(!(std::array<int, 1>{2} < std::array<int, 1>{1}));
static_assert(!(std::array<int, 1>{2} < std::array<int, 1>{2}));
static_assert(!(std::array<int, 0>{} > std::array<int, 0>{}));
static_assert(!(std::array<int, 3>{1, 2, 3} > std::array<int, 3>{1, 2, 3}));
static_assert(std::array<int, 3>{1, 2, 3} > std::array<int, 3>{1, 0, 3});
static_assert(std::array<int, 3>{1, 2, 3} > std::array<int, 3>{0, 2, 3});
static_assert(std::array<int, 3>{1, 2, 4} > std::array<int, 3>{1, 2, 3});
static_assert(!(std::array<int, 3>{1, 2, 3} > std::array<int, 3>{1, 2, 4}));
static_assert(std::array<int, 3>{2, 0, 0} > std::array<int, 3>{1, 9, 9});
static_assert(!(std::array<int, 3>{1, 9, 9} > std::array<int, 3>{2, 0, 0}));
static_assert(std::array<int, 1>{2} > std::array<int, 1>{1});
static_assert(!(std::array<int, 1>{1} > std::array<int, 1>{2}));
static_assert(!(std::array<int, 1>{2} > std::array<int, 1>{2}));
static_assert(std::array<int, 0>{} >= std::array<int, 0>{});
static_assert(std::array<int, 3>{1, 2, 3} >= std::array<int, 3>{1, 2, 3});
static_assert(std::array<int, 3>{1, 2, 3} >= std::array<int, 3>{1, 0, 3});
static_assert(std::array<int, 3>{1, 2, 3} >= std::array<int, 3>{0, 2, 3});
static_assert(std::array<int, 3>{1, 2, 4} >= std::array<int, 3>{1, 2, 3});
static_assert(!(std::array<int, 3>{1, 2, 3} >= std::array<int, 3>{1, 2, 4}));
static_assert(std::array<int, 3>{2, 0, 0} >= std::array<int, 3>{1, 9, 9});
static_assert(!(std::array<int, 3>{1, 9, 9} >= std::array<int, 3>{2, 0, 0}));
static_assert(std::array<int, 1>{2} >= std::array<int, 1>{1});
static_assert(!(std::array<int, 1>{1} >= std::array<int, 1>{2}));
static_assert(std::array<int, 1>{2} >= std::array<int, 1>{2});
static_assert(std::array<int, 0>{} <= std::array<int, 0>{});
static_assert(std::array<int, 3>{1, 2, 3} <= std::array<int, 3>{1, 2, 3});
static_assert(std::array<int, 3>{1, 0, 3} <= std::array<int, 3>{1, 2, 3});
static_assert(std::array<int, 3>{0, 2, 3} <= std::array<int, 3>{1, 2, 3});
static_assert(std::array<int, 3>{1, 2, 3} <= std::array<int, 3>{1, 2, 4});
static_assert(!(std::array<int, 3>{1, 2, 4} <= std::array<int, 3>{1, 2, 3}));
static_assert(std::array<int, 3>{1, 9, 9} <= std::array<int, 3>{2, 0, 0});
static_assert(!(std::array<int, 3>{2, 0, 0} <= std::array<int, 3>{1, 9, 9}));
static_assert(std::array<int, 1>{1} <= std::array<int, 1>{2});
static_assert(!(std::array<int, 1>{2} <= std::array<int, 1>{1}));
static_assert(std::array<int, 1>{2} <= std::array<int, 1>{2});

struct E {
    int v;
    E(int x = 0) : v(x) { std::printf(" c%d", v); }
    E(const E& o) : v(o.v) { std::printf(" cc%d", v); }
    E& operator=(const E& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
};

static void ints() {
    std::array<int, 4> a{1, 2, 3, 4};
    std::printf("max int4=%zu zero=%zu\n", a.max_size(), std::array<int, 0>{}.max_size());
    std::printf("init size=%zu empty=%d front=%d back=%d [", a.size(), int(a.empty()), a.front(), a.back());
    for (std::size_t i = 0; i < a.size(); ++i) std::printf(" %d", a[i]);
    std::printf(" ] data0=%d\n", a.data()[0]);
    const int* p = a.data();
    a.fill(7);
    std::printf("fill stable=%d [", int(a.data() == p));
    for (std::size_t i = 0; i < a.size(); ++i) std::printf(" %d", a[i]);
    std::printf(" ]\n");
    std::array<int, 4> b{8, 9, 10, 11};
    a.swap(b);
    std::printf("swap a");
    for (std::size_t i = 0; i < a.size(); ++i) std::printf(" %d", a[i]);
    std::printf(" b");
    for (std::size_t i = 0; i < b.size(); ++i) std::printf(" %d", b[i]);
    std::printf(" stable=%d b0=%d\n", int(a.data() == p), b[0]);
    std::array<int, 4> c = a;
    std::printf("copy %d %d\n", c.front(), c.back());
    bool threw = false;
    try {
        (void)a.at(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    std::printf("at threw=%d at2=%d\n", int(threw), a.at(2));
    std::array<int, 0> z{};
    z.fill(1);
    std::array<int, 0> w{};
    if (z.size() == 0) z.swap(w);
    std::printf("zero size=%zu empty=%d null=%d wempty=%d\n", z.size(), int(z.empty()), int(z.data() == nullptr),
                int(w.empty()));
    std::array d{3, 1, 4};
    std::printf("deduced %zu %d %d\n", d.size(), d.front(), d.back());
}

static void traced() {
    {
        std::array<E, 3> a{E(1), E(2), E(3)};
        std::printf("\n");
        a.fill(E(9));
        std::printf("\n");
        std::array<E, 3> b{E(4), E(5), E(6)};
        std::printf("\n");
        a.swap(b);
        std::printf("\n");
    }
    std::printf("\n");
}

static int oob() {
    std::array<int, 4> a{1, 2, 3, 4};
    return a[4];
}

// No default constructor: an invented element would not compile. operator==
// counts reads. A negative value is past a mismatch and must not be read.
struct MemEq {
    int v;
    static int calls;
    static int poison;
    explicit MemEq(int x) : v(x) {}
    bool operator==(const MemEq& o) const {
        ++calls;
        if (v < 0 || o.v < 0) ++poison;
        return v == o.v;
    }
};

int MemEq::calls = 0;
int MemEq::poison = 0;

static void equal() {
    std::array<int, 3> a{1, 2, 3};
    std::array<int, 3> same{1, 2, 3};
    std::array<int, 3> mid{1, 0, 3};
    std::array<int, 3> head{0, 2, 3};
    std::array<int, 3> tail{1, 2, 4};
    std::array<int, 1> one{5};
    std::array<int, 1> one_same{5};
    std::array<int, 1> one_diff{6};
    const std::array<int, 2> ca{4, 5};
    const std::array<int, 2> cb{4, 5};
    const std::array<int, 2> cc{4, 6};
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    std::printf("eq same=%d mid=%d head=%d tail=%d one=%d oned=%d self=%d csame=%d cdiff=%d zero=%d\n", int(a == same),
                int(a == mid), int(a == head), int(a == tail), int(one == one_same), int(one == one_diff), int(a == a),
                int(ca == cb), int(ca == cc), int(z == w));

    std::array<MemEq, 3> ma{MemEq(1), MemEq(2), MemEq(3)};
    std::array<MemEq, 3> mb{MemEq(1), MemEq(2), MemEq(3)};
    MemEq::calls = 0;
    const bool ms = (ma == mb);
    const int calls_same = MemEq::calls;

    std::array<MemEq, 3> mm{MemEq(1), MemEq(9), MemEq(3)};
    MemEq::calls = 0;
    const bool mmd = (ma == mm);
    const int calls_mid = MemEq::calls;

    std::array<MemEq, 3> mh{MemEq(9), MemEq(2), MemEq(3)};
    MemEq::calls = 0;
    const bool mhd = (ma == mh);
    const int calls_head = MemEq::calls;

    std::array<MemEq, 3> mt{MemEq(1), MemEq(2), MemEq(9)};
    MemEq::calls = 0;
    const bool mtd = (ma == mt);
    const int calls_tail = MemEq::calls;

    MemEq::calls = 0;
    MemEq::poison = 0;
    std::array<MemEq, 0> mz{};
    std::array<MemEq, 0> mw{};
    const bool mzero = (mz == mw);
    const int calls_zero = MemEq::calls;

    std::array<MemEq, 3> pa{MemEq(1), MemEq(2), MemEq(-1)};
    std::array<MemEq, 3> pb{MemEq(1), MemEq(9), MemEq(-1)};
    MemEq::calls = 0;
    MemEq::poison = 0;
    const bool poison_eq = (pa == pb);
    std::printf(
        "eqm same=%d mid=%d head=%d tail=%d zero=%d poison=%d calls same=%d mid=%d head=%d tail=%d zero=%d poison=%d "
        "poison_reads=%d\n",
        int(ms), int(mmd), int(mhd), int(mtd), int(mzero), int(poison_eq), calls_same, calls_mid, calls_head,
        calls_tail, calls_zero, MemEq::calls, MemEq::poison);
}

// operator!= is !(__x == __y). MemEq::operator== counts the reads inside
// operator==. A negative value past a mismatch must not be read. N == 0
// compares not-equal as false and does not call operator== on an element.
static void not_equal() {
    std::array<int, 3> a{1, 2, 3};
    std::array<int, 3> same{1, 2, 3};
    std::array<int, 3> mid{1, 0, 3};
    std::array<int, 3> head{0, 2, 3};
    std::array<int, 3> tail{1, 2, 4};
    std::array<int, 1> one{5};
    std::array<int, 1> one_same{5};
    std::array<int, 1> one_diff{6};
    const std::array<int, 2> ca{4, 5};
    const std::array<int, 2> cb{4, 5};
    const std::array<int, 2> cc{4, 6};
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    std::printf("ne same=%d mid=%d head=%d tail=%d one=%d oned=%d self=%d csame=%d cdiff=%d zero=%d\n", int(a != same),
                int(a != mid), int(a != head), int(a != tail), int(one != one_same), int(one != one_diff), int(a != a),
                int(ca != cb), int(ca != cc), int(z != w));

    std::array<MemEq, 3> ma{MemEq(1), MemEq(2), MemEq(3)};
    std::array<MemEq, 3> mb{MemEq(1), MemEq(2), MemEq(3)};
    MemEq::calls = 0;
    const bool ms = (ma != mb);
    const int calls_same = MemEq::calls;

    std::array<MemEq, 3> mm{MemEq(1), MemEq(9), MemEq(3)};
    MemEq::calls = 0;
    const bool mmd = (ma != mm);
    const int calls_mid = MemEq::calls;

    std::array<MemEq, 3> mh{MemEq(9), MemEq(2), MemEq(3)};
    MemEq::calls = 0;
    const bool mhd = (ma != mh);
    const int calls_head = MemEq::calls;

    std::array<MemEq, 3> mt{MemEq(1), MemEq(2), MemEq(9)};
    MemEq::calls = 0;
    const bool mtd = (ma != mt);
    const int calls_tail = MemEq::calls;

    MemEq::calls = 0;
    MemEq::poison = 0;
    std::array<MemEq, 0> mz{};
    std::array<MemEq, 0> mw{};
    const bool mzero = (mz != mw);
    const int calls_zero = MemEq::calls;

    std::array<MemEq, 3> pa{MemEq(1), MemEq(2), MemEq(-1)};
    std::array<MemEq, 3> pb{MemEq(1), MemEq(9), MemEq(-1)};
    MemEq::calls = 0;
    MemEq::poison = 0;
    const bool poison_ne = (pa != pb);
    std::printf(
        "nem same=%d mid=%d head=%d tail=%d zero=%d poison=%d calls same=%d mid=%d head=%d tail=%d zero=%d poison=%d "
        "poison_reads=%d\n",
        int(ms), int(mmd), int(mhd), int(mtd), int(mzero), int(poison_ne), calls_same, calls_mid, calls_head,
        calls_tail, calls_zero, MemEq::calls, MemEq::poison);
}

// No default constructor: an invented element would not compile. operator<
// counts reads. A negative value past the deciding element must not be read.
// NeverLt is never less, so two arrays of it are equivalent through N.
struct MemLt {
    int v;
    static int calls;
    static int poison;
    explicit MemLt(int x) : v(x) {}
    bool operator<(const MemLt& o) const {
        ++calls;
        if (v < 0 || o.v < 0) ++poison;
        return v < o.v;
    }
};

int MemLt::calls = 0;
int MemLt::poison = 0;

struct NeverLt {
    int v;
    static int calls;
    explicit NeverLt(int x) : v(x) {}
    bool operator<(const NeverLt&) const {
        ++calls;
        return false;
    }
};

int NeverLt::calls = 0;

static void less() {
    std::array<int, 3> a{1, 2, 3};
    std::array<int, 3> same{1, 2, 3};
    std::array<int, 3> mid{1, 0, 3};
    std::array<int, 3> head{0, 2, 3};
    std::array<int, 3> tail{1, 2, 4};
    std::array<int, 3> early{2, 0, 0};
    std::array<int, 3> early_r{1, 9, 9};
    std::array<int, 1> one{5};
    std::array<int, 1> one_hi{6};
    std::array<int, 1> one_same{5};
    const std::array<int, 2> ca{4, 5};
    const std::array<int, 2> cb{4, 5};
    const std::array<int, 2> cc{4, 6};
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    std::printf(
        "lt same=%d mid=%d head=%d tail=%d gt=%d early=%d one=%d oned=%d oneeq=%d self=%d csame=%d cdiff=%d zero=%d\n",
        int(a < same), int(mid < a), int(head < a), int(a < tail), int(tail < a), int(early < early_r), int(one < one_hi),
        int(one_hi < one), int(one < one_same), int(a < a), int(ca < cb), int(ca < cc), int(z < w));

    std::array<MemLt, 3> ma{MemLt(1), MemLt(2), MemLt(3)};
    std::array<MemLt, 3> mb{MemLt(1), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool ms = (ma < mb);
    const int calls_same = MemLt::calls;

    std::array<MemLt, 3> mm{MemLt(1), MemLt(0), MemLt(3)};
    MemLt::calls = 0;
    const bool mmd = (mm < ma);
    const int calls_mid = MemLt::calls;

    std::array<MemLt, 3> mh{MemLt(0), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool mhd = (mh < ma);
    const int calls_head = MemLt::calls;

    std::array<MemLt, 3> mt{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mtd = (ma < mt);
    const int calls_tail = MemLt::calls;

    std::array<MemLt, 3> mg{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mgd = (mg < ma);
    const int calls_gt = MemLt::calls;

    std::array<MemLt, 3> me{MemLt(2), MemLt(0), MemLt(0)};
    std::array<MemLt, 3> mer{MemLt(1), MemLt(9), MemLt(9)};
    MemLt::calls = 0;
    const bool med = (me < mer);
    const int calls_early = MemLt::calls;

    MemLt::calls = 0;
    MemLt::poison = 0;
    std::array<MemLt, 0> mz{};
    std::array<MemLt, 0> mw{};
    const bool mzero = (mz < mw);
    const int calls_zero = MemLt::calls;

    std::array<MemLt, 3> pa{MemLt(1), MemLt(2), MemLt(-1)};
    std::array<MemLt, 3> pb{MemLt(1), MemLt(9), MemLt(-1)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_lt = (pa < pb);
    const int calls_poison = MemLt::calls;
    const int poison_reads = MemLt::poison;

    std::array<MemLt, 3> ga{MemLt(2), MemLt(-1), MemLt(-1)};
    std::array<MemLt, 3> gb{MemLt(1), MemLt(0), MemLt(0)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_gt = (ga < gb);
    const int calls_pgt = MemLt::calls;
    const int poison_gt_reads = MemLt::poison;

    std::array<NeverLt, 3> na{NeverLt(1), NeverLt(2), NeverLt(3)};
    std::array<NeverLt, 3> nb{NeverLt(9), NeverLt(8), NeverLt(7)};
    NeverLt::calls = 0;
    const bool never = (na < nb);
    const int calls_never = NeverLt::calls;

    NeverLt::calls = 0;
    std::array<NeverLt, 0> nz{};
    std::array<NeverLt, 0> nw{};
    const bool never_zero = (nz < nw);
    const int calls_nzero = NeverLt::calls;

    std::printf(
        "ltm same=%d mid=%d head=%d tail=%d gt=%d early=%d zero=%d poison=%d pgt=%d never=%d nzero=%d calls same=%d "
        "mid=%d head=%d tail=%d gt=%d early=%d zero=%d poison=%d pread=%d pgt=%d pgread=%d never=%d nzero=%d\n",
        int(ms), int(mmd), int(mhd), int(mtd), int(mgd), int(med), int(mzero), int(poison_lt), int(poison_gt),
        int(never), int(never_zero), calls_same, calls_mid, calls_head, calls_tail, calls_gt, calls_early, calls_zero,
        calls_poison, poison_reads, calls_pgt, poison_gt_reads, calls_never, calls_nzero);
}

// operator> uses operator< on the elements. A negative value past the
// deciding element must not be read. NeverLt is never less, so two arrays
// of it are equivalent through N and neither is greater.
static void greater() {
    std::array<int, 3> a{1, 2, 3};
    std::array<int, 3> same{1, 2, 3};
    std::array<int, 3> mid{1, 0, 3};
    std::array<int, 3> head{0, 2, 3};
    std::array<int, 3> tail{1, 2, 4};
    std::array<int, 3> early{2, 0, 0};
    std::array<int, 3> early_r{1, 9, 9};
    std::array<int, 1> one{5};
    std::array<int, 1> one_hi{6};
    std::array<int, 1> one_same{5};
    const std::array<int, 2> ca{4, 5};
    const std::array<int, 2> cb{4, 5};
    const std::array<int, 2> cc{4, 6};
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    std::printf(
        "gt same=%d mid=%d head=%d tail=%d lt=%d early=%d one=%d oned=%d oneeq=%d self=%d csame=%d cdiff=%d zero=%d\n",
        int(a > same), int(a > mid), int(a > head), int(tail > a), int(a > tail), int(early > early_r),
        int(one_hi > one), int(one > one_hi), int(one > one_same), int(a > a), int(ca > cb), int(cc > ca), int(z > w));

    std::array<MemLt, 3> ma{MemLt(1), MemLt(2), MemLt(3)};
    std::array<MemLt, 3> mb{MemLt(1), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool ms = (ma > mb);
    const int calls_same = MemLt::calls;

    std::array<MemLt, 3> mm{MemLt(1), MemLt(0), MemLt(3)};
    MemLt::calls = 0;
    const bool mmd = (ma > mm);
    const int calls_mid = MemLt::calls;

    std::array<MemLt, 3> mh{MemLt(0), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool mhd = (ma > mh);
    const int calls_head = MemLt::calls;

    std::array<MemLt, 3> mt{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mtd = (mt > ma);
    const int calls_tail = MemLt::calls;

    std::array<MemLt, 3> ml{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mld = (ma > ml);
    const int calls_lt = MemLt::calls;

    std::array<MemLt, 3> me{MemLt(2), MemLt(0), MemLt(0)};
    std::array<MemLt, 3> mer{MemLt(1), MemLt(9), MemLt(9)};
    MemLt::calls = 0;
    const bool med = (me > mer);
    const int calls_early = MemLt::calls;

    MemLt::calls = 0;
    MemLt::poison = 0;
    std::array<MemLt, 0> mz{};
    std::array<MemLt, 0> mw{};
    const bool mzero = (mz > mw);
    const int calls_zero = MemLt::calls;

    std::array<MemLt, 3> pa{MemLt(1), MemLt(9), MemLt(-1)};
    std::array<MemLt, 3> pb{MemLt(1), MemLt(2), MemLt(-1)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_gt = (pa > pb);
    const int calls_poison = MemLt::calls;
    const int poison_reads = MemLt::poison;

    std::array<MemLt, 3> ga{MemLt(1), MemLt(-1), MemLt(-1)};
    std::array<MemLt, 3> gb{MemLt(2), MemLt(0), MemLt(0)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_lt = (ga > gb);
    const int calls_plt = MemLt::calls;
    const int poison_lt_reads = MemLt::poison;

    std::array<NeverLt, 3> na{NeverLt(1), NeverLt(2), NeverLt(3)};
    std::array<NeverLt, 3> nb{NeverLt(9), NeverLt(8), NeverLt(7)};
    NeverLt::calls = 0;
    const bool never = (na > nb);
    const int calls_never = NeverLt::calls;

    NeverLt::calls = 0;
    std::array<NeverLt, 0> nz{};
    std::array<NeverLt, 0> nw{};
    const bool never_zero = (nz > nw);
    const int calls_nzero = NeverLt::calls;

    std::printf(
        "gtm same=%d mid=%d head=%d tail=%d lt=%d early=%d zero=%d poison=%d plt=%d never=%d nzero=%d calls same=%d "
        "mid=%d head=%d tail=%d lt=%d early=%d zero=%d poison=%d pread=%d plt=%d plread=%d never=%d nzero=%d\n",
        int(ms), int(mmd), int(mhd), int(mtd), int(mld), int(med), int(mzero), int(poison_gt), int(poison_lt),
        int(never), int(never_zero), calls_same, calls_mid, calls_head, calls_tail, calls_lt, calls_early, calls_zero,
        calls_poison, poison_reads, calls_plt, poison_lt_reads, calls_never, calls_nzero);
}

// operator>= uses operator< on the elements. A negative value past the
// deciding element must not be read. NeverLt is never less, so two arrays
// of it are equivalent through N and each is greater-or-equal. N == 0 is
// true and does not read an element.
static void geq() {
    std::array<int, 3> a{1, 2, 3};
    std::array<int, 3> same{1, 2, 3};
    std::array<int, 3> mid{1, 0, 3};
    std::array<int, 3> head{0, 2, 3};
    std::array<int, 3> tail{1, 2, 4};
    std::array<int, 3> early{2, 0, 0};
    std::array<int, 3> early_r{1, 9, 9};
    std::array<int, 1> one{5};
    std::array<int, 1> one_hi{6};
    std::array<int, 1> one_same{5};
    const std::array<int, 2> ca{4, 5};
    const std::array<int, 2> cb{4, 5};
    const std::array<int, 2> cc{4, 6};
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    std::printf(
        "ge same=%d mid=%d head=%d tail=%d lt=%d early=%d one=%d oned=%d oneeq=%d self=%d csame=%d cdiff=%d zero=%d\n",
        int(a >= same), int(a >= mid), int(a >= head), int(tail >= a), int(a >= tail), int(early >= early_r),
        int(one_hi >= one), int(one >= one_hi), int(one >= one_same), int(a >= a), int(ca >= cb), int(cc >= ca),
        int(z >= w));

    std::array<MemLt, 3> ma{MemLt(1), MemLt(2), MemLt(3)};
    std::array<MemLt, 3> mb{MemLt(1), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool ms = (ma >= mb);
    const int calls_same = MemLt::calls;

    std::array<MemLt, 3> mm{MemLt(1), MemLt(0), MemLt(3)};
    MemLt::calls = 0;
    const bool mmd = (ma >= mm);
    const int calls_mid = MemLt::calls;

    std::array<MemLt, 3> mh{MemLt(0), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool mhd = (ma >= mh);
    const int calls_head = MemLt::calls;

    std::array<MemLt, 3> mt{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mtd = (mt >= ma);
    const int calls_tail = MemLt::calls;

    std::array<MemLt, 3> ml{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mld = (ma >= ml);
    const int calls_lt = MemLt::calls;

    std::array<MemLt, 3> me{MemLt(2), MemLt(0), MemLt(0)};
    std::array<MemLt, 3> mer{MemLt(1), MemLt(9), MemLt(9)};
    MemLt::calls = 0;
    const bool med = (me >= mer);
    const int calls_early = MemLt::calls;

    MemLt::calls = 0;
    MemLt::poison = 0;
    std::array<MemLt, 0> mz{};
    std::array<MemLt, 0> mw{};
    const bool mzero = (mz >= mw);
    const int calls_zero = MemLt::calls;

    std::array<MemLt, 3> pa{MemLt(1), MemLt(9), MemLt(-1)};
    std::array<MemLt, 3> pb{MemLt(1), MemLt(2), MemLt(-1)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_ge = (pa >= pb);
    const int calls_poison = MemLt::calls;
    const int poison_reads = MemLt::poison;

    std::array<MemLt, 3> ga{MemLt(1), MemLt(-1), MemLt(-1)};
    std::array<MemLt, 3> gb{MemLt(2), MemLt(0), MemLt(0)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_lt = (ga >= gb);
    const int calls_plt = MemLt::calls;
    const int poison_lt_reads = MemLt::poison;

    std::array<NeverLt, 3> na{NeverLt(1), NeverLt(2), NeverLt(3)};
    std::array<NeverLt, 3> nb{NeverLt(9), NeverLt(8), NeverLt(7)};
    NeverLt::calls = 0;
    const bool never = (na >= nb);
    const int calls_never = NeverLt::calls;

    NeverLt::calls = 0;
    std::array<NeverLt, 0> nz{};
    std::array<NeverLt, 0> nw{};
    const bool never_zero = (nz >= nw);
    const int calls_nzero = NeverLt::calls;

    std::printf(
        "gem same=%d mid=%d head=%d tail=%d lt=%d early=%d zero=%d poison=%d plt=%d never=%d nzero=%d calls same=%d "
        "mid=%d head=%d tail=%d lt=%d early=%d zero=%d poison=%d pread=%d plt=%d plread=%d never=%d nzero=%d\n",
        int(ms), int(mmd), int(mhd), int(mtd), int(mld), int(med), int(mzero), int(poison_ge), int(poison_lt),
        int(never), int(never_zero), calls_same, calls_mid, calls_head, calls_tail, calls_lt, calls_early, calls_zero,
        calls_poison, poison_reads, calls_plt, poison_lt_reads, calls_never, calls_nzero);
}

// operator<= uses operator< on the elements. A negative value past the
// deciding element must not be read. NeverLt is never less, so two arrays
// of it are equivalent through N and each is less-or-equal. N == 0 is
// true and does not read an element.
static void leq() {
    std::array<int, 3> a{1, 2, 3};
    std::array<int, 3> same{1, 2, 3};
    std::array<int, 3> mid{1, 0, 3};
    std::array<int, 3> head{0, 2, 3};
    std::array<int, 3> tail{1, 2, 4};
    std::array<int, 3> early{2, 0, 0};
    std::array<int, 3> early_r{1, 9, 9};
    std::array<int, 1> one{5};
    std::array<int, 1> one_hi{6};
    std::array<int, 1> one_same{5};
    const std::array<int, 2> ca{4, 5};
    const std::array<int, 2> cb{4, 5};
    const std::array<int, 2> cc{4, 6};
    std::array<int, 0> z{};
    std::array<int, 0> w{};
    std::printf(
        "le same=%d mid=%d head=%d tail=%d gt=%d early=%d one=%d oned=%d oneeq=%d self=%d csame=%d cdiff=%d zero=%d\n",
        int(a <= same), int(mid <= a), int(head <= a), int(a <= tail), int(tail <= a), int(early_r <= early),
        int(one <= one_hi), int(one_hi <= one), int(one <= one_same), int(a <= a), int(ca <= cb), int(ca <= cc),
        int(z <= w));

    std::array<MemLt, 3> ma{MemLt(1), MemLt(2), MemLt(3)};
    std::array<MemLt, 3> mb{MemLt(1), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool ms = (ma <= mb);
    const int calls_same = MemLt::calls;

    std::array<MemLt, 3> mm{MemLt(1), MemLt(0), MemLt(3)};
    MemLt::calls = 0;
    const bool mmd = (mm <= ma);
    const int calls_mid = MemLt::calls;

    std::array<MemLt, 3> mh{MemLt(0), MemLt(2), MemLt(3)};
    MemLt::calls = 0;
    const bool mhd = (mh <= ma);
    const int calls_head = MemLt::calls;

    std::array<MemLt, 3> mt{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mtd = (ma <= mt);
    const int calls_tail = MemLt::calls;

    std::array<MemLt, 3> mg{MemLt(1), MemLt(2), MemLt(4)};
    MemLt::calls = 0;
    const bool mgd = (mg <= ma);
    const int calls_gt = MemLt::calls;

    std::array<MemLt, 3> me{MemLt(1), MemLt(9), MemLt(9)};
    std::array<MemLt, 3> mer{MemLt(2), MemLt(0), MemLt(0)};
    MemLt::calls = 0;
    const bool med = (me <= mer);
    const int calls_early = MemLt::calls;

    MemLt::calls = 0;
    MemLt::poison = 0;
    std::array<MemLt, 0> mz{};
    std::array<MemLt, 0> mw{};
    const bool mzero = (mz <= mw);
    const int calls_zero = MemLt::calls;

    std::array<MemLt, 3> pa{MemLt(1), MemLt(2), MemLt(-1)};
    std::array<MemLt, 3> pb{MemLt(1), MemLt(9), MemLt(-1)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_le = (pa <= pb);
    const int calls_poison = MemLt::calls;
    const int poison_reads = MemLt::poison;

    std::array<MemLt, 3> ga{MemLt(2), MemLt(-1), MemLt(-1)};
    std::array<MemLt, 3> gb{MemLt(1), MemLt(0), MemLt(0)};
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool poison_gt = (ga <= gb);
    const int calls_pgt = MemLt::calls;
    const int poison_gt_reads = MemLt::poison;

    std::array<NeverLt, 3> na{NeverLt(1), NeverLt(2), NeverLt(3)};
    std::array<NeverLt, 3> nb{NeverLt(9), NeverLt(8), NeverLt(7)};
    NeverLt::calls = 0;
    const bool never = (na <= nb);
    const int calls_never = NeverLt::calls;

    NeverLt::calls = 0;
    std::array<NeverLt, 0> nz{};
    std::array<NeverLt, 0> nw{};
    const bool never_zero = (nz <= nw);
    const int calls_nzero = NeverLt::calls;

    std::printf(
        "lem same=%d mid=%d head=%d tail=%d gt=%d early=%d zero=%d poison=%d pgt=%d never=%d nzero=%d calls same=%d "
        "mid=%d head=%d tail=%d gt=%d early=%d zero=%d poison=%d pread=%d pgt=%d pgread=%d never=%d nzero=%d\n",
        int(ms), int(mmd), int(mhd), int(mtd), int(mgd), int(med), int(mzero), int(poison_le), int(poison_gt),
        int(never), int(never_zero), calls_same, calls_mid, calls_head, calls_tail, calls_gt, calls_early, calls_zero,
        calls_poison, poison_reads, calls_pgt, poison_gt_reads, calls_never, calls_nzero);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "oob") return oob();
    ints();
    equal();
    not_equal();
    less();
    greater();
    geq();
    leq();
    traced();
    return 0;
}
