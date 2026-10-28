// Differential trace of std::array. Compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/array) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical. No heap: `oob` is operator[] at size(), which aborts in
// the _GLIBCXX_ASSERTIONS check. operator== is the same N: N == 0 is true
// and does not read an element, and N > 0 stops at the first mismatch.
// Iterators, operator<=>, std::get, and std::to_array are not called.
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

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "oob") return oob();
    ints();
    equal();
    traced();
    return 0;
}
