#pragma once
// prism::detail::serialized_fork: fork() for every PRISM child, one thread at
// a time. Z3's scoped_timer registers a pthread_atfork prepare handler that
// takes its timer-pool locks; two --jobs workers forking at once both run it
// and livelock. The lock is FORGE's process-wide fork mutex
// (forge::os::serialized_fork), which FORGE's own forks (spawn_wait under
// prism::exec, sandbox, child::start) hold too, so PRISM's direct forks and
// FORGE's never overlap. Header-only so prism_deps_lib can use it.
#if !defined(_WIN32)
#include <forge/os/os.hpp>

namespace prism::detail {
inline int serialized_fork() { return static_cast<int>(forge::os::serialized_fork()); }
}  // namespace prism::detail
#endif
