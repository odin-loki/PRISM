#pragma once

#include "prism/export.hpp"

#include <string>
#include <string_view>
#include <unordered_set>

#ifdef ERROR
#  undef ERROR
#endif

namespace prism::laws {

inline constexpr std::string_view PROVED_UNBOUNDED = "PROVED-UNBOUNDED";
inline constexpr std::string_view PROVED = "PROVED";
inline constexpr std::string_view PROVED_ASSUMING = "PROVED-ASSUMING";
inline constexpr std::string_view BOUNDED = "BOUNDED";
inline constexpr std::string_view FAILED = "FAILED";
inline constexpr std::string_view UNKNOWN = "UNKNOWN";
inline constexpr std::string_view TIMEOUT = "TIMEOUT";
inline constexpr std::string_view ERROR = "ERROR";
inline constexpr std::string_view NOFUNC = "NOFUNC";
inline constexpr std::string_view NOTRUN = "NOTRUN";
inline constexpr std::string_view NEEDS_HARNESS = "NEEDS-HARNESS";

inline constexpr std::string_view CRASH = "CRASH";
inline constexpr std::string_view CLEAN = "CLEAN";
inline constexpr std::string_view NOSEED = "NOSEED";
inline constexpr std::string_view SANFAIL = "SANFAIL";

inline constexpr std::string_view HYPOTHESIS = "HYPOTHESIS";
inline constexpr std::string_view READS = "READS";

inline constexpr std::string_view STRENGTH_PROVES = "PROVES";
inline constexpr std::string_view STRENGTH_FINDS = "FINDS";
inline constexpr std::string_view STRENGTH_SOME = "SOME";
inline constexpr std::string_view STRENGTH_READS = "READS";

PRISM_API bool is_proof(std::string_view status);
PRISM_API void refuse_merge(std::string_view a, std::string_view b);
PRISM_API bool is_answered(std::string_view status);

}  // namespace prism::laws
