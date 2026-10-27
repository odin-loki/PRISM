// Trace of std::mutex. Include <mutex> and use only try_lock and unlock.
// The flag lives in the object. There is no heap and no thread.
// tests/cpp/test_cxx_mutex.cpp checks the model text; it does not shell out
// to this file.
//
// lock, timed_mutex, recursive_mutex, scoped_lock, lock_guard, and unique_lock
// are not called: the model does not declare them.
//
// argv "unlocked" calls unlock on a mutex that was never locked. The model
// must __prism_check and must not print returned.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print. A passing check returns. libstdc++ <mutex> never calls it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "unlocked") == 0) {
        std::mutex m;
        m.unlock();
        std::printf("returned\n");
        return 0;
    }

    std::mutex m;
    const int first = m.try_lock() ? 1 : 0;
    const int second = m.try_lock() ? 1 : 0;
    std::printf("first=%d second=%d\n", first, second);
    m.unlock();
    const int again = m.try_lock() ? 1 : 0;
    std::printf("after=%d\n", again);
    m.unlock();
    return 0;
}
