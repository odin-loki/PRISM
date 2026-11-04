// Trace of the partial <barrier> model (src/prism/pir/models/cxx/barrier).
// Arrives exactly the expected count. Does not call wait.
//
// argv "over" is arrive past the expected count. The model must
// __prism_check and must not print a wrapped count. argv "neg" is a
// negative arrive. The model must __prism_check and must not print
// an increased count. argv "zero" is arrive(0): the count stays put
// and the check does not fire.
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Native stand-in for the PIR intrinsic. A failing check aborts. A passing
// check returns. libstdc++ does not call it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "over") == 0) {
        std::barrier b(1);
        b.arrive(2);
        std::printf("wrapped\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neg") == 0) {
        std::barrier b(3);
        b.arrive(-1);
        std::printf("increased\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "zero") == 0) {
        std::barrier b(2);
        b.arrive(0);
        std::printf("held\n");
        return 0;
    }
    std::barrier b(2);
    b.arrive(2);
    std::printf("arrived\n");
    return 0;
}
