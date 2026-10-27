// Differential trace of std::unordered_map / std::unordered_set (tests/cpp/test_cxx_models.cpp).
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static int g_live = 0, g_ctor = 0, g_copy = 0, g_move = 0;

struct T {
    int v;
    T(int x = 0) : v(x) {
        ++g_live;
        ++g_ctor;
    }
    T(const T& o) : v(o.v) {
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
    friend bool operator==(const T& a, const T& b) { return a.v == b.v; }
};

struct Hash {
    size_t operator()(int x) const { return static_cast<size_t>(x); }
};

static void counts(const char* what) {
    std::printf("%s: live=%d ctor=%d copy=%d move=%d\n", what, g_live, g_ctor, g_copy, g_move);
}

template <typename M>
static void dump_umap(const char* what, const M& m) {
    std::vector<std::pair<int, int>> v;
    for (const auto& p : m) v.emplace_back(p.first, p.second.v);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::printf("%s size=%zu buckets=%zu load=%.3f [", what, m.size(), m.bucket_count(), m.load_factor());
    for (auto& [k, val] : v) std::printf(" %d:%d", k, int(val));
    std::printf(" ]\n");
}

template <typename S>
static void dump_uset(const char* what, const S& s) {
    std::vector<int> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    std::printf("%s size=%zu buckets=%zu [", what, s.size(), s.bucket_count());
    for (int x : v) std::printf(" %d", x);
    std::printf(" ]\n");
}

static void umap_basics() {
    std::unordered_map<int, T> m;
    std::printf("empty=%d buckets=%zu\n", int(m.empty()), m.bucket_count());
    auto r1 = m.insert({3, T(30)});
    auto r2 = m.insert({1, T(10)});
    auto r3 = m.insert({3, T(33)});
    std::printf("insert %d %d %d val=%d\n", int(r1.second), int(r2.second), int(r3.second), r3.first->second.v);
    counts("after insert");
    m.emplace(4, T(40));
    m.find(4)->second.v += 1;
    auto t1 = m.try_emplace(2, 20);
    auto t2 = m.try_emplace(2, 22);
    std::printf("try_emplace %d %d\n", int(t1.second), int(t2.second));
    auto io1 = m.insert_or_assign(6, T(60));
    auto io2 = m.insert_or_assign(6, T(66));
    std::printf("insert_or_assign %d %d\n", int(io1.second), int(io2.second));
    dump_umap("umap", m);
    std::printf("find=%d count=%zu contains=%d,%d\n", m.find(6)->second.v, m.count(6), int(m.contains(1)),
                int(m.contains(99)));
    try {
        (void)m.at(42);
    } catch (const std::out_of_range& e) {
        std::printf("at threw: %s\n", e.what());
    }
    std::printf("at=%d\n", m.at(1).v);
    m.reserve(32);
    std::printf("after reserve buckets=%zu\n", m.bucket_count());
    std::printf("erase key %zu\n", m.erase(3));
    dump_umap("umap", m);
    std::unordered_map<int, T> c(m);
    counts("after copy");
    std::printf("eq=%d\n", int(c == m));
    c.clear();
    counts("after clear");
}

static void uset_basics() {
    std::unordered_set<int> s;
    s.insert(3);
    s.insert(1);
    s.insert(3);
    dump_uset("uset", s);
    std::printf("find=%d erase=%zu\n", *s.find(1), s.erase(1));
    dump_uset("uset", s);
    std::unordered_multiset<int> ms{1, 2, 1, 3};
    std::printf("multiset count2=%zu erase1=%zu\n", ms.count(2), ms.erase(1));
    std::printf("erase_if=%zu\n", std::erase_if(s, [](int x) { return x % 2 == 0; }));
}

static int uaf() {
    std::unordered_map<int, int> m{{1, 1}, {2, 2}};
    auto it = m.find(1);
    m.erase(1);
    return it->second;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    umap_basics();
    uset_basics();
    counts("end");
    return 0;
}
