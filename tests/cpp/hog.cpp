// prism_test_hog MB SECONDS: allocates MB megabytes, touches every page so
// they are resident, then sleeps SECONDS. The certified-mode doctest uses it
// as a checker that passes the memory cap (PRISM_CHECKER_MEM) and must be
// killed and reported as out of memory, never as a verdict.
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    const long mb = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 400;
    const long secs = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 20;
    const std::size_t n = static_cast<std::size_t>(mb > 0 ? mb : 1) << 20;
    auto* p = static_cast<volatile char*>(std::malloc(n));
    if (!p) return 3;
    for (std::size_t i = 0; i < n; i += 4096) p[i] = 1;
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    std::free(const_cast<char*>(p));
    return 0;
}
