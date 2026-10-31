// BOUNDED: std::atomic message passing with release/acquire (the <atomic>
// model passes the order through; sccp folds it).
#include <atomic>
#include <thread>
#include <cassert>
int data;
std::atomic<int> flag{0};
int main() {
    std::thread a([] { data = 42; flag.store(1, std::memory_order_release); });
    std::thread b([] { if (flag.load(std::memory_order_acquire) == 1) assert(data == 42); });
    a.join();
    b.join();
}
