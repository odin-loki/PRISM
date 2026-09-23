// What is in scope: one skip-directory list for every stage.
// Port of the Python engine prism/scope.py — same table, same walk.

#include "prism/scope.hpp"

#include <algorithm>
#include <system_error>

namespace prism::scope {
namespace fs = std::filesystem;

const std::set<std::string>& skip_dirs() {
    static const std::set<std::string> k = {
        ".git", "prism-out", "third_party", "build", "node_modules", "__pycache__",
        ".venv", "venv", "target", ".tox", ".mypy_cache", ".ruff_cache", ".pytest_cache",
    };
    return k;
}

bool skip_dir(std::string_view name) {
    return skip_dirs().contains(std::string(name)) || name.starts_with("prism-out") ||
           name.starts_with("build");
}

bool skipped_path(const fs::path& p, const fs::path& root) {
    auto rel = p.lexically_relative(root);
    if (rel.empty() || rel.native().starts_with(fs::path("..").native())) return false;
    auto parent = rel.parent_path();
    for (auto& part : parent)
        if (skip_dir(part.string())) return true;
    return false;
}

namespace {

std::size_t count_sources(const fs::path& d,
                          const std::function<bool(const fs::path&)>& is_source) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             d, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_symlink(e2)) {
            if (it->is_directory(e2)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(e2) && is_source(it->path())) ++n;
    }
    return n;
}

}  // namespace

std::vector<Skipped> skipped_dirs(const fs::path& root,
                                  const std::function<bool(const fs::path&)>& is_source) {
    std::vector<Skipped> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    std::vector<fs::path> stack{root};
    while (!stack.empty()) {
        auto dir = stack.back();
        stack.pop_back();
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_symlink(e2) || !it->is_directory(e2)) continue;
            if (skip_dir(it->path().filename().string())) {
                if (auto n = count_sources(it->path(), is_source))
                    out.push_back({it->path().lexically_relative(root).generic_string(), n});
            } else {
                stack.push_back(it->path());
            }
        }
        ec.clear();
    }
    std::sort(out.begin(), out.end(), [](const Skipped& a, const Skipped& b) { return a.dir < b.dir; });
    return out;
}

std::string skipped_message(const std::string& dir, std::size_t files) {
    return "skipped " + dir + "/ (" + std::to_string(files) + " source files): vendor/build directory";
}

}  // namespace prism::scope
