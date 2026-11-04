// PRISM conformance task cxx/cxx_cv_wait_pred_true.cpp: expected true (no-overflow)
// the predicate already holds, so wait(l, pred) returns at once and x < 100 after it
#include <condition_variable>
struct L { void lock() {} void unlock() {} };
int cxx_cv_wait_pred_true(int x) {
    std::condition_variable_any cv;
    L l;
    if (x >= 100) x = 0;
    cv.wait(l, [&] { return x < 100; });
    return x + 1;  // pred() holds on return: x < 100, no overflow
}
