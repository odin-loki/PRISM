#include <condition_variable>
struct L { void lock() {} void unlock() {} };
int cxx_cv_wait_pred_true(int x) {
    std::condition_variable cv;
    L l;
    cv.wait(l, [&] { return x < 100; });
    return x + 1;  // pred() holds on return: x < 100, no overflow
}
