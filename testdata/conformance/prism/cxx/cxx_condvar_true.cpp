// PRISM conformance task cxx/cxx_condvar_true.cpp: expected true (noassert)
// producer/consumer with a predicate wait: every interleaving sees data == 42
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
static std::mutex m;
static std::condition_variable cv;
static bool ready;
static int data;
int cxx_condvar_true(int c) {
    ready = false; data = 0;
    std::thread t([c] { std::lock_guard<std::mutex> g(m); data = 42 + (c & 0); ready = true; cv.notify_one(); });
    { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return ready; }); assert(data == 42); }
    t.join();
    return data;
}
