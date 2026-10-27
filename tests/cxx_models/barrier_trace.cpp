// Trace of the partial <barrier> model (src/prism/pir/models/cxx/barrier).
// Arrives exactly the expected count. Does not call wait.
#include <barrier>
#include <cstdio>
#include <cstdlib>

// Native stand-in for the PIR intrinsic. A failing check aborts. A passing
// check returns. libstdc++ does not call it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main() {
    std::barrier b(2);
    b.arrive(2);
    std::printf("arrived\n");
    return 0;
}
