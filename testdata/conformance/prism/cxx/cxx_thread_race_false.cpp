// PRISM conformance task cxx/cxx_thread_race_false.cpp: expected false (noassert)
// the worker may run before or after the store: one schedule sees x == 0
#include <cassert>
#include <mutex>
#include <thread>
static std::mutex m;
static int x;
int cxx_thread_race_false(int c) {
    x = 0;
    std::thread t([] { std::lock_guard<std::mutex> g(m); assert(x == 1); });
    { std::lock_guard<std::mutex> g(m); x = 1 + (c & 0); }
    t.join();
    return x;
}
