// Differential trace of std::deque (tests/cpp/test_cxx_deque.cpp): compiled
// once against libstdc++ and once with PRISM's model header
// (src/prism/pir/models/cxx/deque) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS. The two traces must be identical.
//
// Only operations whose constructor/destructor effects do not depend on
// libstdc++'s block map are traced on a user type. Middle insert/erase is
// checked on int (the final sequence). argv[1] == "uaf" uses an iterator
// after push_back: defined as UB; the model frees the iterator anchor so
// ASan reports heap-use-after-free. libstdc++ often does not free a block
// on that push, so the doctest runs "uaf" only against the model.
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

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
};

template <typename D>
static void show(const char* what, const D& d) {
    std::printf("%s size=%zu empty=%d [", what, d.size(), int(d.empty()));
    for (auto it = d.begin(); it != d.end(); ++it) std::printf(" %d", int((*it).v));
    std::printf(" ] rev [");
    for (auto it = d.rbegin(); it != d.rend(); ++it) std::printf(" %d", int(it->v));
    std::printf(" ]\n");
}

static void show_int(const char* what, const std::deque<int>& d) {
    std::printf("%s size=%zu empty=%d dist=%td [", what, d.size(), int(d.empty()),
                static_cast<std::ptrdiff_t>(d.end() - d.begin()));
    for (int x : d) std::printf(" %d", x);
    std::printf(" ] rev [");
    for (auto it = d.rbegin(); it != d.rend(); ++it) std::printf(" %d", *it);
    std::printf(" ]\n");
}

static void ints() {
    std::deque<int> empty;
    std::printf("max_size=%zu empty=%d\n", empty.max_size(), int(empty.empty()));
    std::deque<int> d{1, 2, 3};
    show_int("init", d);
    std::printf("idx %d %d %d front %d back %d\n", d[0], d[1], d[2], d.front(), d.back());
    int* p0 = &d[0];
    int* p2 = &d[2];
    d.push_back(4);
    d.push_front(0);
    std::printf("stable %d %d\n", *p0, *p2);
    show_int("ends", d);
    d.pop_front();
    std::printf("after pop_front %d %d\n", *p0, *p2);
    int* last = &d.back();
    d.pop_back();
    std::printf("popped last was live neighbour %d\n", *p2);
    (void)last;
    show_int("pops", d);
    d.insert(d.begin(), 7);
    d.insert(d.end(), 8);
    d.insert(d.begin() + 1, 9);
    show_int("insert", d);
    d.erase(d.begin());
    d.erase(d.end() - 1);
    d.erase(d.begin() + 1);
    show_int("erase", d);
    int extra[3] = {5, 6, 7};
    d.insert(d.end(), extra, extra + 3);
    show_int("range", d);
    d.resize(d.size() + 2);
    show_int("resize+", d);
    d.resize(2);
    show_int("resize-", d);
    d.assign(3, 4);
    show_int("assign", d);
    std::deque<int> copy = d;
    std::deque<int> moved(std::move(copy));
    std::printf("copy-move src=%zu dst=%d %d %d\n", copy.size(), moved[0], moved[1], moved[2]);
    copy.push_back(1);
    std::printf("reused %d\n", copy[0]);
    std::deque<int> other{8, 9};
    moved.swap(other);
    show_int("swapped", moved);
    show_int("other", other);
    std::printf("eq %d lt %d\n", int(moved == other), int(std::deque<int>{1, 2} < std::deque<int>{1, 3}));
    std::deque<int> odds{1, 2, 3, 4, 5};
    std::printf("erase_if %zu\n", std::erase_if(odds, [](int x) { return x % 2 == 0; }));
    show_int("odds", odds);
    odds.clear();
    std::printf("clear %zu %d\n", odds.size(), int(odds.begin() == odds.end()));
    std::deque<int> em;
    em.emplace_back(1);
    em.emplace_front(2);
    em.emplace(em.begin() + 1, 3);
    show_int("emplace", em);
    try {
        (void)em.at(em.size());
    } catch (const std::out_of_range& e) {
        std::printf("at: %s\n", e.what());
    }
    try {
        std::deque<int> big(std::deque<int>{}.max_size() + 1);
        std::printf("no length %zu\n", big.size());
    } catch (const std::length_error& e) {
        std::printf("len: %s\n", e.what());
    }
    std::deque<int> n(4, 6);
    show_int("fill", n);
    std::deque deduced{3, 1, 2};
    show_int("deduced", deduced);
}

static void traced() {
    {
        std::deque<E> d;
        d.emplace_back(1);
        d.emplace_back(2);
        d.push_front(E(3));
        std::printf("\n");
        show("built", d);
        d.pop_front();
        d.pop_back();
        std::printf("\n");
        show("pops", d);
        std::deque<E> c = d;
        show("copy", c);
        c.resize(3);
        show("resize+", c);
        c.resize(1);
        show("resize-", c);
        g_throw_in = 0;
        try {
            d.push_back(E(9));
        } catch (const std::runtime_error&) {
            std::printf(" threw");
        }
        g_throw_in = -1;
        std::printf(" size=%zu\n", d.size());
        show("after throw", d);
        d.clear();
        std::printf("cleared %zu\n", d.size());
    }
    std::printf("\nleft scope\n");
}

// Iterator used after push_back. The standard invalidates every iterator;
// references to existing elements stay valid. The model deletes the iterator
// anchor, so this is a heap use-after-free under ASan.
static int uaf() {
    std::deque<int> d;
    d.push_back(1);
    auto it = d.begin();
    d.push_back(2);
    return *it;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "uaf") return uaf();
    ints();
    traced();
    return 0;
}
