// Trace of std::shared_mutex. Include <shared_mutex>. Compiled against
// libstdc++ and against src/prism/pir/models/cxx (that directory first on
// the include path), both under ASan/UBSan, the lines should match.
// tests/cpp/test_cxx_shared_mutex.cpp checks the model text; it does not
// shell out to this file. No heap and no thread: one object, try_lock and
// try_lock_shared only. lock() and lock_shared() are not called.
#include <cstdio>
#include <cstdlib>
#include <shared_mutex>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <shared_mutex> never calls it, so a successful run prints nothing from here.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "%s: %s\n", cls, msg);
        std::abort();
    }
}

int main() {
    std::shared_mutex m;
    const int shared = m.try_lock_shared() ? 1 : 0;
    const int excl = m.try_lock() ? 1 : 0;
    std::printf("shared %d excl %d\n", shared, excl);
    m.unlock_shared();

    const int excl2 = m.try_lock() ? 1 : 0;
    const int shared2 = m.try_lock_shared() ? 1 : 0;
    std::printf("excl2 %d shared2 %d\n", excl2, shared2);
    m.unlock();
    return 0;
}
