#include <condition_variable>
struct L { void lock() {} void unlock() {} };
int cxx_cv_wait_for_false(int x) {
    std::condition_variable cv;
    L l;
    if (cv.wait_for(l, 1, [&] { return x < 100; })) return x + 1;
    return x + 1;  // timed out: pred() false, x may be INT_MAX
}
