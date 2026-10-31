#pragma once

// PRISM's thread pool: a thin interface over FORGE's forge::task::pool
// (support/task/pool.hpp). One process-wide pool sized by prism::clamp_jobs;
// a call with an explicit jobs count runs on a pool of that size.

#include "prism/export.hpp"

#include <cstddef>
#include <functional>

namespace prism::pool {

using RangeFn = std::function<void(std::size_t begin, std::size_t end)>;

PRISM_API void parallel_for(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn);
PRISM_API unsigned size();

// Per-index convenience on top of parallel_for.
inline void for_each_index(int jobs, std::size_t n, const std::function<void(std::size_t)>& fn) {
    parallel_for(jobs, n, 1, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) fn(i);
    });
}

}  // namespace prism::pool
