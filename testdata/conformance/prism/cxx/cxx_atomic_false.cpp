// PRISM conformance task cxx/cxx_atomic_false.cpp: expected false (noassert)
// a load then a store is not one atomic step: one interleaving loses an increment
#include <atomic>
#include <cassert>
#include <thread>
static std::atomic<int> n;
int cxx_atomic_false(int c) {
    n.store(c & 0);
    std::thread t([] { int v = n.load(); n.store(v + 1); });
    int v = n.load();
    n.store(v + 1);
    t.join();
    assert(n.load() == 2);
    return 0;
}
