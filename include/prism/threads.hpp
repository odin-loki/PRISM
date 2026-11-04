#pragma once

#include "prism/pool.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace prism {

// --jobs policy; the threads themselves come from prism::pool (FORGE task pool).
inline int clamp_jobs(int jobs) {
    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    // Python engine: --jobs 0 (and default) is cpu/2, never "use every core".
    if (jobs <= 0) return static_cast<int>(std::max(1u, hc / 2));
    return std::max(1, std::min(jobs, static_cast<int>(hc)));
}

// One call per item, on prism::pool (FORGE's task pool by default).
template <class T, class F>
void parallel_for(int jobs, const std::vector<T>& items, F&& fn) {
    if (items.empty()) return;
    pool::for_each_index(jobs, items.size(), [&](std::size_t i) { fn(i, items[i]); });
}

}  // namespace prism
