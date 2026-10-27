// Differential trace of std::latch. Compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/latch) first on the
// include path. wait and arrive_and_wait are not called: this header does
// not declare them.
//
// argv "over" is count_down past zero. The model must __prism_check and
// must not print a wrapped count.
#include <latch>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

static void show() {
    std::latch g(2);
    std::printf("start %d\n", static_cast<int>(g.try_wait()));
    g.count_down(1);
    std::printf("mid %d\n", static_cast<int>(g.try_wait()));
    g.count_down();
    std::printf("zero %d\n", static_cast<int>(g.try_wait()));
    std::latch z(0);
    std::printf("empty %d\n", static_cast<int>(z.try_wait()));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "over") == 0) {
        std::latch g(1);
        g.count_down(2);
        std::printf("wrapped\n");
        return 0;
    }
    show();
    return 0;
}
