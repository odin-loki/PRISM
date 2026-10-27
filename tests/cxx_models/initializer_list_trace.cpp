// Differential trace of std::initializer_list
// (tests/cpp/test_cxx_initializer_list.cpp): compiled once against libstdc++
// and once with PRISM's model header (src/prism/pir/models/cxx/initializer_list)
// first on the include path. The model is a non-owning view. Destroying a
// copy must not destroy or free the compiler-owned array.
#include <cstdio>
#include <initializer_list>
#include <new>

struct E {
    int v;
    static int dtors;
    static int frees;
    E(int x) : v(x) {}
    E(const E& o) : v(o.v) {}
    ~E() { ++dtors; }
    static void operator delete(void* p) noexcept {
        ++frees;
        ::operator delete(p);
    }
    static void operator delete(void* p, std::size_t) noexcept {
        ++frees;
        ::operator delete(p);
    }
    static void operator delete[](void* p) noexcept {
        ++frees;
        ::operator delete[](p);
    }
    static void operator delete[](void* p, std::size_t) noexcept {
        ++frees;
        ::operator delete[](p);
    }
};

int E::dtors = 0;
int E::frees = 0;

static int walk(std::initializer_list<int> xs) {
    int s = 0;
    auto it = xs.begin();
    for (decltype(xs.size()) i = 0; i < xs.size(); ++i) {
        s += *it;
        ++it;
    }
    return s;
}

int main() {
    std::initializer_list<int> empty;
    int hits = 0;
    auto eit = empty.begin();
    for (decltype(empty.size()) i = 0; i < empty.size(); ++i) {
        hits += *eit;
        ++eit;
    }
    std::printf("empty size=%zu eq=%d hits=%d\n", empty.size(), int(empty.begin() == empty.end()), hits);

    E::dtors = 0;
    E::frees = 0;
    std::initializer_list<E> il{E{1}, E{2}, E{3}};
    const int dtors_at_build = E::dtors;
    const int frees_at_build = E::frees;
    {
        std::initializer_list<E> copy = il;
        (void)copy.size();
    }
    std::printf("copy_dtors=%d copy_frees=%d\n", E::dtors - dtors_at_build, E::frees - frees_at_build);
    int still = 0;
    auto it = il.begin();
    for (decltype(il.size()) i = 0; i < il.size(); ++i) {
        still += it->v;
        ++it;
    }
    std::printf("still=%d\n", still);
    std::printf("walk=%d\n", walk({10, 20, 30}));
    std::printf("empty_walk=%d\n", walk({}));
    return 0;
}
