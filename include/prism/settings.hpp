#pragma once

// PRISM's environment settings: FORGE's typed, declared settings
// (forge::os::settings, os/settings.hpp). Every PRISM_* variable is defined
// once here, with its default and help text, and read through .get();
// `forge::os::settings::markdown("PRISM_")` documents them all.

#include <forge/os/settings.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace prism::settings {

namespace fs = forge::os::settings;

inline const auto& triage() { static auto s = fs::define("PRISM_TRIAGE", true, "write the AI triage report after a run"); return s; }
inline const auto& mem_reserve_mb() { static auto s = fs::define("PRISM_MEM_RESERVE_MB", std::int64_t{1500}, "stop work when available memory falls below this many MB; 0: off"); return s; }
inline const auto& mem_stuck_ms() { static auto s = fs::define("PRISM_MEM_STUCK_MS", std::int64_t{1000}, "watchdog: ms over budget before a run is stopped"); return s; }
inline const auto& mem_trace() { static auto s = fs::define("PRISM_MEM_TRACE", false, "trace memory-budget decisions to stderr"); return s; }
inline const auto& pbsd() { static auto s = fs::define("PRISM_PBSD", "", "PRISM data directory override"); return s; }
inline const auto& gguf() { static auto s = fs::define("PRISM_GGUF", "", "model file override"); return s; }
inline const auto& tools_dir() { static auto s = fs::define("PRISM_TOOLS_DIR", "", "directory holding PRISM's helper tools"); return s; }
inline const auto& embed_server() { static auto s = fs::define("PRISM_EMBED_SERVER", "", "embedding server URL for triage"); return s; }
inline const auto& proof_cache() { static auto s = fs::define("PRISM_PROOF_CACHE", "", "persistent proof-repair cache directory"); return s; }
inline const auto& n_gpu_layers() { static auto s = fs::define("PRISM_N_GPU_LAYERS", std::int64_t{-1}, "model layers offloaded to the GPU; -1: all"); return s; }

inline const auto& pir_lean_export() { static auto s = fs::define("PRISM_PIR_LEAN_EXPORT", "", "export PIR to Lean (path)"); return s; }
inline const auto& pir_progress() { static auto s = fs::define("PRISM_PIR_PROGRESS", "", "PIR progress-note file"); return s; }
inline const auto& pir_only() { static auto s = fs::define("PRISM_PIR_ONLY", "", "PIR: restrict to these units / skip memory headroom"); return s; }
inline const auto& pir_batch() { static auto s = fs::define("PRISM_PIR_BATCH", "", "PIR units per batch"); return s; }
inline const auto& houdini_trace() { static auto s = fs::define("PRISM_HOUDINI_TRACE", "", "trace Houdini invariant inference"); return s; }
inline const auto& z3_worker() { static auto s = fs::define("PRISM_Z3_WORKER", "", "path of the z3 worker binary"); return s; }
inline const auto& checker_mem() { static auto s = fs::define("PRISM_CHECKER_MEM", "", "proof-checker memory cap"); return s; }
inline const auto& solver_predict() { static auto s = fs::define("PRISM_SOLVER_PREDICT", "", "0: disable the solver predictor"); return s; }
inline const auto& solver_model() { static auto s = fs::define("PRISM_SOLVER_MODEL", "", "solver predictor model override"); return s; }
inline const auto& cert_cache_max() { static auto s = fs::define("PRISM_CERT_CACHE_MAX", "", "certificate cache size cap"); return s; }

inline const auto& bin() { static auto s = fs::define("PRISM_BIN", "", "prism binary used by the QA tools"); return s; }
inline const auto& function_budget() { static auto s = fs::define("PRISM_FUNCTION_BUDGET", "", "per-function solver budget (s)"); return s; }
inline const auto& conf_keep() { static auto s = fs::define("PRISM_CONF_KEEP", "", "keep QA work directories"); return s; }
inline const auto& mem_budget_mb() { static auto s = fs::define("PRISM_MEM_BUDGET_MB", "", "memory budget per run (MB)"); return s; }
inline const auto& version() { static auto s = fs::define("PRISM_VERSION", "", "version string for prism-deps packaging"); return s; }

// The raw value as getenv gives it (nullptr when unset), for callers that keep
// their own parsing. The name must be defined above (FORGE's registry lists it).
inline const char* cstr(const fs::setting<std::string>& s) { return s.is_set() ? std::getenv(s.name().c_str()) : nullptr; }

}  // namespace prism::settings
