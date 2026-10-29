// Differential trace of std::list (tests/cpp/test_cxx_list.cpp): compiled once
// against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/list) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// splice and merge are not called: the model does not declare them.
#include <cstdio>
#include <iterator>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

struct S {  // stable-sort key
    int k, id;
    bool operator<(const S& o) const { return k < o.k; }
};

template <typename L>
static void show(const char* what, const L& l) {
    std::printf("%s size=%zu [", what, l.size());
    for (auto& x : l) std::printf(" %d", int(x));
    std::printf(" ] rev [");
    for (auto it = l.rbegin(); it != l.rend(); ++it) std::printf(" %d", int(*it));
    std::printf(" ]\n");
}

static void ints() {
    std::list<int> l;
    std::printf("max_size int=%zu char=%zu\n", l.max_size(), std::list<char>().max_size());
    for (int i = 0; i < 6; ++i) l.push_back(i);
    l.push_front(-1);
    l.emplace_back(9);
    l.emplace_front(-2);
    show("after ends", l);
    auto at = l.begin();
    std::advance(at, 2);
    l.insert(at, 42);
    l.insert(l.begin(), 2, 7);
    int extra[] = {8, 8, 1};
    l.insert(l.end(), extra, extra + 3);
    l.insert(l.end(), {5, 5});
    show("after insert", l);
    auto b = l.begin();
    std::advance(b, 3);
    l.erase(b);
    auto r = l.begin();
    std::advance(r, 1);
    auto e = r;
    std::advance(e, 3);
    l.erase(r, e);
    l.pop_front();
    l.pop_back();
    show("after erase", l);
    l.resize(12);
    l.resize(4);
    show("after resize", l);
    l.assign(3, 1);
    l.assign({4, 5, 4, 5, 4});
    show("after assign", l);
    std::list<int> w(l);
    std::printf("eq=%d lt=%d\n", int(l == w), int(l < std::list<int>{4, 6}));
    std::list<int> moved = std::move(w);
    std::printf("moved size=%zu src=%zu front=%d\n", moved.size(), w.size(), moved.front());
    w = std::move(moved);
    w.swap(l);
    show("after swap l", l);
    show("after swap w", w);
    l.reverse();
    show("reversed", l);
    std::list<int> rev{1, 2, 3, 2};
    rev.reverse();
    show("reverse 1232", rev);
    l.sort();
    show("sorted", l);
    std::printf("unique=%zu\n", l.unique());
    show("unique", l);
    std::printf("remove=%zu\n", l.remove(4));
    show("remove", l);
    std::printf("erase_if=%zu\n", std::erase_if(l, [](int x) { return x == 5; }));
    show("erase_if", l);
    std::list<S> stable{{2, 1}, {1, 2}, {2, 3}, {1, 4}, {3, 5}};
    stable.sort();
    std::printf("stable");
    for (auto& s : stable) std::printf(" %d:%d", s.k, s.id);
    std::printf("\n");
    std::list<int> self{1, 2, 3};
    self.insert(self.end(), self.begin(), self.end());
    show("self insert", self);
    std::list<std::string> s{"a", "bb"};
    s.emplace_back(3, 'x');
    s.push_front("front");
    std::printf("strings %zu %s %zu\n", s.size(), s.front().c_str(), s.back().size());
    std::list<std::unique_ptr<int>> u;
    u.push_back(std::make_unique<int>(1));
    u.emplace_back(new int(2));
    auto u2 = std::move(u);
    u2.erase(u2.begin());
    std::printf("unique %d %zu %zu\n", *u2.front(), u2.size(), u.size());
    int raw[] = {1, 2, 3};
    std::list deduced(raw, raw + 3);
    std::printf("deduced %zu %d\n", deduced.size(), deduced.back());
    std::list<int> fill(4, 2);
    std::list<int> n(3);
    std::printf("fill %zu %d n %zu %d\n", fill.size(), fill.front(), n.size(), n.back());
    struct Rec {
        int v = 0;
        std::list<Rec> kids;
    };
    std::list<Rec> rec;
    rec.emplace_back();
    rec.back().v = 1;
    rec.back().kids.emplace_back();
    std::printf("rec %d %zu\n", rec.front().v, rec.front().kids.size());
}

static void traced() {
    {
        std::list<E> l;
        for (int i = 0; i < 4; ++i) l.emplace_back(i);
        std::printf("\n");
        l.insert(std::next(l.begin()), E(9));
        std::printf("\n");
        l.erase(std::next(l.begin(), 2));
        std::printf("\n");
        std::list<E> w = l;
        std::printf("\n");
        w = std::list<E>{E(5), E(6)};
        std::printf("\n");
        l.remove(1);
        std::printf("\nremoved\n");
        l.unique();
        std::printf("\n");
        g_throw_in = 2;
        try {
            l.insert(l.end(), 3, E(7));
        } catch (const std::runtime_error&) {
            std::printf(" threw");
        }
        g_throw_in = -1;
        std::printf("\n");
    }
    std::printf("\n");
    {
        std::list<int> l{3, 1, 4, 1, 5, 9, 2, 6};
        struct Boom {
            int left;
            bool operator()(int a, int b) {
                if (left >= 0 && left-- == 0) throw std::runtime_error("cmp");
                return a < b;
            }
        };
        try {
            l.sort(Boom{0});
        } catch (const std::runtime_error&) {
            std::printf("sort threw");
        }
        show(" after throw", l);
        l.sort();
        show("resorted", l);
    }
}

static int uaf() {
    std::list<int> l;
    l.push_back(1);
    l.push_back(2);
    auto it = l.begin();
    l.erase(it);
    return *it;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    ints();
    traced();
    return 0;
}
