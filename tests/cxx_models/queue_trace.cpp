// Differential trace of std::queue (same shape as tests/cxx_models/list_trace.cpp):
// compiled once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/queue) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces must be identical.
// priority_queue and a non-default container argument are not named: the
// model does not declare them.
// argv[1] == "uaf" reads front() after pop. The model frees that node, so
// ASan reports heap-use-after-free. libstdc++ keeps the sequence block, so
// the same read is still undefined and ASan does not report it.
#include <cstdio>
#include <memory>
#include <queue>
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
    E(E&& o) noexcept : v(o.v) {
        std::printf(" m%d", v);
        o.v = -1;
    }
    E& operator=(const E& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    E& operator=(E&& o) noexcept {
        v = o.v;
        o.v = -1;
        std::printf(" m=%d", v);
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
};

static void ints() {
    std::queue<int> q;
    std::printf("empty=%d size=%zu\n", int(q.empty()), q.size());
    q.push(1);
    q.push(2);
    q.push(3);
    std::printf("push front=%d back=%d size=%zu empty=%d\n", q.front(), q.back(), q.size(), int(q.empty()));
    q.pop();
    std::printf("pop front=%d back=%d size=%zu\n", q.front(), q.back(), q.size());
    q.emplace(4);
    std::printf("emplace front=%d back=%d size=%zu\n", q.front(), q.back(), q.size());
    q.emplace();
    std::printf("def front=%d back=%d size=%zu\n", q.front(), q.back(), q.size());
    int& keep = q.back();
    q.push(q.front());
    std::printf("self front=%d back=%d size=%zu keep=%d\n", q.front(), q.back(), q.size(), keep);
    q.pop();
    std::printf("kept front=%d back=%d keep=%d\n", q.front(), q.back(), keep);
    std::queue<int> c(q);
    std::printf("copy front=%d back=%d size=%zu src=%zu\n", c.front(), c.back(), c.size(), q.size());
    c.pop();
    std::printf("copy-pop front=%d src-front=%d\n", c.front(), q.front());
    std::queue<int> moved(std::move(q));
    std::printf("move size=%zu src=%zu front=%d back=%d\n", moved.size(), q.size(), moved.front(), moved.back());
    q.push(8);
    q = std::move(moved);
    std::printf("move-assign size=%zu src=%zu front=%d back=%d\n", q.size(), moved.size(), q.front(), q.back());
    std::queue<int> w;
    w.push(9);
    w.swap(q);
    std::printf("swap w=%d %zu q=%d %zu\n", w.front(), w.size(), q.front(), q.size());
    std::queue<int> longer;
    longer.push(1);
    std::queue<int> src;
    src.push(3);
    src.push(4);
    longer = src;
    std::printf("assign-long front=%d back=%d size=%zu\n", longer.front(), longer.back(), longer.size());
    src.pop();
    longer = src;
    std::printf("assign-short front=%d back=%d size=%zu src=%zu\n", longer.front(), longer.back(), longer.size(),
                src.size());
    std::queue<std::unique_ptr<int>> u;
    u.push(std::make_unique<int>(1));
    u.emplace(new int(2));
    auto u2 = std::move(u);
    u2.pop();
    std::printf("unique %d %zu %zu\n", *u2.front(), u2.size(), u.size());
    struct Rec {
        int v = 0;
        std::queue<Rec> kids;
    };
    std::queue<Rec> rec;
    rec.emplace();
    rec.back().v = 1;
    rec.back().kids.emplace();
    std::printf("rec %d %zu\n", rec.front().v, rec.front().kids.size());
}

static void traced() {
    {
        std::queue<E> q;
        q.emplace(1);
        q.push(E(2));
        std::printf("\n");
        q.pop();
        std::printf("\n");
        E e(5);
        q.push(e);
        std::printf("\n");
        q.pop();
        std::printf("\n");
    }
    std::printf("\n");
    {
        std::queue<E> dst;
        dst.emplace(1);
        std::queue<E> src;
        src.emplace(2);
        src.emplace(3);
        dst = src;
        std::printf("\n");
    }
    std::printf("\n");
    {
        std::queue<E> dst;
        dst.emplace(1);
        dst.emplace(2);
        std::queue<E> src;
        src.emplace(3);
        dst = src;
        std::printf("\n");
    }
    std::printf("\n");
    {
        std::queue<E> q;
        q.emplace(1);
        E e(7);
        g_throw_in = 0;
        try {
            q.push(e);
        } catch (const std::runtime_error&) {
            std::printf(" threw");
        }
        g_throw_in = -1;
        std::printf(" size=%zu front=%d\n", q.size(), q.front().v);
    }
    std::printf("\n");
}

static int uaf() {
    std::queue<int> q;
    q.push(1);
    q.push(2);
    int& r = q.front();
    q.pop();
    return r;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    ints();
    traced();
    return 0;
}
