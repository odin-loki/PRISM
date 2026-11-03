#pragma once

// PRISM's timing: FORGE's monotonic stopwatch and deadline
// (forge::os::stopwatch / forge::os::deadline, os/time.hpp). Every elapsed-time
// measurement and timeout in PRISM goes through these two types.

#include <forge/os/time.hpp>

#include <chrono>
#include <cstdint>

namespace prism::clock {

using Stopwatch = forge::os::stopwatch;
using Deadline = forge::os::deadline;

// Deadline after a duration in seconds (<= 0: never).
inline Deadline after_s(double seconds) {
    return seconds > 0 ? Deadline::after_ns(static_cast<std::uint64_t>(seconds * 1e9)) : Deadline::never();
}

// Monotonic nanoseconds (unique-ish stamps, rate sampling).
inline std::uint64_t now_ns() { return forge::os::monotonic_ns(); }

}  // namespace prism::clock
