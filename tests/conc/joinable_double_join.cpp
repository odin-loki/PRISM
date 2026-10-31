// FAILED: second join() on a thread that is no longer joinable (std::system_error).
#include <thread>
int main() {
    std::thread t([] {});
    if (t.joinable()) t.join();
    t.join();
    return 0;
}
