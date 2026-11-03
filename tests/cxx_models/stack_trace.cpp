// Differential trace of std::stack. Compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/stack) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical. `stack_trace uaf` is separate: the model frees the node
// pop destroys, and libstdc++'s deque does not free that block.
#include <cstdio>
#include <memory>
#include <stack>
#include <string>
#include <utility>

// The models call __prism_check, the precondition intrinsic. libstdc++ never
// calls it. A failed check aborts so the model and libstdc++ runs still differ.
extern "C" __attribute__((weak)) void __prism_check(int ok, const char*, const char*) {
    if (!ok) __builtin_abort();
}

struct E {
    int v;
    E(int x = 0) : v(x) { std::printf(" c%d", v); }
    E(const E& o) : v(o.v) { std::printf(" cc%d", v); }
    E(E&& o) noexcept : v(o.v) {
        o.v = -1;
        std::printf(" m%d", v);
    }
    E& operator=(const E& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    E& operator=(E&& o) noexcept {
        v = o.v;
        o.v = -1;
        std::printf(" =m%d", v);
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
    bool operator==(const E& o) const { return v == o.v; }
    bool operator<(const E& o) const { return v < o.v; }
};

static void traced() {
    using S = std::stack<E>;
    {
        S s;
        std::printf("empty %d %zu\n", int(s.empty()), s.size());
        s.push(E(1));
        std::printf(" | push-tmp\n");
        s.emplace(2);
        std::printf(" | emplace\n");
        E e(3);
        s.push(e);
        std::printf(" | push-lv top %d size %zu\n", s.top().v, s.size());
        s.pop();
        std::printf("popped top %d size %zu\n", s.top().v, s.size());
        S c = s;
        std::printf(" | copy top %d size %zu\n", c.top().v, c.size());
        S m = std::move(c);
        std::printf(" | move top %d src %zu\n", m.top().v, c.size());
        S longer;
        longer.emplace(4);
        longer.emplace(5);
        longer.emplace(6);
        longer.emplace(7);
        std::printf(" | assign-from-shorter\n");
        longer = m;
        std::printf(" | top %d size %zu\n", longer.top().v, longer.size());
        S shorter;
        shorter.emplace(8);
        std::printf(" | assign-from-longer\n");
        shorter = m;
        std::printf(" | top %d size %zu\n", shorter.top().v, shorter.size());
        S same = m;
        std::printf(" | assign-same\n");
        same = m;
        std::printf(" | top %d\n", same.top().v);
        same = same;
        std::printf("self-assign size %zu top %d\n", same.size(), same.top().v);
        S blank;
        std::printf("assign-empty");
        same = blank;
        std::printf(" size %zu\n", same.size());
        std::printf("assign-onto-empty");
        blank = m;
        std::printf(" top %d size %zu\n", blank.top().v, blank.size());
        S sw;
        sw.emplace(9);
        std::printf(" | swap\n");
        sw.swap(m);
        std::printf("m %d sw %d\n", m.top().v, sw.top().v);
        m.swap(m);
        std::printf("self-swap %d %zu\n", m.top().v, m.size());
        std::swap(m, sw);
        std::printf("std-swap m %d sw %d\n", m.top().v, sw.top().v);
        std::printf("eq %d lt %d gt %d\n", int(longer == blank), int(longer < blank), int(longer > blank));
        S empty;
        std::printf("empty< %d empty== %d\n", int(empty < longer), int(empty == S{}));
        S prefix;
        prefix.emplace(1);
        std::printf("prefix< %d\n", int(prefix < longer));
        std::printf("self-move");
        longer = std::move(longer);
        std::printf(" size %zu\n", longer.size());
        S victim;
        victim.emplace(3);
        victim.emplace(4);
        std::printf(" | move-assign\n");
        victim = std::move(blank);
        std::printf("victim %d size %zu src %zu\n", victim.top().v, victim.size(), blank.size());
        std::printf("leaving\n");
    }
    std::printf("left\n");
}

static void ints() {
    std::stack<int> s;
    s.push(1);
    s.push(2);
    s.push(3);
    std::printf("ints %d %zu %d\n", s.top(), s.size(), int(s.empty()));
    int& older = s.top();
    s.push(s.top());
    std::printf("push-top %d %zu older %d\n", s.top(), s.size(), older);
    s.pop();
    std::printf("pop %d older %d\n", s.top(), older);
    s.pop();
    std::printf("pop2 %d %zu\n", s.top(), s.size());
    std::stack<int> j = s;
    std::printf("eq %d\n", int(s == j));
    j.pop();
    std::printf("ord %d %d %d %d\n", int(j < s), int(s < j), int(j > s), int(j != s));
    std::stack<int> k;
    k.push(1);
    k.push(9);
    std::printf("lex %d %d %d cmp %d\n", int(k < s), int(s < k), int(k == s), int((k <=> s) < 0));
    // Bottom differs: (1, 3) vs (2, 0). Lexicographical order is the bottom, not the top.
    std::stack<int> lo;
    lo.push(1);
    lo.push(3);
    std::stack<int> hi;
    hi.push(2);
    hi.push(0);
    std::printf("bottom %d %d\n", int(lo < hi), int(hi < lo));
    s.swap(k);
    std::printf("swap %d %d\n", s.top(), k.top());
    const std::stack<int> ck = k;
    std::printf("const %d %zu\n", ck.top(), ck.size());
    std::stack<int> live;
    live.push(10);
    int& kept = live.top();
    live.push(20);
    live.pop();
    std::printf("kept %d top %d\n", kept, live.top());
    std::stack<std::string> str;
    str.push("a");
    str.emplace(3, 'x');
    std::printf("str %zu %s %zu\n", str.size(), str.top().c_str(), str.top().size());
    std::stack<std::unique_ptr<int>> u;
    u.push(std::make_unique<int>(1));
    u.emplace(new int(2));
    auto u2 = std::move(u);
    u2.pop();
    std::printf("uniq %d %zu %zu\n", *u2.top(), u2.size(), u.size());
    std::stack<int> spec;
    std::printf("nox %d %d %d %d %d\n", int(noexcept(spec.empty())), int(noexcept(spec.size())),
                int(noexcept(spec.pop())), int(noexcept(spec.swap(spec))),
                int(noexcept(std::stack<int>(std::move(spec)))));
}

static int uaf() {
    std::stack<int> s;
    s.push(1);
    int& t = s.top();
    s.pop();
    return t;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    traced();
    ints();
    return 0;
}
