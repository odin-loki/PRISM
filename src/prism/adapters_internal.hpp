#pragma once

// Internal to prism_core: the pieces of the optional-tool adapters
// (src/prism/adapters.cpp) that tests/cpp/test_adapters.cpp drives directly.
// run_optional_tools is the public entry (prism/stages.hpp).

#include "prism/config.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace prism::adapters_detail {

// Throws std::runtime_error on --no-*-check (Law 8); every adapter command
// line goes through it.
void refuse_disabled_checks(const std::vector<std::string>& cmd);
// doctest / Catch2 / "unknown option" answering as a verifier.
bool is_fake_adapter(const std::string& text);
// is_fake_adapter, shell 126/127, "not found" / "cannot execute".
bool tool_unusable(const std::string& text, int rc);
// A --help probe answered by something that is not the tool.
bool probe_looks_missing(const std::string& text, int rc);
// The bundled .cocci rules (prism/cocci next to the cwd, the scanned root or
// the binary; share/prism/cocci) plus any in the source roots.
std::vector<std::filesystem::path> cocci_rules(const std::vector<std::filesystem::path>& paths,
                                               const Config& cfg);
bool cocci_has_script(const std::filesystem::path& rule);
// First '{' .. last '}'; "{}" for empty or whitespace-only output.
std::string extract_json_object(const std::string& text);
std::vector<Finding> run_cbmc(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                              const Config& cfg);
std::vector<Finding> run_spatch(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                                const Config& cfg);
std::vector<Finding> run_semgrep(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                                 const Config& cfg);
std::vector<Finding> run_clang_tidy(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                                    const Config& cfg);
std::vector<Finding> run_klee(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                              const Config& cfg);
std::vector<Finding> run_frama_c(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                                 const Config& cfg);
std::vector<Finding> run_strix(const std::string& exe, const std::vector<std::filesystem::path>& paths,
                               const Config& cfg);

}  // namespace prism::adapters_detail

// libfuzzer_probe is implemented in adapters.cpp (anonymous namespace + wrapper).
namespace prism {
Finding libfuzzer_probe(const Config& cfg);
}  // namespace prism
