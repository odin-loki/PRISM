#include "prism/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  ifdef ERROR
#    undef ERROR
#  endif
#endif

namespace prism {
namespace fs = std::filesystem;

static fs::path default_pbsd() {
    if (const char* env = std::getenv("PRISM_PBSD"); env && *env) return env;
    fs::path sibling = R"(C:\Users\odinl\OneDrive\Desktop\ParanoidBSD)";
    if (fs::exists(sibling)) return sibling;
    return fs::current_path().parent_path() / "ParanoidBSD";
}

static fs::path default_gguf() {
    if (const char* env = std::getenv("PRISM_GGUF"); env && *env) return env;
    fs::path home;
#ifdef _WIN32
    if (const char* u = std::getenv("USERPROFILE")) home = u;
#else
    if (const char* u = std::getenv("HOME")) home = u;
#endif
    return home / ".ollama" / "models" / "blobs" /
           "sha256-dec52a44569a2a25341c4e4d3fee25846eed4f6f0b936278e3a3c900bb99d37c";
}

static std::string env_or(const char* name, const char* def) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return def;
}

Config default_config() {
    Config c;
    c.pbsd_root = default_pbsd();
    c.gguf = default_gguf();
    c.model = env_or("PRISM_MODEL", "qwen3.5:9b");
    c.ollama_host = env_or("OLLAMA_HOST", "http://127.0.0.1:11434");
    c.llama_server = env_or("PRISM_LLAMA_SERVER", "http://127.0.0.1:8080");
    unsigned n = std::max(2u, std::thread::hardware_concurrency());
    c.jobs = static_cast<int>(std::max(1u, n / 2));
    return c;
}

bool Config::want(std::string_view name) const {
    for (auto& s : skip)
        if (s == name) return false;
    if (!stages) return true;
    for (auto& s : *stages)
        if (s == name) return true;
    return false;
}

std::optional<fs::path> Config::which(std::initializer_list<std::string_view> names) const {
#ifdef _WIN32
    char buf[MAX_PATH];
    for (auto n : names) {
        std::string name(n);
        if (SearchPathA(nullptr, name.c_str(), ".exe", MAX_PATH, buf, nullptr))
            return fs::path(buf);
        if (SearchPathA(nullptr, name.c_str(), nullptr, MAX_PATH, buf, nullptr))
            return fs::path(buf);
    }
#else
    auto path = std::getenv("PATH");
    if (!path) return std::nullopt;
    std::string p(path);
    std::vector<std::string> dirs;
    std::size_t i = 0;
    while (i < p.size()) {
        auto j = p.find(':', i);
        if (j == std::string::npos) j = p.size();
        dirs.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    for (auto n : names) {
        for (auto& d : dirs) {
            fs::path cand = fs::path(d) / std::string(n);
            if (fs::exists(cand)) return cand;
        }
    }
#endif
    return std::nullopt;
}

static const char* vendor_dir_for(std::string_view stage) {
    struct Map {
        const char* stage;
        const char* dir;
    };
    static const Map kMap[] = {
        {"esbmc", "esbmc"},
        {"dafny", "dafny"},
        {"cppcheck", "cppcheck"},
        {"klee", "klee"},
        {"afl-fuzz", "AFLplusplus"},
        {"frama-c", "Frama-C"},
        {"infer", "infer"},
        {"codeql", "codeql"},
        {"cbmc", "cbmc"},
        {"strix", "strix"},
        {"semgrep", "semgrep"},
        {"spatch", "coccinelle"},
        {"fuse", "FuSeBMC"},
        {"fuzz4all", "Fuzz4All"},
    };
    for (auto& m : kMap)
        if (stage == m.stage) return m.dir;
    return nullptr;
}

std::string adapter_install(std::string_view stage) {
    if (const char* dir = vendor_dir_for(stage))
        return std::string("build from vendored third_party/") + dir +
               " (see third_party/SOURCES.md)";
    return std::string(stage) + " is not vendored (see third_party/SOURCES.md)";
}

static bool skip_vendor_part(const fs::path& p) {
    for (auto& part : p) {
        auto s = part.string();
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "scripts" || s == "tests" || s == "docs" || s == "examples" ||
            s == "regression" || s == "website" || s == "ql" || s == "misc" ||
            s == "javascript" || s == "change-notes")
            return true;
    }
    return false;
}

static bool is_built_exe(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return false;
    auto ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" ||
        ext == ".hpp" || ext == ".py" || ext == ".md" || ext == ".txt" || ext == ".json" ||
        ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib" || ext == ".so" ||
        ext == ".dll" || ext == ".sh" || ext == ".bat")
        return false;
    if (skip_vendor_part(p)) return false;
#ifdef _WIN32
    return ext == ".exe";
#else
    auto st = fs::status(p, ec);
    if (ec) return false;
    return (st.permissions() & fs::perms::owner_exec) != fs::perms::none;
#endif
}

