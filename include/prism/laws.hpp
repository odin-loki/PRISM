#pragma once

// Status vocabulary (string API). The lattice, merge law, admission gate
// and audit live in the pure module prism/verdict.hpp
// (src/prism/verdict/); everything here forwards to it. docs/VERDICTS.md.

#include "prism/export.hpp"

#include <string>
#include <string_view>
#include <unordered_set>

#ifdef ERROR
#  undef ERROR
#endif

namespace prism {
struct RunReport;
}

namespace prism::laws {

inline constexpr std::string_view PROVED_CERTIFIED = "PROVED-CERTIFIED";
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

// extra key/value a finding carries when its UNSAT result was checked by a
// verified checker against the exact CNF PRISM produced (roadmap 3.2).
inline constexpr std::string_view CERTIFICATE_KEY = "certificate";
inline constexpr std::string_view CERTIFICATE_CHECKED = "checked";

// Unknown status strings are "not in the vocabulary": every predicate is false.
PRISM_API bool is_proof(std::string_view status);     // PROVED-CERTIFIED/-UNBOUNDED/PROVED/-ASSUMING
PRISM_API bool is_formal(std::string_view status);    // a proof or BOUNDED
PRISM_API bool is_answered(std::string_view status);  // formal or FAILED
PRISM_API bool is_no_answer(std::string_view status);
PRISM_API bool is_status(std::string_view status);    // in the vocabulary
// Throws std::runtime_error when the merge law refuses (verdict::merge_refusal).
PRISM_API void refuse_merge(std::string_view a, std::string_view b);
// verdict::may_rewrite on strings; false for unknown strings.
PRISM_API bool may_rewrite(std::string_view from, std::string_view to);

// Final pipeline pass (both engines, prism/laws.py audit_report): every
// finding goes through verdict::audit(stage, status, certificate). A
// violation demotes the finding (to UNKNOWN, or PROVED-CERTIFIED without a
// certificate to PROVED), records extra.audit_original, and appends one
// ERROR finding "verdict audit: <stage> may not emit <status>" to that
// stage. Idempotent. Returns the number of violations.
PRISM_API int audit_report(RunReport& report);

}  // namespace prism::laws
