// PRISM conformance task cxx/cxx_condvar_false.cpp: expected false (noassert)
// a predicate no thread sets when c != 0: CXX-DEADLOCK on that schedule
#include <condition_variable>
#include <mutex>
#include <thread>
static std::mutex m;
static std::condition_variable cv;
static bool ready;
int cxx_condvar_false(int c) {
    ready = false;
    std::thread t([c] { std::lock_guard<std::mutex> g(m); if (!c) ready = true; cv.notify_one(); });
    { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return ready; }); }
    t.join();
    return 0;
}
