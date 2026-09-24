// Differential trace of std::vector (tests/test_cxx_models.py): compiled once
// against libstdc++ and once with PRISM's model headers
// (src/prism/pir/models/cxx) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS; the two traces must be identical
// (sizes, capacities, contents, element constructions/destructions in order,
// exceptions).
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

int main() {
    ints();
    traced();
    return 0;
}
