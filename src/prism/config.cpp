#include "prism/config.hpp"
#include "prism/manifest_pins.hpp"  // generated from third_party/MANIFEST.toml by CMake

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
    };
    for (auto& m : kMap)
        if (stage == m.stage) return m.dir;
    return nullptr;
}

std::string adapter_install(std::string_view stage) {
    if (const char* comp = vendor_dir_for(stage))
        return std::string("python scripts/fetch_deps.py --tool ") + comp +
               " (pinned in third_party/MANIFEST.toml)";
    return std::string(stage) +
           " is a system tool, not pinned by fetch_deps (see third_party/MANIFEST.toml)";
}

// The commit third_party/MANIFEST.toml pins for an external component. Baked
// in at CMake configure time (generated prism/manifest_pins.hpp), so the
// binary trusts exactly the builds its own manifest named.
std::optional<std::string> pinned_commit(std::string_view component) {
    for (const auto& pin : kManifestPins)
        if (component == pin.name) return std::string(pin.commit);
    return std::nullopt;
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
// as installed by scripts/fetch_deps.py. Never compiles, never walks source.
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

// ---- SHA-256 (FIPS 180-4), for tool_identity of unpinned binaries ----
namespace {
struct Sha256 {
    std::uint32_t h[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64]{};
    std::size_t used = 0;
    std::uint64_t bits = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const unsigned char* p) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            auto t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const unsigned char* p, std::size_t n) {
        bits += std::uint64_t(n) * 8;
        while (n > 0) {
            std::size_t take = std::min(n, sizeof(buf) - used);
            std::memcpy(buf + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == sizeof(buf)) {
                block(buf);
                used = 0;
            }
        }
    }

    std::string hex() {
        auto total = bits;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (used != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(total >> (56 - 8 * i));
        update(len, 8);
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (auto v : h)
            for (int s = 28; s >= 0; s -= 4) out.push_back(digits[(v >> s) & 0xf]);
        return out;
    }
};
}  // namespace

std::string sha256_hex(std::string_view data) {
    Sha256 s;
    s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return s.hex();
}

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
        if (parts.size() >= 3 && is_hex40(parts[1])) return parts[1];
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
        Sha256 s;
        std::vector<char> chunk(1 << 20);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            auto got = in.gcount();
            if (got > 0) s.update(reinterpret_cast<const unsigned char*>(chunk.data()),
                                  static_cast<std::size_t>(got));
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
