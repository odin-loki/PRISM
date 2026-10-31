#include "prism/config.hpp"
#include "prism/deps.hpp"
#include "prism/manifest_pins.hpp"  // generated from third_party/MANIFEST.toml by prism-deps pins
#include "proc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifdef ERROR
#    undef ERROR
#  endif
#else
#  include <unistd.h>
#endif

namespace prism {
namespace fs = std::filesystem;

// PRISM_PBSD only. No guessed location: the ParanoidBSD tree is used when
// named explicitly (PRISM_PBSD or --pbsd PATH). Empty = not configured.
static fs::path default_pbsd() {
    if (const char* env = std::getenv("PRISM_PBSD"); env && *env) return env;
    return {};
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

#ifdef _WIN32
static std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool in_windows_apps(const fs::path& p) {
    return lower_ascii(p.generic_string()).find("/windowsapps/") != std::string::npos;
}

// A python launcher under ...\WindowsApps\ is either a real Microsoft Store
// install or the "install Python from the Store" stub, which exits 9009 and
// runs nothing. Only a probe tells them apart (both are app-execution-alias
// reparse points), so run `--version` once per path. A probe that cannot start
// or exits 9009 means "not a python": the tool is missing (Law 1), never
// accepted and then reported as a tool failure.
static bool is_store_python_stub(const fs::path& p) {
    if (!in_windows_apps(p)) return false;
    const auto stem = lower_ascii(p.stem().string());
    if (stem.rfind("python", 0) != 0) return false;
    static std::mutex mu;
    static std::map<std::string, bool> cache;
    const auto key = lower_ascii(p.string());
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    auto r = detail::run_process({p.string(), "--version"}, 10.0);
    const bool stub = r.failed || r.timed_out || r.rc == 9009;
    std::lock_guard<std::mutex> lock(mu);
    cache[key] = stub;
    return stub;
}

// Extension including the dot, lowercased. A leading-dot name (".git") has none.
static std::string win_ext_lower(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return lower_ascii(std::string(name.substr(dot)));
}

static bool win_exec_ext(std::string_view ext) {
    return ext == ".exe" || ext == ".bat" || ext == ".cmd";
}

// A directory, or a file that is not .exe/.bat/.cmd, is not a tool. The Store
// python stub is not a tool either (Law 1); the caller keeps walking.
static bool win_acceptable_tool(const fs::path& cand) {
    if (!win_exec_ext(lower_ascii(cand.extension().string()))) return false;
    std::error_code ec;
    if (!fs::is_regular_file(cand, ec) || ec) return false;
    return !is_store_python_stub(cand);
}

// SearchPath(NULL) directory order. System32 stays ahead of PATH, so the WSL
// launcher bash.exe is still found when PATH has been replaced.
static std::vector<fs::path> win_search_dirs() {
    std::vector<fs::path> dirs;
    // Win32 path cap. Heap: which() runs on worker threads, and a 32K wchar
    // buffer is 64KB on the stack.
    std::wstring buf(32768, L'\0');
    const auto cap = static_cast<DWORD>(buf.size());
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), cap);
    if (n > 0 && n < cap) {
        auto app = fs::path(buf.c_str()).parent_path();
        if (!app.empty()) dirs.push_back(std::move(app));
    }
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) cwd = ".";
    dirs.push_back(cwd);
    n = static_cast<DWORD>(GetSystemDirectoryW(buf.data(), cap));
    if (n > 0 && n < cap) dirs.emplace_back(buf.c_str());
    n = static_cast<DWORD>(GetWindowsDirectoryW(buf.data(), cap));
    if (n > 0 && n < cap) {
        fs::path windir(buf.c_str());
        dirs.push_back(windir / "System");  // 16-bit system directory
        dirs.push_back(std::move(windir));
    }
    if (const char* env = std::getenv("PATH")) {
        const std::string p(env);
        for (std::size_t i = 0; i <= p.size();) {
            auto j = p.find(';', i);
            if (j == std::string::npos) j = p.size();
            std::string dir = p.substr(i, j - i);
            i = j + 1;
            // An empty PATH entry is the current directory, as in SearchPath.
            dirs.push_back(dir.empty() ? cwd : fs::path(std::move(dir)));
        }
    }
    return dirs;
}

static std::optional<fs::path> win_find_named(const std::vector<fs::path>& dirs, const std::string& file) {
    for (const auto& dir : dirs) {
        fs::path cand = dir / file;
        if (win_acceptable_tool(cand)) return cand;
    }
    return std::nullopt;
}

