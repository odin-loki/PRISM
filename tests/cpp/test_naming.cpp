// The project is PRISM. The retired name must not creep back outside third_party/.

#include <doctest/doctest.h>
#include "prism/deps.hpp"

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace deps = prism::deps;

fs::path repo_root() {
    const fs::path from_file = fs::path(__FILE__).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (fs::exists(from_file / "third_party" / "MANIFEST.toml", ec)) return fs::absolute(from_file);
    for (fs::path d = fs::current_path(); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "tests" / "cpp" / "test_naming.cpp", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return from_file;
}

const std::regex& retired_name() {
    // Split so this file does not match the text scan.
    static const std::regex re(std::string("hel") + "ix", std::regex::icase);
    return re;
}

bool skip_dir_name(const std::string& name) {
    static const std::set<std::string> skip = {".git", "third_party", "__pycache__", "prism-out", "node_modules"};
    if (skip.count(name)) return true;
    if (name.size() >= 5 && name.compare(0, 5, "build") == 0) return true;
    if (name.size() >= 9 && name.compare(0, 9, "prism-out") == 0) return true;
    return false;
}

bool skip_git_entry(const fs::path& root, const fs::path& p) {
    const auto rel = fs::relative(p, root);
    if (rel.empty()) return true;
    auto it = rel.begin();
    if (it != rel.end() && skip_dir_name(it->string())) return true;
    return false;
}

bool allowed_file(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    const auto a = fs::absolute(p, ec);
    if (ec) return false;
    return a == fs::absolute(root / "CLAUDE.md", ec) || a == fs::absolute(root / "AGENTS.md", ec);
}

std::vector<fs::path> tracked_files(const fs::path& root) {
    std::vector<fs::path> out;
    if (deps::which("git")) {
        auto r = deps::run_process({"git", "-C", root.string(), "ls-files", "--cached", "--others",
                                    "--exclude-standard"},
                                   root);
        if (r.code == 0) {
            std::istringstream iss(r.out);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const fs::path p = root / line;
                std::error_code ec;
                if (allowed_file(root, p) || skip_git_entry(root, p) || !fs::is_regular_file(p, ec))
                    continue;
                out.push_back(p);
            }
            return out;
        }
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_directory()) {
            if (skip_dir_name(it->path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (allowed_file(root, it->path())) continue;
        out.push_back(it->path());
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream ss;
    for (const auto& s : lines) ss << s << '\n';
    return ss.str();
}

}  // namespace

TEST_CASE("naming: no retired name in paths") {
    const auto root = repo_root();
    const auto& re = retired_name();
    std::vector<std::string> bad;
    for (const auto& p : tracked_files(root)) {
        const auto rel = fs::relative(p, root).generic_string();
        if (std::regex_search(rel, re)) bad.push_back(rel);
    }
    CHECK_MESSAGE(bad.empty(), "rename these to PRISM:\n" << join_lines(bad));
}

TEST_CASE("naming: no retired name in text") {
    const auto root = repo_root();
    const auto& re = retired_name();
    std::vector<std::string> bad;
    for (const auto& p : tracked_files(root)) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::string line;
        for (int i = 1; std::getline(in, line); ++i) {
            if (std::regex_search(line, re)) {
                std::string preview = line;
                if (preview.size() > 100) preview.resize(100);
                bad.push_back((fs::relative(p, root).generic_string() + ":" + std::to_string(i) + ": " + preview));
            }
        }
    }
    CHECK_MESSAGE(bad.empty(), "the project is PRISM; rename these:\n" << join_lines(bad));
}
