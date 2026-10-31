// PRISM conformance task cxx/cxx_condvar_false.cpp: expected false (noassert)
// a predicate no thread sets when c != 0: CXX-DEADLOCK on that schedule
#include <condition_variable>
#include <mutex>
#include <thread>
static std::mutex m;
static std::condition_variable cv;
static bool ready;
static int seed;  // the input, read by the thread (captureless: the conc stage encodes no thread arguments)
int cxx_condvar_false(int c) {
    ready = false;
    seed = c;
    std::thread t([] { std::lock_guard<std::mutex> g(m); if (!seed) ready = true; cv.notify_one(); });
    { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return ready; }); }
    t.join();
    return 0;
}
