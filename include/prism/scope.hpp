#pragma once

// What is in scope: one skip-directory list for every stage. Port of the
// Python engine prism/scope.py (tests/test_polyglot.py locks the tables).
// The inventory stage records each skipped directory that holds source
// files as UNKNOWN, so nothing is skipped silently (Law 7).

#include "prism/export.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace prism::scope {

PRISM_API const std::set<std::string>& skip_dirs();
// True for a directory name no stage descends into (SKIP_DIRS or a
// prism-out* / build* prefix).
PRISM_API bool skip_dir(std::string_view name);
// True when a directory between root and p (below root only) is skipped.
PRISM_API bool skipped_path(const std::filesystem::path& p, const std::filesystem::path& root);

struct Skipped {
    std::string dir;  // relative to root, generic separators
    std::size_t files = 0;
};

// Each top-most skipped directory holding at least one source file, sorted by dir.
PRISM_API std::vector<Skipped> skipped_dirs(
    const std::filesystem::path& root,
    const std::function<bool(const std::filesystem::path&)>& is_source);

PRISM_API std::string skipped_message(const std::string& dir, std::size_t files);

}  // namespace prism::scope
