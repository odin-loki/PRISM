// Differential trace of std::vector (tests/cpp/test_cxx_models.cpp): compiled once
// against libstdc++ and once with PRISM's model headers
// (src/prism/pir/models/cxx) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS; the two traces must be identical
// (sizes, capacities, contents, element constructions/destructions in order,
// exceptions).
// operator!= is the negation of operator==. Two empty vectors compare
// not-equal as false. operator== compares sizes first; when both sizes are
// 0 it still calls std::equal, and that empty range does not load an
// element. A size mismatch is false before std::equal. A mismatch of equal
// length stops at the first difference inside operator==.
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The models call __prism_check, the precondition intrinsic. libstdc++ never
// calls it. A failed check aborts so the model and libstdc++ runs still differ.
extern "C" __attribute__((weak)) void __prism_check(int ok, const char*, const char*) {
    if (!ok) __builtin_abort();
}

static int g_throw_in = -1;

struct E {
    int v;
    E(int x = 0) : v(x) { std::printf(" c%d", v); }
    E(const E& o) : v(o.v) {
        if (g_throw_in >= 0 && g_throw_in-- == 0) {
            std::printf(" throw%d", v);
            throw std::runtime_error("copy");
        }
        std::printf(" cc%d", v);
    }
    E& operator=(const E& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
    friend bool operator==(const E& a, const E& b) { return a.v == b.v; }
};

struct M {  // nothrow-movable: relocated on growth
    int v;
    M(int x = 0) : v(x) { std::printf(" c%d", v); }
    M(const M& o) : v(o.v) { std::printf(" cc%d", v); }
    M(M&& o) noexcept : v(o.v) { std::printf(" mc%d", v); }
    M& operator=(const M& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    M& operator=(M&& o) noexcept {
        v = o.v;
        std::printf(" m=%d", v);
        return *this;
    }
    ~M() { std::printf(" d%d", v); }
};

template <typename V>
static void show(const char* what, const V& v) {
    std::printf("\n%s: size=%zu cap=%zu [", what, v.size(), v.capacity());
    for (auto& x : v) std::printf(" %d", int(x.v));
    std::printf(" ]\n");
}

static void ints() {
    std::vector<int> v;
    std::printf("max_size=%zu\n", v.max_size());
    for (int i = 0; i < 9; ++i) {
        v.push_back(i);
        std::printf("push %d cap=%zu\n", i, v.capacity());
    }
    v.insert(v.begin() + 3, 2, 42);
    v.insert(v.begin() + 1, {7, 8, 9});
    v.insert(v.end(), 5);
    v.erase(v.begin() + 2, v.begin() + 5);
    v.erase(v.begin());
    v.resize(20);
    v.resize(4);
    v.shrink_to_fit();
    std::printf("after edits cap=%zu:", v.capacity());
    for (int x : v) std::printf(" %d", x);
    std::printf("\n");
    v.assign(3, 1);
    v.assign({4, 5});
    std::vector<int> w(v);
    std::printf("eq=%d lt=%d\n", int(v == w), int(v < std::vector<int>{4, 6}));
    std::printf("erase_if=%zu size=%zu\n", std::erase_if(v, [](int x) { return x == 4; }), v.size());
    try {
        (void)v.at(10);
    } catch (const std::out_of_range& e) {
        std::printf("at: %s\n", e.what());
    }
    try {
        v.reserve(v.max_size() + 1);
    } catch (const std::length_error& e) {
        std::printf("reserve: %s\n", e.what());
    }
    std::vector<std::string> s{"a", "bb"};
    s.emplace_back(40, 'x');
    s.insert(s.begin(), "front");
    std::printf("strings %zu %s %zu\n", s.size(), s[0].c_str(), s.back().size());
    std::vector<std::unique_ptr<int>> u;
    u.push_back(std::make_unique<int>(1));
    u.emplace_back(new int(2));
    u.erase(u.begin());
    std::printf("unique %d\n", *u[0]);
    std::vector d(3, 1.5);
    std::printf("deduced %zu\n", d.size());
}

static void traced() {
    {
        std::vector<E> v;
        for (int i = 0; i < 4; ++i) v.push_back(E(i));
        show("E push", v);
        v.insert(v.begin() + 1, E(9));
        show("E insert", v);
        v.erase(v.begin() + 2);
        show("E erase", v);
        std::vector<E> w = v;
        show("E copy", w);
        w = std::vector<E>(2, E(5));
        show("E assign", w);
        g_throw_in = 2;
        try {
            v.push_back(E(7));
        } catch (const std::runtime_error&) {
            std::printf("\nthrew");
        }
        g_throw_in = -1;
        show("E strong guarantee", v);
    }
    std::printf("\n");
    {
        std::vector<M> v;
        for (int i = 0; i < 4; ++i) v.emplace_back(i);
        show("M emplace", v);
        v.insert(v.begin(), M(8));
        show("M insert", v);
        v.reserve(20);
        show("M reserve", v);
        v.shrink_to_fit();
        show("M shrink", v);
        v.resize(7);
        show("M resize", v);
    }
    std::printf("\n");
}

struct Load {
    int v;
    static int calls;
    static int reads;
    static int ctors;
    explicit Load(int x) : v(x) { ++ctors; }
    Load(const Load& o) : v(o.v) { ++ctors; }
    friend bool operator==(const Load& a, const Load& b) {
        ++calls;
        if (a.v < 0 || b.v < 0) ++reads;
        return a.v == b.v;
    }
};
int Load::calls = 0;
int Load::reads = 0;
int Load::ctors = 0;

struct NoDef {
    int v;
    explicit NoDef(int x) : v(x) {}
    NoDef(const NoDef&) = default;
    friend bool operator==(const NoDef& a, const NoDef& b) { return a.v == b.v; }
};

static std::vector<Load> load_vec(std::initializer_list<int> xs) {
    std::vector<Load> v;
    v.reserve(xs.size());
    for (int x : xs) v.emplace_back(x);
    return v;
}

struct NeCount {
    int ne = 0;
    int calls = 0;
    int reads = 0;
    int ctors = 0;
};

static NeCount count_ne(std::vector<Load>& a, std::vector<Load>& b) {
    Load::calls = 0;
    Load::reads = 0;
    Load::ctors = 0;
    NeCount c;
    c.ne = int(a != b);
    c.calls = Load::calls;
    c.reads = Load::reads;
    c.ctors = Load::ctors;
    return c;
}

static void not_equal() {
    std::vector<int> empty_a, empty_b;
    std::vector<int> same{1, 2, 3};
    std::vector<int> copy{1, 2, 3};
    std::vector<int> head{9, 2, 3};
    std::vector<int> mid{1, 9, 3};
    std::vector<int> tail{1, 2, 9};
    std::vector<int> shorter{1, 2};
    std::vector<int> longer{1, 2, 3, 4};
    std::vector<int> cleared_a{1, 2};
    std::vector<int> cleared_b{9};
    cleared_a.clear();
    cleared_b.clear();
    std::vector<int> reserved_a, reserved_b;
    reserved_a.reserve(8);
    reserved_b.reserve(1);
    const std::vector<int> csame{1, 2, 3};
    std::vector<NoDef> nd_a, nd_b;
    std::printf("ne empty=%d same=%d head=%d mid=%d tail=%d short=%d long=%d self=%d clear=%d reserve=%d csame=%d "
                "nodef=%d\n",
                int(empty_a != empty_b), int(same != copy), int(same != head), int(same != mid), int(same != tail),
                int(same != shorter), int(shorter != longer), int(same != same), int(cleared_a != cleared_b),
                int(reserved_a != reserved_b), int(csame != copy), int(nd_a != nd_b));

    auto e1 = load_vec({});
    auto e2 = load_vec({});
    const NeCount empty = count_ne(e1, e2);
    auto same_l = load_vec({1, 2, 3});
    auto same_r = load_vec({1, 2, 3});
    const NeCount same_c = count_ne(same_l, same_r);
    auto head_r = load_vec({9, 2, 3});
    const NeCount head_c = count_ne(same_l, head_r);
    auto mid_r = load_vec({1, 9, 3});
    const NeCount mid_c = count_ne(same_l, mid_r);
    auto tail_r = load_vec({1, 2, 9});
    const NeCount tail_c = count_ne(same_l, tail_r);
    auto len_l = load_vec({1, -1});
    auto len_r = load_vec({1});
    const NeCount len_c = count_ne(len_l, len_r);
    auto poison_l = load_vec({1, 2, -1});
    auto poison_r = load_vec({1, 9, -1});
    const NeCount poison_c = count_ne(poison_l, poison_r);
    auto eq_l = load_vec({1, -1});
    auto eq_r = load_vec({1, -1});
    const NeCount eq_c = count_ne(eq_l, eq_r);
    const int ctors = empty.ctors + same_c.ctors + head_c.ctors + mid_c.ctors + tail_c.ctors + len_c.ctors +
                      poison_c.ctors + eq_c.ctors;
    std::printf("nel empty=%d/%d same=%d/%d head=%d/%d mid=%d/%d tail=%d/%d len=%d/%d lreads=%d poison=%d/%d "
                "preads=%d eqneg=%d/%d/%d ctors=%d\n",
                empty.ne, empty.calls, same_c.ne, same_c.calls, head_c.ne, head_c.calls, mid_c.ne, mid_c.calls,
                tail_c.ne, tail_c.calls, len_c.ne, len_c.calls, len_c.reads, poison_c.ne, poison_c.calls,
                poison_c.reads, eq_c.ne, eq_c.calls, eq_c.reads, ctors);
}

int main() {
    ints();
    traced();
    not_equal();
    return 0;
}
