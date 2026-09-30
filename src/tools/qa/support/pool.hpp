#pragma once

// ThreadPoolExecutor.map: fn over items on `jobs` threads, results in item order.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace prism::qa {

template <class T, class F>
auto parallel_map(int jobs, const std::vector<T>& items, F fn) -> std::vector<decltype(fn(items[0]))> {
    using R = decltype(fn(items[0]));
    std::vector<R> out(items.size());
    std::atomic<std::size_t> next{0};
    std::exception_ptr first_error;
    std::mutex err_mu;
    auto worker = [&] {
        for (;;) {
            std::size_t i = next.fetch_add(1);
            if (i >= items.size()) return;
            try {
                out[i] = fn(items[i]);
            } catch (...) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (!first_error) first_error = std::current_exception();
                next.store(items.size());
                return;
            }
        }
    };
    const int n = std::max(1, std::min<int>(jobs, static_cast<int>(items.size())));
    std::vector<std::thread> threads;
    for (int k = 0; k < n; ++k) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (first_error) std::rethrow_exception(first_error);
    return out;
}

}  // namespace prism::qa
