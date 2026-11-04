#pragma once
// prism::detail::serialized_fork: fork() for every PRISM child, one thread at
// a time. Z3's scoped_timer registers a pthread_atfork prepare handler that
// takes its timer-pool locks; two --jobs workers forking at once both run it
// and livelock. Header-only (inline, one process-wide mutex by ODR) so the
// low-level prism_deps_lib, which prism_core links, can use it too.
#if !defined(_WIN32)
#include <mutex>
#include <unistd.h>

namespace prism::detail {
inline int serialized_fork() {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    return static_cast<int>(::fork());
}
}  // namespace prism::detail
#endif
