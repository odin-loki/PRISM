#pragma once

// PRISM's environment settings: FORGE's typed, declared settings
// (forge::os::settings, os/settings.hpp). Every PRISM_* variable is defined
// once here, with its default and help text, and read through .get();
// `forge::os::settings::markdown("PRISM_")` documents them all.

#include <forge/os/settings.hpp>

#include <cstdint>
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

}  // namespace prism::settings
