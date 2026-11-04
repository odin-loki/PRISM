#pragma once

// One set of exit codes and one error-reporting path for every PRISM CLI
// (prism and its subcommands, prism-qa, prism-deps, prism_ai and the small
// generators). Numbers are unchanged from the documented contract
// (docs/USER_GUIDE.md section 5, docs/MSVC_PORT.md):
//   0  ok            the command ran; no finding at or above --fail-on
//   1  failed        a finding at --fail-on, a gate failed, a check said FAIL
//   2  usage         bad arguments, a missing or unreadable input
//   3  error         an internal or environment failure (tool missing, I/O)

#include "prism/log.hpp"

#include <string_view>

namespace prism {

enum class Exit : int { Ok = 0, Failed = 1, Usage = 2, Error = 3 };

constexpr int code(Exit e) noexcept { return static_cast<int>(e); }

// Report an error through prism::log and return the exit code to hand back
// from main()/a subcommand:  return prism::fail("conformance", "bad --filter regex");
inline int fail(std::string_view component, std::string_view message, Exit e = Exit::Usage) {
    log::error(component, message);
    return code(e);
}

// As fail(), plus a hint line (e.g. "(prism --help lists the options)").
inline int fail_usage(std::string_view component, std::string_view message, std::string_view hint) {
    log::error(component, message);
    if (!hint.empty()) log::plain(hint);
    return code(Exit::Usage);
}

}  // namespace prism
