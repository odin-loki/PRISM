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

}  // namespace prism::pool
