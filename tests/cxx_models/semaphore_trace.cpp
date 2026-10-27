// Trace of std::counting_semaphore (tests/cpp/test_cxx_semaphore.cpp).
// Compiled against libstdc++ and with PRISM's model header
// (src/prism/pir/models/cxx/semaphore) first on the include path.
//
// The default path is defined on both: try_acquire on an empty semaphore is
// false and does not consume a token, so a later release still leaves one
// token to take. argv "over" is the model only: release past the max
// template argument is a __prism_check, and the counter is not wrapped.
// libstdc++ counting_semaphore<N>::max() returns N, but release still adds
// into a counter whose limit is the implementation maximum, so the same
// release is not a sanitizer failure there.
#include <cstdio>
#include <cstring>
#include <semaphore>

// Native stand-in for the PIR intrinsic. A failing check is reported and
// returns, so the caller can show that the counter was not wrapped.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
    }
}

static void show() {
    std::counting_semaphore<1> s(0);
    std::printf("zero=%d\n", s.try_acquire() ? 1 : 0);
    s.release();
    std::printf("took=%d\n", s.try_acquire() ? 1 : 0);
    std::printf("empty=%d\n", s.try_acquire() ? 1 : 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "over") == 0) {
        std::counting_semaphore<1> s(1);
        s.release(1);
        const int a = s.try_acquire() ? 1 : 0;
        const int b = s.try_acquire() ? 1 : 0;
        std::printf("over a=%d b=%d\n", a, b);
        return 0;
    }
    show();
    return 0;
}
