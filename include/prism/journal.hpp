#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace prism {

PRISM_API void journal_reset(const std::filesystem::path& out);
PRISM_API void journal_append_stage(const std::filesystem::path& out, const StageResult& rec);
PRISM_API std::vector<StageResult> journal_read_stages(const std::filesystem::path& out);
PRISM_API bool journal_stages_present(const std::filesystem::path& out);
PRISM_API std::map<std::string, StageResult> journal_completed_ok(const std::filesystem::path& out);
PRISM_API void journal_write_functions(const std::filesystem::path& out,
                                       const std::vector<FunctionInfo>& functions);
PRISM_API std::vector<FunctionInfo> journal_read_functions(const std::filesystem::path& out);

}  // namespace prism
