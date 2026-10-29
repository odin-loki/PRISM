// Differential trace of std::forward_list (same idea as list_trace.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/forward_list) first on the include path, both
// under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// splice_after and merge are not called: the model does not declare them.
// before_begin is the sentinel. erase_after and pop_front free the node.
#include <cstdio>
#include <forward_list>
#include <iterator>
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
    std::size_t n = 0;
    std::printf("%s [", what);
    for (auto& x : l) {
        std::printf(" %d", int(x));
        ++n;
    }
    std::printf(" ] n=%zu\n", n);
}

static void ints() {
    std::forward_list<int> l;
    std::printf("max_size int=%zu char=%zu\n", l.max_size(), std::forward_list<char>().max_size());
    auto tail = l.cbefore_begin();
    for (int i = 0; i < 6; ++i) tail = l.insert_after(tail, i);
    l.push_front(-1);
    l.emplace_front(-2);
    l.emplace_after(l.begin(), 9);
    show("after ends", l);
    auto bb = l.cbefore_begin();
    ++bb;
    std::printf("before_begin steps to %d\n", *bb);
    auto at = l.begin();
    std::advance(at, 2);
    l.insert_after(at, 42);
    l.insert_after(l.cbefore_begin(), 2, 7);
    int extra[] = {8, 8, 1};
    l.insert_after(l.cbefore_begin(), extra, extra + 3);
    auto last = l.cbefore_begin();
    while (std::next(last) != l.cend()) ++last;
    l.insert_after(last, {5, 5});
    show("after insert", l);
    auto b = l.begin();
    std::advance(b, 3);
    l.erase_after(b);
    auto r = l.begin();
    auto e = r;
    std::advance(e, 3);
    l.erase_after(r, e);
    l.pop_front();
    show("after erase", l);
    l.resize(12);
    l.resize(4);
    show("after resize", l);
    l.assign(3, 1);
    l.assign({4, 5, 4, 5, 4});
    show("after assign", l);
    std::forward_list<int> w(l);
    std::printf("eq=%d lt=%d\n", int(l == w), int(l < std::forward_list<int>{4, 6}));
    std::forward_list<int> moved = std::move(w);
    auto mit = moved.begin();
    moved.insert_after(mit, 8);
    std::printf("moved front=%d second=%d src_empty=%d\n", moved.front(), *std::next(moved.begin()),
                int(w.empty()));
    w = std::move(moved);
    w.swap(l);
    show("after swap l", l);
    show("after swap w", w);
    std::forward_list<int> left{1, 2};
    std::forward_list<int> right{3};
    auto ia = left.begin();
    left.swap(right);
    right.insert_after(ia, 7);
    std::printf("swap own a=%d b0=%d b1=%d it=%d\n", left.front(), right.front(), *std::next(right.begin()), *ia);
    l.reverse();
    show("reversed", l);
    std::forward_list<int> rev{1, 2, 3, 2};
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
    std::forward_list<S> stable{{2, 1}, {1, 2}, {2, 3}, {1, 4}, {3, 5}};
    stable.sort();
    std::printf("stable");
    for (auto& s : stable) std::printf(" %d:%d", s.k, s.id);
    std::printf("\n");
    std::forward_list<int> self{1, 2, 3};
    self.insert_after(self.cbefore_begin(), self.begin(), self.end());
    show("self insert", self);
    std::forward_list<std::string> s;
    s.push_front("bb");
    s.push_front("a");
    s.emplace_after(s.begin(), 3, 'x');
    std::printf("strings %s %zu\n", s.front().c_str(), std::next(s.begin())->size());
    std::forward_list<std::unique_ptr<int>> u;
    u.push_front(std::make_unique<int>(2));
    u.emplace_front(new int(1));
    auto u2 = std::move(u);
    u2.erase_after(u2.before_begin());
    std::printf("unique %d empty_src=%d\n", *u2.front(), int(u.empty()));
    int raw[] = {1, 2, 3};
    std::forward_list deduced(raw, raw + 3);
    std::printf("deduced %d\n", deduced.front());
    std::forward_list<int> fill(4, 2);
    std::forward_list<int> n(3);
    std::printf("fill %d n0=%d\n", fill.front(), n.front());
    struct Rec {
        int v = 0;
        std::forward_list<Rec> kids;
    };
    std::forward_list<Rec> rec;
    rec.emplace_front();
    rec.front().v = 1;
    rec.front().kids.emplace_front();
    std::printf("rec %d kids=%d\n", rec.front().v, int(!rec.front().kids.empty()));
    std::forward_list<int> selfmv{1, 2, 3};
    selfmv = std::move(selfmv);
    std::printf("selfmove empty=%d\n", int(selfmv.empty()));
}

static void traced() {
    {
        std::forward_list<E> l;
        auto p = l.cbefore_begin();
        for (int i = 0; i < 4; ++i) p = l.emplace_after(p, i);
        std::printf("\n");
        l.insert_after(l.cbefore_begin(), E(9));
        std::printf("\n");
        l.erase_after(l.begin());
        std::printf("\n");
        std::forward_list<E> w = l;
        std::printf("\n");
        w = std::forward_list<E>{E(5), E(6)};
        std::printf("\n");
        l.remove(1);
        std::printf("\nremoved\n");
        l.unique();
        std::printf("\n");
        g_throw_in = 2;
        try {
            l.insert_after(l.cbefore_begin(), 3, E(7));
        } catch (const std::runtime_error&) {
            std::printf(" threw");
        }
        g_throw_in = -1;
        std::printf("\n");
    }
    std::printf("\n");
    {
        std::forward_list<int> l{3, 1, 4, 1, 5, 9, 2, 6};
        l.sort();
        show("sorted pi", l);
    }
}

static int uaf() {
    std::forward_list<int> l;
    l.push_front(2);
    l.push_front(1);
    auto it = l.begin();
    l.erase_after(l.before_begin());
    return *it;
}

static int pop_uaf() {
    std::forward_list<int> l;
    l.push_front(2);
    l.push_front(1);
    auto it = l.begin();
    l.pop_front();
    return *it;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    if (argc > 1 && std::string(argv[1]) == "pop") return pop_uaf();
    ints();
    traced();
    return 0;
}
