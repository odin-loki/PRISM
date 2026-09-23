#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"
#include "prism/regex.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

using FuncOf = std::function<std::optional<std::string>(int)>;

void lint_add(std::vector<Finding>& out, std::string_view rel, std::optional<std::string> fn,
              int line, std::string_view cls, std::string_view msg,
              const std::vector<std::string>& lines);

// Discarded-return: regex matches a statement that throws away the result.
void lint_discarded(const std::vector<FunctionInfo>& funcs,
                    const std::vector<std::string>& lines,
                    std::string_view rel,
                    const Regex& re,
                    std::string_view cls,
                    std::string_view msg,
                    std::vector<Finding>& out);

std::vector<Finding> run_lints(const std::vector<std::filesystem::path>& paths,
                               const std::filesystem::path& root, int jobs = 0);

// Split implementations (all called from checkers_dispatch.cpp)
// `funcs` bodies have string-literal contents blanked; `raw_funcs` keep
// them for the few checkers that read literal bytes.
void checkers_core(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs,
                   const std::vector<FunctionInfo>& raw_funcs, const std::filesystem::path& path,
                   std::string_view raw_text, std::vector<Finding>& out);
void checkers_api(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out);
void checkers_cxx(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::string_view stripped,
                  std::vector<Finding>& out);

}  // namespace prism
