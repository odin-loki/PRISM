// PRISM conformance task cxx/cxx_cv_wait_pred_false.cpp: expected false (no-overflow)
// pred() only bounds x above: x * 100000000 overflows for 22 <= x < 100
#include <condition_variable>
struct L { void lock() {} void unlock() {} };
int cxx_cv_wait_pred_false(int x) {
    std::condition_variable_any cv;
    L l;
    if (x >= 100 || x < 0) x = 0;
    cv.wait(l, [&] { return x < 100; });
    return x * 100000000;
}
