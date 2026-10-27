// Differential trace of std::array. Compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/array) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical. No heap: `oob` is operator[] at size(), which aborts in
// the _GLIBCXX_ASSERTIONS check. Iterators, comparisons, std::get, and
// std::to_array are not called.
#include <array>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>

struct E {
    int v;
    E(int x = 0) : v(x) { std::printf(" c%d", v); }
    E(const E& o) : v(o.v) { std::printf(" cc%d", v); }
    E& operator=(const E& o) {
        v = o.v;
        std::printf(" =%d", v);
        return *this;
    }
    ~E() { std::printf(" d%d", v); }
};

static void ints() {
    std::array<int, 4> a{1, 2, 3, 4};
    std::printf("max int4=%zu zero=%zu\n", a.max_size(), std::array<int, 0>{}.max_size());
    std::printf("init size=%zu empty=%d front=%d back=%d [", a.size(), int(a.empty()), a.front(), a.back());
    for (std::size_t i = 0; i < a.size(); ++i) std::printf(" %d", a[i]);
    std::printf(" ] data0=%d\n", a.data()[0]);
    const int* p = a.data();
    a.fill(7);
    std::printf("fill stable=%d [", int(a.data() == p));
    for (std::size_t i = 0; i < a.size(); ++i) std::printf(" %d", a[i]);
    std::printf(" ]\n");
    std::array<int, 4> b{8, 9, 10, 11};
    a.swap(b);
    std::printf("swap a");
    for (std::size_t i = 0; i < a.size(); ++i) std::printf(" %d", a[i]);
    std::printf(" b");
    for (std::size_t i = 0; i < b.size(); ++i) std::printf(" %d", b[i]);
    std::printf(" stable=%d b0=%d\n", int(a.data() == p), b[0]);
    std::array<int, 4> c = a;
    std::printf("copy %d %d\n", c.front(), c.back());
    bool threw = false;
    try {
        (void)a.at(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    std::printf("at threw=%d at2=%d\n", int(threw), a.at(2));
    std::array<int, 0> z{};
    z.fill(1);
    std::array<int, 0> w{};
    if (z.size() == 0) z.swap(w);
    std::printf("zero size=%zu empty=%d null=%d wempty=%d\n", z.size(), int(z.empty()), int(z.data() == nullptr),
                int(w.empty()));
    std::array d{3, 1, 4};
    std::printf("deduced %zu %d %d\n", d.size(), d.front(), d.back());
}

static void traced() {
    {
        std::array<E, 3> a{E(1), E(2), E(3)};
        std::printf("\n");
        a.fill(E(9));
        std::printf("\n");
        std::array<E, 3> b{E(4), E(5), E(6)};
        std::printf("\n");
        a.swap(b);
        std::printf("\n");
    }
    std::printf("\n");
}

static int oob() {
    std::array<int, 4> a{1, 2, 3, 4};
    return a[4];
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "oob") return oob();
    ints();
    traced();
    return 0;
}
