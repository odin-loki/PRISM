#include "qa.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
bool executable(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#ifndef _WIN32
    return ::access(p.c_str(), X_OK) == 0;
#else
    return true;
#endif
}

fs::path self_dir() {
    std::error_code ec;
#ifdef __linux__
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe.parent_path();
#endif
    return {};
}
}  // namespace

std::optional<fs::path> find_prism(const std::string& explicit_bin, const fs::path& repo) {
    std::vector<fs::path> cands;
    if (!explicit_bin.empty()) {
        // an explicit binary is the only candidate: a wrong path is an error, not a fallback
        if (executable(explicit_bin)) return fs::absolute(explicit_bin);
        return std::nullopt;
    }
    if (const char* e = std::getenv("PRISM_BIN"); e && *e) cands.emplace_back(e);
    if (auto d = self_dir(); !d.empty()) cands.push_back(d / "prism");
    cands.push_back(repo / "build" / "prism");
    for (const auto& c : cands)
        if (executable(c)) return fs::absolute(c);
    return std::nullopt;
}

std::optional<nlohmann::json> load_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        return nlohmann::json::parse(in);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

TempDir::TempDir(const std::string& prefix) {
    auto base = fs::temp_directory_path();
    for (int i = 0; i < 128; ++i) {
        auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        fs::path cand = base / (prefix + std::to_string(ticks) + "_" + std::to_string(i));
        std::error_code ec;
        if (fs::create_directory(cand, ec)) {
            path = std::move(cand);
            return;
        }
    }
    throw std::runtime_error("could not create a temporary directory");
}

TempDir::~TempDir() {
    std::error_code ec;
    if (!path.empty()) fs::remove_all(path, ec);
}

}  // namespace prism::qa
