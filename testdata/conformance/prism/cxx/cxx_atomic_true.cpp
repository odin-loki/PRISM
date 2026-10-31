// PRISM conformance task cxx/cxx_atomic_true.cpp: expected true (noassert)
// two threads each fetch_add once; every interleaving ends at 2
#include <atomic>
#include <cassert>
#include <thread>
static std::atomic<int> n;
int cxx_atomic_true(int c) {
    n.store(c & 0);
    std::thread t([] { n.fetch_add(1); });
    n.fetch_add(1);
    t.join();
    assert(n.load() == 2);
    return 0;
}
