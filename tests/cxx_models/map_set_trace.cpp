// Differential trace of std::map / std::multimap / std::set / std::multiset
// (tests/test_cxx_models.py): compiled once against libstdc++ and once with
// PRISM's model headers (src/prism/pir/models/cxx) first on the include
// path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS; the two traces must
// be identical. Element construction/destruction is traced as counts (the
// standard leaves the order inside copies and clear() unspecified).
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

static int g_live = 0, g_ctor = 0, g_copy = 0, g_move = 0, g_throw_in = -1;

struct T {
    int v;
    T(int x = 0) : v(x) {
        ++g_live;
        ++g_ctor;
    }
    T(const T& o) : v(o.v) {
        if (g_throw_in >= 0 && g_throw_in-- == 0) throw std::runtime_error("copy");
        ++g_live;
        ++g_copy;
    }
    T(T&& o) noexcept : v(o.v) {
        ++g_live;
        ++g_move;
    }
    T& operator=(const T& o) {
        v = o.v;
        return *this;
    }
    T& operator=(T&& o) noexcept {
        v = o.v;
        return *this;
    }
    ~T() { --g_live; }
    friend bool operator<(const T& a, const T& b) { return a.v < b.v; }
    friend bool operator==(const T& a, const T& b) { return a.v == b.v; }
    friend auto operator<=>(const T& a, const T& b) { return a.v <=> b.v; }
};

static void counts(const char* what) {
    std::printf("%s: live=%d ctor=%d copy=%d move=%d\n", what, g_live, g_ctor, g_copy, g_move);
}

template <typename M>
static void dump_map(const char* what, const M& m) {
    std::printf("%s size=%zu empty=%d [", what, m.size(), int(m.empty()));
    for (auto& [k, v] : m) std::printf(" %d:%d", int(k), int(v.v));
    std::printf(" ] rev [");
    for (auto it = m.rbegin(); it != m.rend(); ++it) std::printf(" %d", int(it->first));
    std::printf(" ]\n");
}

template <typename S>
static void dump_set(const char* what, const S& s) {
    std::printf("%s size=%zu [", what, s.size());
    for (auto& k : s) std::printf(" %d", int(k.v));
    std::printf(" ]\n");
}

struct Less {  // transparent comparator
    using is_transparent = void;
    bool operator()(int a, int b) const { return a < b; }
    bool operator()(const std::string& a, int b) const { return int(a.size()) < b; }
    bool operator()(int a, const std::string& b) const { return a < int(b.size()); }
    bool operator()(const std::string& a, const std::string& b) const { return a.size() < b.size(); }
};

static void map_basics() {
    std::map<int, T> m;
    std::printf("max_size=%zu\n", m.max_size());
    auto r1 = m.insert({3, T(30)});
    auto r2 = m.insert(std::pair<const int, T>(1, T(10)));
    auto r3 = m.insert({3, T(33)});
    std::printf("insert %d %d %d -> %d\n", int(r1.second), int(r2.second), int(r3.second), r3.first->second.v);
    counts("after insert");
    auto e1 = m.emplace(5, 50);
    auto e2 = m.emplace(5, 55);
    auto e3 = m.emplace(std::piecewise_construct, std::forward_as_tuple(7), std::forward_as_tuple(70));
    auto e4 = m.emplace(std::piecewise_construct, std::forward_as_tuple(7), std::forward_as_tuple(77));
    std::printf("emplace %d %d %d %d\n", int(e1.second), int(e2.second), int(e3.second), int(e4.second));
    counts("after emplace");
    auto t1 = m.try_emplace(2, 20);
    auto t2 = m.try_emplace(2, 22);
    std::printf("try_emplace %d %d\n", int(t1.second), int(t2.second));
    counts("after try_emplace");
    m[4].v = 40;
    m[4].v += 1;
    auto io1 = m.insert_or_assign(6, T(60));
    auto io2 = m.insert_or_assign(6, T(66));
    std::printf("insert_or_assign %d %d\n", int(io1.second), int(io2.second));
    auto h = m.emplace_hint(m.end(), 9, 90);
    auto h2 = m.emplace_hint(m.begin(), 9, 99);
    std::printf("hint %d %d same=%d\n", h->first, h2->second.v, int(h == h2));
    m.insert(m.find(4), {8, T(80)});
    m.try_emplace(m.end(), 0, 0);
    m.insert_or_assign(m.begin(), 0, T(1));
    dump_map("map", m);
    counts("after hints");
    std::printf("find=%d count=%zu,%zu contains=%d,%d\n", m.find(5)->second.v, m.count(5), m.count(42),
                int(m.contains(9)), int(m.contains(10)));
    std::printf("lower=%d upper=%d\n", m.lower_bound(5)->first, m.upper_bound(5)->first);
    auto er = m.equal_range(6);
    std::printf("equal_range=%d..%d\n", er.first->first, er.second->first);
    std::printf("lower_end=%d\n", int(m.lower_bound(100) == m.end()));
    try {
        (void)m.at(42);
    } catch (const std::out_of_range& e) {
        std::printf("at threw: %s\n", e.what());
    }
    std::printf("at=%d\n", m.at(1).v);
    auto it = m.erase(m.find(3));
    std::printf("erase -> %d\n", it->first);
    std::printf("erase key %zu %zu\n", m.erase(5), m.erase(5));
    auto it2 = m.erase(m.find(6), m.find(9));
    std::printf("erase range -> %d\n", it2->first);
    dump_map("map", m);
    counts("after erase");
    std::map<int, T> c(m);
    counts("after copy");
    std::printf("eq=%d lt=%d\n", int(c == m), int(c < m));
    c[100];
    std::printf("eq=%d lt=%d gt=%d\n", int(c == m), int(m < c), int(c > m));
    std::map<int, T> mv(std::move(c));
    std::printf("moved size=%zu from=%zu\n", mv.size(), c.size());
    c = mv;
    counts("after copy assign");
    c = std::move(mv);
    counts("after move assign");
    auto keep = c.begin();
    std::map<int, T> other{{11, T(1)}, {12, T(2)}};
    c.swap(other);
    std::printf("swap: kept=%d in other=%d\n", keep->first, int(other.find(keep->first) == keep));
    dump_map("c", c);
    dump_map("other", other);
    std::printf("erase_if=%zu\n", std::erase_if(other, [](const auto& p) { return p.first % 2 == 0; }));
    dump_map("other", other);
    other.clear();
    counts("after clear");
    std::map<int, int> il{{3, 1}, {1, 2}, {3, 3}, {2, 4}};
    il = {{5, 5}, {4, 4}};
    for (auto& [k, v] : il) std::printf("il %d:%d\n", k, v);
    std::map<int, int, std::greater<int>> g{{1, 1}, {3, 3}, {2, 2}};
    for (auto& [k, v] : g) std::printf("greater %d\n", k);
    std::map dg{std::pair{1, 2}, std::pair{0, 1}};
    std::printf("deduced %d\n", dg.begin()->first);
    std::map<std::string, int, Less> tr{{"aa", 1}, {"b", 2}, {"cccc", 3}};
    std::printf("transparent %d %zu %d\n", tr.find(2)->second, tr.count(3), int(tr.contains(4)));
    auto vc = m.value_comp();
    std::printf("value_comp %d\n", int(vc(*m.begin(), *std::next(m.begin()))));
    const auto& cm = m;
    std::printf("const find %d\n", cm.find(1)->second.v);
}

