#pragma once

// PRISM's thin thread-pool interface, built like log.hpp. Work goes through
// parallel_for(n, grain, fn(begin, end)), the same shape as FORGE's
// forge::task::pool::parallel_for (support/task/pool.hpp). The default backend
// is a forge::task::pool sized by prism::clamp_jobs (threads.hpp).

#include "prism/export.hpp"

#include <cstddef>
#include <functional>

namespace prism::pool {

using RangeFn = std::function<void(std::size_t begin, std::size_t end)>;

struct Backend {
    // Run fn over [0, n) in chunks of about `grain`, on up to `jobs` threads
    // (jobs <= 0: the backend's default). Returns when every chunk is done.
    void (*parallel_for)(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn) = nullptr;
    unsigned (*size)() = nullptr;  // worker count at jobs <= 0
};

// Replace the backend (a default-constructed Backend restores the default).
PRISM_API Backend set_backend(Backend b);

PRISM_API void parallel_for(int jobs, std::size_t n, std::size_t grain, const RangeFn& fn);
PRISM_API unsigned size();

// Per-index convenience on top of parallel_for.
inline void for_each_index(int jobs, std::size_t n, const std::function<void(std::size_t)>& fn) {
    parallel_for(jobs, n, 1, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) fn(i);
    });
}

}  // namespace prism::pool