// A bare name takes .exe from anywhere in the SearchPath list before .bat or
// .cmd (the old SearchPath ".exe" pass). .bat then .cmd apply only when no
// .exe exists, and an earlier directory wins between those two. A name that
// already ends in .exe/.bat/.cmd is that file only. Any other dotted name
// (tool.12) still gets .exe/.bat/.cmd appended, so tool.12.exe is found.
static std::optional<fs::path> win_which_name(const std::vector<fs::path>& dirs, std::string_view name) {
    if (win_exec_ext(win_ext_lower(name))) return win_find_named(dirs, std::string(name));
    const std::string base(name);
    if (auto hit = win_find_named(dirs, base + ".exe")) return hit;
    for (const auto& dir : dirs) {
        for (const char* suffix : {".bat", ".cmd"}) {
            fs::path cand = dir / (base + suffix);
            if (win_acceptable_tool(cand)) return cand;
        }
    }
    return std::nullopt;
}
#endif

std::optional<fs::path> Config::which(std::initializer_list<std::string_view> names) const {
#ifdef _WIN32
    const auto dirs = win_search_dirs();
    for (auto n : names)
        if (auto hit = win_which_name(dirs, n)) return hit;
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
    // A directory, or a file without execute permission, is not the tool.
    for (auto n : names) {
        for (auto& d : dirs) {
            fs::path cand = fs::path(d.empty() ? "." : d) / std::string(n);
            std::error_code ec;
            if (fs::is_regular_file(cand, ec) && ::access(cand.c_str(), X_OK) == 0) return cand;
        }
    }
#endif
    return std::nullopt;
}

std::optional<fs::path> Config::llvm_tool(std::string_view name) const {
    std::error_code ec;
    if (auto it = tools.find(std::string(name)); it != tools.end() && fs::is_regular_file(it->second, ec))
        return it->second;
    std::vector<fs::path> dirs;
    if (!llvm_bin.empty()) dirs.push_back(llvm_bin);
    if (const char* e = std::getenv("PRISM_LLVM_BIN"); e && *e) dirs.emplace_back(e);
#ifdef PRISM_LLVM_PINNED_BIN
    dirs.emplace_back(PRISM_LLVM_PINNED_BIN);
#endif
    std::string base(name);
#ifdef _WIN32
    base += ".exe";
#endif
    for (const auto& d : dirs) {
        fs::path cand = d / base;
        if (fs::is_regular_file(cand, ec)) return cand;
    }
    return which({name});
}

// Stage -> third_party/MANIFEST.toml component. Same table as
// prism/config.py VENDOR_DIR. No CodeQL (roadmap 1.2: engine terms).
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
        {"afl-fuzz", "aflplusplus"},
        {"frama-c", "frama-c"},
        {"infer", "infer"},
        {"cbmc", "cbmc"},
        {"strix", "strix"},
        {"semgrep", "semgrep"},
        {"spatch", "coccinelle"},
        {"cadical", "cadical"},
        {"kissat", "kissat"},
        {"cake_lpr", "cake_lpr"},
        {"bitwuzla", "bitwuzla"},
    };
    for (auto& m : kMap)
        if (stage == m.stage) return m.dir;
    return nullptr;
}

std::string adapter_install(std::string_view stage) {
    if (const char* comp = vendor_dir_for(stage))
        return std::string("prism-deps tool ") + comp + " (pinned in third_party/MANIFEST.toml)";
    return std::string(stage) +
           " is a system tool, not pinned by prism-deps (see third_party/MANIFEST.toml)";
}

// The commit third_party/MANIFEST.toml pins for an external component. Baked
// in at CMake configure time (generated prism/manifest_pins.hpp), so the
// binary trusts exactly the builds its own manifest named.
std::optional<std::string> pinned_commit(std::string_view component) {
    for (const auto& pin : kManifestPins)
        if (component == pin.name) return std::string(pin.commit);
    return std::nullopt;
}

fs::path expand_user(const fs::path& p) {
    const auto s = p.string();
    if (s.empty() || s[0] != '~' || (s.size() > 1 && s[1] != '/' && s[1] != '\\')) return p;
    fs::path home;
#ifdef _WIN32
    if (const char* u = std::getenv("USERPROFILE")) home = u;
#else
    if (const char* u = std::getenv("HOME")) home = u;
#endif
    if (home.empty()) return p;
    return s.size() <= 2 ? home : home / s.substr(2);
}

fs::path tools_home() {
    if (const char* env = std::getenv("PRISM_TOOLS_DIR"); env && *env) return fs::path(env);
    fs::path home;
#ifdef _WIN32
    if (const char* u = std::getenv("USERPROFILE")) home = u;
#else
    if (const char* u = std::getenv("HOME")) home = u;
#endif
    return home / ".prism" / "tools";
}