static void multimap_basics() {
    std::multimap<int, T> mm;
    mm.insert({2, T(1)});
    mm.insert({2, T(2)});
    mm.emplace(1, 3);
    mm.emplace(2, 4);
    mm.insert(mm.find(2), {2, T(5)});          // before the hint
    mm.insert(mm.end(), {2, T(6)});             // at the end
    mm.insert(mm.begin(), {0, T(7)});
    mm.insert(mm.begin(), {2, T(8)});           // hint too small: lower insert
    mm.emplace_hint(mm.upper_bound(1), 2, 9);
    mm.emplace_hint(mm.end(), 1, 10);           // hint too large: upper insert
    std::printf("mm [");
    for (auto& [k, v] : mm) std::printf(" %d:%d", k, v.v);
    std::printf(" ] count2=%zu\n", mm.count(2));
    auto er = mm.equal_range(2);
    int n = 0;
    for (auto it = er.first; it != er.second; ++it) ++n;
    std::printf("range=%d erase=%zu size=%zu\n", n, mm.erase(2), mm.size());
    counts("multimap");
}

static void set_basics() {
    std::set<T> s{T(3), T(1), T(2), T(3)};
    dump_set("set", s);
    std::printf("max_size=%zu %zu\n", s.max_size(), std::multimap<char, char>().max_size());
    auto r = s.insert(T(2));
    auto r2 = s.emplace(0);
    std::printf("insert %d emplace %d\n", int(r.second), int(r2.second));
    s.emplace_hint(s.end(), 7);
    s.insert(s.begin(), T(5));
    std::printf("find %d lower %d upper %d count %zu\n", s.find(T(2))->v, s.lower_bound(T(4))->v,
                s.upper_bound(T(5))->v, s.count(T(7)));
    std::printf("erase %zu -> %d\n", s.erase(T(5)), s.erase(s.begin())->v);
    dump_set("set", s);
    std::set<T> c = s;
    std::printf("eq %d lt %d\n", int(c == s), int(c < s));
    std::multiset<T> ms{T(2), T(1), T(2)};
    ms.insert(T(2));
    ms.emplace(1);
    ms.insert(ms.begin(), T(3));
    dump_set("multiset", ms);
    std::printf("count %zu erase %zu\n", ms.count(T(2)), ms.erase(T(2)));
    std::set<int> is{5, 1, 4};
    std::printf("erase_if %zu\n", std::erase_if(is, [](int x) { return x > 3; }));
    for (int x : is) std::printf("is %d\n", x);
    std::set ded{3, 1, 2};
    std::printf("deduced %d\n", *ded.rbegin());
    counts("set");
}

static void throwing() {
    std::map<int, T> m{{1, T(1)}, {2, T(2)}, {3, T(3)}};
    std::map<int, T> d{{9, T(9)}};
    counts("before throw");
    g_throw_in = 1;
    try {
        std::map<int, T> c(m);
    } catch (const std::runtime_error&) {
        std::printf("copy threw\n");
    }
    counts("after copy throw");
    g_throw_in = 1;
    try {
        d = m;
    } catch (const std::runtime_error&) {
        std::printf("assign threw size=%zu\n", d.size());
    }
    counts("after assign throw");
    g_throw_in = 0;
    T t(4);
    try {
        m.insert({4, t});
    } catch (const std::runtime_error&) {
        std::printf("insert threw size=%zu\n", m.size());
    }
    g_throw_in = -1;
    counts("after insert throw");
}

// argv[1] == "uaf": use an iterator to an erased element (both builds must
// stop with ASan's heap-use-after-free)
static int uaf() {
    std::map<int, int> m{{1, 1}, {2, 2}};
    auto it = m.find(1);
    m.erase(1);
    return it->second;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    map_basics();
    multimap_basics();
    set_basics();
    throwing();
    counts("end");
    return 0;
}
