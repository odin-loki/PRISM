// Trace of std::priority_queue (tests follow tests/cxx_models/list_trace.cpp).
// libstdc++ has no <priority_queue> header; the class lives in <queue>. This
// file includes the PRISM model (src/prism/pir/models/cxx/priority_queue)
// first on the include path, under ASan/UBSan with _GLIBCXX_ASSERTIONS.
//
// Pop order of a total order is the sorted order: the top is the greatest
// element under the comparator (std::less by default). Equal keys are not
// ordered by identity. Sift swaps node pointers, so a traced element is
// move-constructed into its node once and destroyed on pop; it is not
// move-assigned along the heap. emplace constructs in the new node and
// returns void. swap exchanges heap pointers and sizes: it does not
// construct, copy, or destroy elements. `uaf` uses the reference from top()
// after pop, which frees that node.
#include <cstdio>
#include <priority_queue>
#include <string>
#include <type_traits>
#include <utility>

struct IntSeq {
    using value_type = int;
};

struct Greater {
    bool operator()(const int& a, const int& b) const { return a > b; }
};

// Same type, opposite order. swap must carry the comparator with the heap:
// pop sifts with the comparator that arrives, not the one left behind.
struct Keyed {
    bool min;
    bool operator()(const int& a, const int& b) const { return min ? a > b : a < b; }
};

struct E {
    int v;
    E(int x = 0) : v(x) { std::printf(" c%d", v); }
    E(const E& o) : v(o.v) { std::printf(" cc%d", v); }
    E(E&& o) noexcept : v(o.v) { std::printf(" m%d", v); }
    E& operator=(const E& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    E& operator=(E&& o) noexcept {
        v = o.v;
        std::printf(" =m%d", v);
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
    bool operator<(const E& o) const { return v < o.v; }
};

static void ints() {
    std::priority_queue<int> q;
    std::printf("empty=%d size=%zu\n", int(q.empty()), q.size());
    int vals[] = {3, 1, 4, 1, 5, 9, 2, 6};
    for (int v : vals) q.push(v);
    std::printf("pushed size=%zu top=%d\n", q.size(), q.top());
    std::printf("pop");
    while (!q.empty()) {
        std::printf(" %d", q.top());
        q.pop();
    }
    std::printf("\n");
    std::printf("after size=%zu empty=%d\n", q.size(), int(q.empty()));

    std::priority_queue<int, IntSeq, Greater> minq;
    for (int v : vals) minq.push(v);
    std::printf("min");
    while (!minq.empty()) {
        std::printf(" %d", minq.top());
        minq.pop();
    }
    std::printf("\n");

    std::priority_queue<int> em;
    static_assert(std::is_void_v<decltype(em.emplace(0))>);
    em.emplace(3);
    em.emplace(1);
    em.emplace(4);
    const int& kept = em.top();
    em.emplace(9);
    std::printf("emplace kept=%d top=%d size=%zu\n", kept, em.top(), em.size());
    std::printf("emplace");
    while (!em.empty()) {
        std::printf(" %d", em.top());
        em.pop();
    }
    std::printf("\n");

    std::priority_queue<int> eq;
    eq.push(2);
    eq.push(2);
    eq.push(1);
    std::printf("eq");
    while (!eq.empty()) {
        std::printf(" %d", eq.top());
        eq.pop();
    }
    std::printf("\n");

    std::priority_queue<int> src;
    src.push(4);
    src.push(7);
    std::priority_queue<int> dst = std::move(src);
    std::printf("move dst=%d srcempty=%d\n", dst.top(), int(src.empty()));
    std::priority_queue<int> cp = dst;
    std::printf("copy top=%d size=%zu\n", cp.top(), cp.size());
    cp.pop();
    std::printf("copy after pop src=%d copysize=%zu\n", dst.top(), cp.size());

    std::priority_queue<int> left;
    left.push(1);
    left.push(8);
    left.push(3);
    std::priority_queue<int> right;
    right.push(7);
    right.push(2);
    static_assert(std::is_void_v<decltype(left.swap(right))>);
    static_assert(noexcept(left.swap(right)));
    static_assert(std::is_void_v<decltype(std::swap(left, right))>);
    static_assert(noexcept(std::swap(left, right)));
    const int& swkept = left.top();
    left.swap(right);
    std::printf("swap left=%d right=%d kept=%d lsize=%zu rsize=%zu\n", left.top(), right.top(), swkept, left.size(),
                right.size());
    std::swap(left, right);
    std::printf("swap back left=%d right=%d\n", left.top(), right.top());
    left.swap(left);
    std::printf("self top=%d size=%zu\n", left.top(), left.size());

    std::priority_queue<int> hole;
    left.swap(hole);
    std::printf("with empty leftempty=%d holesize=%zu holetop=%d\n", int(left.empty()), hole.size(), hole.top());
    left.push(5);
    std::printf("repush top=%d size=%zu\n", left.top(), left.size());
    std::printf("hole");
    while (!hole.empty()) {
        std::printf(" %d", hole.top());
        hole.pop();
    }
    std::printf("\n");

    std::priority_queue<int> e1;
    std::priority_queue<int> e2;
    e1.swap(e2);
    e1.swap(e1);
    std::printf("empty swap %d %d\n", int(e1.empty()), int(e2.empty()));

    std::priority_queue<int, IntSeq, Keyed> lo(Keyed{true});
    std::priority_queue<int, IntSeq, Keyed> hi(Keyed{false});
    int keyed[] = {1, 4, 2};
    for (int v : keyed) {
        lo.push(v);
        hi.push(v);
    }
    lo.swap(hi);
    std::printf("keyed lo");
    while (!lo.empty()) {
        std::printf(" %d", lo.top());
        lo.pop();
    }
    std::printf(" hi");
    while (!hi.empty()) {
        std::printf(" %d", hi.top());
        hi.pop();
    }
    std::printf("\n");
}

static void traced() {
    {
        std::priority_queue<E> q;
        q.push(E(1));
        q.push(E(3));
        q.push(E(2));
        std::printf("\n");
        while (!q.empty()) {
            std::printf(" top%d", q.top().v);
            q.pop();
        }
        std::printf("\n");
    }
    {
        std::priority_queue<E> q;
        q.emplace(4);
        q.emplace(6);
        q.emplace(5);
        std::printf("\n");
        while (!q.empty()) {
            std::printf(" top%d", q.top().v);
            q.pop();
        }
        std::printf("\n");
    }
    {
        std::priority_queue<E> a;
        a.emplace(1);
        a.emplace(3);
        std::priority_queue<E> b;
        b.emplace(2);
        std::printf(" |");
        a.swap(b);
        a.swap(a);
        std::priority_queue<E> empty;
        b.swap(empty);
        std::printf(" |");
        std::printf(" a");
        while (!a.empty()) {
            std::printf(" top%d", a.top().v);
            a.pop();
        }
        std::printf(" hole");
        while (!empty.empty()) {
            std::printf(" top%d", empty.top().v);
            empty.pop();
        }
        std::printf("\n");
    }
    std::printf("\n");
}

static int uaf() {
    std::priority_queue<int> q;
    q.push(1);
    q.push(2);
    const int& r = q.top();
    q.pop();
    return r;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    ints();
    traced();
    return 0;
}