// True when p is root or lies below it (both made canonical).
bool path_within(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    auto cp = fs::weakly_canonical(fs::absolute(p, ec), ec);
    auto cr = fs::weakly_canonical(fs::absolute(root, ec), ec);
    auto it = cp.begin();
    for (auto rt = cr.begin(); rt != cr.end(); ++rt, ++it) {
        if (rt->empty()) continue;  // trailing separator
        if (it == cp.end() || *it != *rt) return false;
    }
    return true;
}

static fs::path self_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#else
    std::error_code ec;
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe.parent_path();
#endif
    return {};
}

// The PRISM checkout that holds third_party/SOURCES.md: searched from the
// prism executable's directory, then the cwd. Law 9: a root inside the
// scanned tree is refused (a hostile tree could plant third_party/SOURCES.md
// and third_party/esbmc/bin/esbmc) unless the user passed --allow-exec.
static fs::path find_repo_root(const Config& cfg) {
    std::vector<fs::path> starts{self_dir(), fs::current_path()};
    for (auto p : starts) {
        for (int i = 0; i < 8 && !p.empty(); ++i) {
            std::error_code ec;
            if (fs::exists(p / "third_party" / "SOURCES.md", ec)) {
                if (cfg.allow_exec || !path_within(p, cfg.root)) return p;
                break;
            }
            auto parent = p.parent_path();
            if (parent == p) break;
            p = std::move(parent);
        }
    }
    return {};
}

static std::optional<fs::path> find_vendored_exe(const Config& cfg, std::string_view stage,
                                                 std::initializer_list<std::string_view> names) {
    const char* vendor = vendor_dir_for(stage);
    if (!vendor) return std::nullopt;
    auto repo = find_repo_root(cfg);
    if (repo.empty()) return std::nullopt;
    auto root = repo / "third_party" / vendor;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return std::nullopt;
    static const char* kSubs[] = {
        "", "bin", "build", "build/bin", "build/src", "Release", "Debug",
        "build/Release", "build/Debug",
    };
    for (auto* sub : kSubs) {
        fs::path base = *sub ? (root / sub) : root;
        for (auto n : names) {
            fs::path cand = base / std::string(n);
            if (is_built_exe(cand)) return cand;
            std::string ns(n);
            if (ns.size() < 4 || ns.substr(ns.size() - 4) != ".exe") {
                auto with_exe = base / (ns + ".exe");
                if (is_built_exe(with_exe)) return with_exe;
            }
        }
    }
    std::vector<std::string> want;
    for (auto n : names) {
        std::string s(n);
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        want.push_back(s);
        if (s.size() < 4 || s.substr(s.size() - 4) != ".exe") want.push_back(s + ".exe");
    }
    struct Node {
        fs::path p;
        int depth;
    };
    std::vector<Node> stack{{root, 0}};
    int visited = 0;
    while (!stack.empty()) {
        auto cur = std::move(stack.back());
        stack.pop_back();
        std::error_code it_ec;
        fs::directory_iterator it(cur.p, it_ec);
        if (it_ec) continue;
        const fs::directory_iterator end;
        for (; it != end; it.increment(it_ec)) {
            if (it_ec) break;
            if (++visited > 4000) return std::nullopt;
            auto name = it->path().filename().string();
            std::string low = name;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.empty() || low.front() == '.') continue;
            if (low == "scripts" || low == "tests" || low == "docs" || low == "examples" ||
                low == "regression" || low == "website" || low == "node_modules" ||
                low == "__pycache__" || low == "ql" || low == "misc" ||
                low == "javascript" || low == "change-notes" || low == "src" ||
                low == "include" || low == "lib")
                continue;
            std::error_code fec;
            if (it->is_directory(fec) && !fec) {
                if (low != "bin" && low != "build" && low != "release" && low != "debug" &&
                    low != "out" && low != "dist" && low != "install")
                    continue;
                if (cur.depth + 1 <= 3) stack.push_back({it->path(), cur.depth + 1});
                continue;
            }
            bool match = false;
            for (auto& w : want) {
                if (low == w) {
                    match = true;
                    break;
                }
            }
            if (match && is_built_exe(it->path())) return it->path();
        }
    }
    return std::nullopt;
}

std::optional<fs::path> Config::which_adapter(std::string_view stage,
                                             std::initializer_list<std::string_view> names) const {
    auto lookup = [&](const std::string& key) -> std::optional<fs::path> {
        auto it = tools.find(key);
        if (it == tools.end()) return std::nullopt;
        std::error_code ec;
        if (fs::is_regular_file(it->second, ec) && !ec) return it->second;
        return std::nullopt;
    };
    if (auto hit = lookup(std::string(stage))) return hit;
    for (auto n : names) {
        if (auto hit = lookup(std::string(n))) return hit;
    }
    if (auto v = find_vendored_exe(*this, stage, names)) return v;
    return which(names);
}

}  // namespace prism
