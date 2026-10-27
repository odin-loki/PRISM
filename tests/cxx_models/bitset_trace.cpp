// Differential trace of std::bitset (compiled once against libstdc++ and once
// with PRISM's model header src/prism/pir/models/cxx/bitset first on the
// include path), both under ASan/UBSan with _GLIBCXX_ASSERTIONS. The traces
// must be identical. operator[] at an index >= N is not called: libstdc++
// reads a padding bit, and the model treats that index as a precondition
// and does not return one.
#include <bitset>
#include <cstdio>
#include <limits>
#include <stdexcept>

static void show(const char* what, const std::bitset<8>& b) {
    std::printf("%s size=%zu count=%zu any=%d all=%d none=%d ul=%lu bits", what, b.size(), b.count(), int(b.any()),
                int(b.all()), int(b.none()), b.to_ulong());
    for (std::size_t i = 0; i < b.size(); ++i) std::printf(" %d", int(b.test(i)));
    std::printf("\n");
}

static void ops() {
    std::bitset<8> b;
    show("default", b);
    b.set(0);
    b.set(3, true);
    b.flip(1);
    show("set-flip", b);
    b.reset(0);
    b[2] = true;
    b[5] = b[3];
    show("reset-idx", b);
    b.flip(5);
    show("idx-flip", b);
    b.set();
    show("setall", b);
    b.reset();
    show("resetall", b);
    b.flip();
    show("flipall", b);
    std::bitset<8> u(0xA5ull);
    show("a5", u);
    std::printf("eq %d ne %d\n", int(u == std::bitset<8>(0xA5ull)), int(u == b));
    std::bitset<3> narrow(0xFFull);
    std::printf("narrow ul=%lu count=%zu all=%d\n", narrow.to_ulong(), narrow.count(), int(narrow.all()));
    narrow.flip();
    std::printf("narrow-flip ul=%lu count=%zu none=%d\n", narrow.to_ulong(), narrow.count(), int(narrow.none()));
    narrow.set();
    std::printf("narrow-set ul=%lu all=%d\n", narrow.to_ulong(), int(narrow.all()));
}

static void throws() {
    try {
        (void)std::bitset<4>().test(4);
    } catch (const std::out_of_range& e) {
        std::printf("test4: [%s]\n", e.what());
    }
    try {
        (void)std::bitset<0>().test(0);
    } catch (const std::out_of_range& e) {
        std::printf("test0: [%s]\n", e.what());
    }
    try {
        std::bitset<4>().set(4);
    } catch (const std::out_of_range& e) {
        std::printf("set4: [%s]\n", e.what());
    }
    try {
        std::bitset<4>().reset(9);
    } catch (const std::out_of_range& e) {
        std::printf("reset: [%s]\n", e.what());
    }
    try {
        std::bitset<4>().flip(7);
    } catch (const std::out_of_range& e) {
        std::printf("flip: [%s]\n", e.what());
    }
    std::bitset<70> wide;
    wide.set(64);
    try {
        auto v = wide.to_ulong();
        std::printf("to_ulong %lu\n", v);
    } catch (const std::overflow_error& e) {
        std::printf("ovf: [%s]\n", e.what());
    }
    try {
        auto v = wide.to_ullong();
        std::printf("to_ullong %llu\n", v);
    } catch (const std::overflow_error& e) {
        std::printf("ovfull: [%s]\n", e.what());
    }
}

static void empty() {
    std::bitset<0> z;
    std::printf("empty any %d all %d none %d count %zu size %zu ul %lu\n", int(z.any()), int(z.all()), int(z.none()),
                z.count(), z.size(), z.to_ulong());
    z.set();
    z.flip();
    z.reset();
    std::printf("empty-after any %d\n", int(z.any()));
    std::printf("ul digits %d\n", std::numeric_limits<unsigned long>::digits);
}

int main() {
    ops();
    throws();
    empty();
    return 0;
}
