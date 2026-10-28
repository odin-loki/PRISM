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
// returns void. `uaf` uses the reference from top() after pop, which frees
// that node.
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
