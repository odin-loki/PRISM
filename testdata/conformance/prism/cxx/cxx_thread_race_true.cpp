// PRISM conformance task cxx/cxx_thread_race_true.cpp: expected true (noassert)
// the store happens before the thread starts: every schedule sees x == 1
#include <cassert>
#include <mutex>
#include <thread>
static std::mutex m;
static int x;
int cxx_thread_race_true(int c) {
    x = 0;
    { std::lock_guard<std::mutex> g(m); x = 1 + (c & 0); }
    std::thread t([] { std::lock_guard<std::mutex> g(m); assert(x == 1); });
    t.join();
    return x;
}
