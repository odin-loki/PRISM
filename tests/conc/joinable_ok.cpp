// TRUE: joinable() is true after launch, false after join; default thread is not joinable.
#include <thread>
#include <cassert>
int main() {
    std::thread d;
    assert(!d.joinable());
    std::thread t([] {});
    assert(t.joinable());
    t.join();
    assert(!t.joinable());
    return 0;
}
