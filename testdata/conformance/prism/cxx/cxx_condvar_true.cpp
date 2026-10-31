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
static int seed;  // the input, read by the thread (a captureless lambda: the conc stage encodes no thread arguments)
int cxx_condvar_true(int c) {
    ready = false; data = 0; seed = c;
    std::thread t([] { std::lock_guard<std::mutex> g(m); data = 42 + (seed & 0); ready = true; cv.notify_one(); });
    { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return ready; }); assert(data == 42); }
    t.join();
    return data;
}
