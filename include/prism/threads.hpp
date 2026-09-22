#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace prism {

// ISO C++ threads (std::jthread). Not POSIX pthread_* and not MinGW winpthreads.
inline int clamp_jobs(int jobs) {
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    // Python engine: --jobs 0 (and default) is cpu/2, never "use every core".
    if (jobs <= 0) return static_cast<int>(std::max(1u, hc / 2));
    return std::max(1, std::min(jobs, static_cast<int>(hc)));
}

template <class T, class F>
void parallel_for(int jobs, const std::vector<T>& items, F&& fn) {
    if (items.empty()) return;
    jobs = clamp_jobs(jobs);
    if (jobs == 1 || items.size() == 1) {
        for (std::size_t i = 0; i < items.size(); ++i) fn(i, items[i]);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(jobs));
    for (int w = 0; w < jobs; ++w) {
        workers.emplace_back([&] {
            for (;;) {
                auto i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= items.size()) break;
                fn(i, items[i]);
            }
        });
    }
}

}  // namespace prism
