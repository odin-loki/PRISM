// PRISM conformance task cxx/cxx_cv_wait_for_true.cpp: expected true (no-overflow)
// only the satisfied predicate path adds: x < 100
#include <condition_variable>
struct L { void lock() {} void unlock() {} };
int cxx_cv_wait_for_true(int x) {
    std::condition_variable cv;
    L l;
    if (cv.wait_for(l, 1, [&] { return x < 100; })) return x + 1;
    return 0;  // timed out: pred() false, no arithmetic
}