static bool is_built_exe(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return false;
#ifdef _WIN32
    auto ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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

// <tools_home>/<component>/<pinned commit>/bin/<name> (then the dir root),
// as installed by `prism-deps tool`. Never compiles, never walks source.
// Law 9: a tools dir inside the scanned tree is refused unless --allow-exec
// (a hostile tree could plant <tree>/.prism/tools/esbmc/<commit>/bin/esbmc).
static std::optional<fs::path> find_vendored_exe(const Config& cfg, std::string_view stage,
                                                 std::initializer_list<std::string_view> names) {
    const char* comp = vendor_dir_for(stage);
    if (!comp) return std::nullopt;
    auto commit = pinned_commit(comp);
    if (!commit) return std::nullopt;
    auto home = tools_home();
    if (!cfg.allow_exec && path_within(home, cfg.root)) return std::nullopt;
    auto root = home / comp / *commit;
    for (const char* sub : {"bin", ""}) {
        fs::path base = *sub ? (root / sub) : root;
        for (auto n : names) {
            fs::path cand = base / std::string(n);
            if (is_built_exe(cand)) return cand;
#ifdef _WIN32
            std::string ns(n);
            if (ns.size() < 4 || ns.substr(ns.size() - 4) != ".exe") {
                auto with_exe = base / (ns + ".exe");
                if (is_built_exe(with_exe)) return with_exe;
            }
#endif
        }
    }
    return std::nullopt;
}

// SHA-256 (FIPS 180-4), for tool_identity of unpinned binaries. One
// implementation, shared with prism-deps (src/tools/prism_deps/sha.cpp).
std::string sha256_hex(std::string_view data) { return deps::sha256_hex(data); }

static bool is_hex40(const std::string& s) {
    if (s.size() != 40) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Which exact build produced a finding (roadmap 1.1). The manifest commit when
// exe lives under <tools_home>/<name>/<commit>/, else
// "path:<abs>;sha256:<file hash>", cached per (path, size, mtime).
// Same format as prism/config.py tool_identity.
std::string tool_identity(const fs::path& exe) {
    std::error_code ec;
    fs::path ap = fs::weakly_canonical(fs::absolute(exe, ec), ec);
    if (ec) ap = exe;
    auto home = fs::weakly_canonical(fs::absolute(tools_home(), ec), ec);
    if (!home.empty() && path_within(ap, home)) {
        auto rel = ap.lexically_relative(home);
        std::vector<std::string> parts;
        for (const auto& part : rel) parts.push_back(part.string());
        if (parts.size() >= 3 && is_hex40(parts.at(1))) return parts.at(1);
    }
    const std::string key_path = ap.string();
    std::error_code sec;
    auto size = fs::file_size(ap, sec);
    auto mtime = fs::last_write_time(ap, sec);
    if (sec) return "path:" + key_path + ";sha256:unreadable";
    const std::string key = key_path + "|" + std::to_string(size) + "|" +
                            std::to_string(mtime.time_since_epoch().count());
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    std::ifstream in(ap, std::ios::binary);
    std::string ident;
    if (!in) {
        ident = "path:" + key_path + ";sha256:unreadable";
    } else {
        deps::Sha256 s;
        std::vector<char> chunk(1 << 20);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            auto got = in.gcount();
            if (got > 0) s.update(chunk.data(), static_cast<std::size_t>(got));
        }
        ident = "path:" + key_path + ";sha256:" + s.hex();
    }
    std::lock_guard<std::mutex> lock(mu);
    cache[key] = ident;
    return ident;
}

void stamp_tool_sha(std::vector<Finding>& findings, const fs::path& exe) {
    if (exe.empty()) return;
    const auto ident = tool_identity(exe);
    for (auto& f : findings) f.extra.try_emplace("tool_sha", ident);
}

std::optional<fs::path> Config::which_adapter(std::string_view stage,
                                             std::initializer_list<std::string_view> names) const {
    auto lookup = [&](const std::string& key) -> std::optional<fs::path> {
        auto it = tools.find(key);
        if (it == tools.end()) return std::nullopt;
        auto p = expand_user(it->second);  // --tool klee=~/bin/klee
        std::error_code ec;
        if (fs::is_regular_file(p, ec) && !ec) return p;
#ifdef _WIN32
        if (!p.has_extension()) {
            const auto pe = p.string() + ".exe";
            if (fs::is_regular_file(pe, ec) && !ec) return fs::path(pe);
        }
#endif
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
