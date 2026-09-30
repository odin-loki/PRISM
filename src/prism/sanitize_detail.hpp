#pragma once

// Internal to prism_core: the sanitize stage's probes and run mapping
// (src/prism/adapters.cpp), declared for the doctests.

#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace prism::sanitize_detail {

// MinGW / mingw-w64 by path or -dumpmachine triple.
bool is_mingw_cc(const std::string& cc);
// -print-file-name resolves a real sanitizer runtime (an echo of the name is not one).
bool has_runtime_lib(const std::string& cc, const std::vector<std::string>& flags);
// The compiler builds and fires the sanitizer (accepting the flag is not support).
bool probe(const std::string& cc, const std::vector<std::string>& flags);
// Run output is a sanitizer report (and not an unusable-runtime message).
bool hit(const std::string& text, int rc);
bool runtime_unusable(const std::string& text);
// (status, message, evidence) of building `source` with `flags` and calling `call`.
std::tuple<std::string, std::string, std::string> compile_and_run(const std::string& cc,
                                                                  const std::filesystem::path& source,
                                                                  const std::vector<std::string>& flags,
                                                                  double timeout,
                                                                  const std::optional<std::string>& call);

}  // namespace prism::sanitize_detail
