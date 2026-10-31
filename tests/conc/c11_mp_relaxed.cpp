// FAILED CONC-DATA-RACE: as c11_mp_ra.cpp with relaxed orders.
#include <atomic>
#include <thread>
int data, seen;
std::atomic<int> flag{0};
int main() {
    std::thread a([] { data = 42; flag.store(1, std::memory_order_relaxed); });
    std::thread b([] { if (flag.load(std::memory_order_relaxed) == 1) seen = data; });
    a.join();
    b.join();
}
