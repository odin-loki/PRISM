// Solver library plumbing: SHA-256, process runner, tool lookup, LRAT checking.

#include "prism/solver.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace prism::solver {

std::string_view kind_name(SolveResult::Kind k) {
    switch (k) {
        case SolveResult::Sat: return "sat";
        case SolveResult::Unsat: return "unsat";
        case SolveResult::Unknown: return "unknown";
        case SolveResult::Timeout: return "timeout";
        case SolveResult::Error: return "error";
    }
    return "unknown";
}

std::string_view verdict_status(const SolveResult& r) {
    switch (r.kind) {
        case SolveResult::Unsat: return r.certified ? laws::PROVED_CERTIFIED : laws::PROVED;
        case SolveResult::Sat: return laws::FAILED;
        case SolveResult::Timeout: return laws::TIMEOUT;
        case SolveResult::Error: return laws::ERROR;
        case SolveResult::Unknown: break;
    }
    return laws::UNKNOWN;
}

// ---------------------------------------------------------------- SHA-256
namespace {

struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<unsigned char, 64> buf{};
    std::size_t used = 0;
    std::uint64_t total = 0;

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
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                      hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const unsigned char* p, std::size_t n) {
        total += n;
        while (n > 0) {
            std::size_t take = std::min(n, 64 - used);
            std::memcpy(buf.data() + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf.data()); used = 0; }
        }
    }

    std::string hex() {
        std::uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (used != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        std::string out;
        char tmp[9];
        for (auto v : h) { std::snprintf(tmp, sizeof tmp, "%08x", v); out += tmp; }
        return out;
    }
};

}  // namespace

std::string sha256_hex(std::string_view data) {
    Sha256 s;
    s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return s.hex();
}

std::string sha256_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    std::array<char, 1 << 16> b{};
    while (in) {
        in.read(b.data(), b.size());
        auto n = in.gcount();
        if (n > 0) s.update(reinterpret_cast<const unsigned char*>(b.data()), std::size_t(n));
    }
    return s.hex();
}

// ---------------------------------------------------------------- helpers
namespace detail {

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
#ifdef _WIN32
    if (const char* h = std::getenv("USERPROFILE"); h && *h) return h;
#endif
    return ".";
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, std::string_view data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    // Write-then-rename so a concurrent reader never sees half a file.
    auto tmp = p;
    tmp += ".tmp." + std::to_string(static_cast<unsigned long long>(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

#if defined(__linux__)
namespace {
// Resident set size of a process in bytes (0 when unreadable).
std::uint64_t rss_of(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/statm", static_cast<int>(pid));
    std::FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    unsigned long long size = 0, res = 0;
    const int n = std::fscanf(f, "%llu %llu", &size, &res);
    std::fclose(f);
    if (n != 2) return 0;
    static const long page = sysconf(_SC_PAGESIZE);
    return static_cast<std::uint64_t>(res) * static_cast<std::uint64_t>(page > 0 ? page : 4096);
}
}  // namespace
#endif

Proc run(const std::vector<std::string>& argv, double timeout_s, const std::atomic<bool>* stop,
         std::size_t cap, const std::string& stdin_path, const std::string& stdout_path, std::uint64_t mem_cap) {
    Proc r;
    r.mem_cap = mem_cap;
    const double t0 = now_s();
#ifdef _WIN32
    (void)argv; (void)timeout_s; (void)stop; (void)cap; (void)stdin_path; (void)stdout_path; (void)mem_cap;
    r.failed = true;
    r.out = "process runner not implemented on Windows";
    return r;
#else
    if (argv.empty()) { r.failed = true; return r; }
    int fds[2];
    // O_CLOEXEC on both ends: a sibling thread spawning at the same moment
    // must not inherit our write end (that would hold EOF back).
    if (pipe2(fds, O_CLOEXEC) != 0) { r.failed = true; return r; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, stdin_path.empty() ? "/dev/null" : stdin_path.c_str(),
                                     O_RDONLY, 0);
    if (stdout_path.empty())
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, args[0], &fa, &at, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        r.failed = true;
        r.out = std::string("cannot start ") + argv[0] + ": " + std::strerror(rc);
        return r;
    }
    bool open = true;
    char b[65536];
    auto kill_group = [&] { ::kill(-pid, SIGKILL); ::kill(pid, SIGKILL); };
    double next_mem = 0;
    // true when the child passed the memory cap (it is then killed)
    auto over_mem = [&]() -> bool {
#if defined(__linux__)
        if (mem_cap == 0 || r.mem_exceeded) return false;
        const double t = now_s();
        if (t < next_mem) return false;
        next_mem = t + 0.1;
        const auto rss = rss_of(pid);
        r.peak_rss = std::max(r.peak_rss, rss);
        if (rss <= mem_cap) return false;
        r.mem_exceeded = true;
        kill_group();
        return true;
#else
        return false;
#endif
    };
    while (open) {
        if (stop && stop->load()) { r.cancelled = true; kill_group(); break; }
        if (timeout_s > 0 && now_s() - t0 > timeout_s) { r.timed_out = true; kill_group(); break; }
        if (over_mem()) break;
        pollfd pf{fds[0], POLLIN, 0};
        int pr = poll(&pf, 1, 20);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        ssize_t n = read(fds[0], b, sizeof b);
        if (n <= 0) { open = false; break; }
        if (r.out.size() < cap) r.out.append(b, std::size_t(n));
    }
    int st = 0;
    // The child may have closed stdout but still be running: wait with the
    // same deadline / stop rules.
    for (;;) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) break;
        if (stop && stop->load() && !r.cancelled) { r.cancelled = true; kill_group(); }
        if (timeout_s > 0 && now_s() - t0 > timeout_s && !r.timed_out && !r.cancelled) {
            r.timed_out = true;
            kill_group();
        }
        if (!r.timed_out && !r.cancelled) over_mem();
        usleep(2000);
    }
    close(fds[0]);
    if (WIFEXITED(st)) r.rc = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) r.rc = 128 + WTERMSIG(st);
    r.secs = now_s() - t0;
    return r;
#endif
}

std::optional<std::string> out_of_memory(const Proc& p) {
    if (p.mem_exceeded)
        return "ran out of memory (resident memory passed the cap of " + std::to_string(p.mem_cap >> 20) +
               " MB; PRISM_CHECKER_MEM raises it)";
    if (p.out.find("heap space exhausted") != std::string::npos) return "ran out of memory (CakeML heap exhausted)";
    if (p.out.find("stack space exhausted") != std::string::npos) return "ran out of memory (CakeML stack exhausted)";
    if (p.out.find("std::bad_alloc") != std::string::npos || p.out.find("out of memory") != std::string::npos ||
        p.out.find("Out of memory") != std::string::npos)
        return "ran out of memory";
    if (!p.timed_out && !p.cancelled && p.rc == 128 + 9) return "was killed (SIGKILL; out of memory?)";
    return std::nullopt;
}

}  // namespace detail

std::string default_cache_dir() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
        return (fs::path(x) / "prism" / "solver").string();
    return (fs::path(detail::home_dir()) / ".cache" / "prism" / "solver").string();
}

// ---------------------------------------------------------------- tools
namespace {

// Tool directory name -> executable names inside it.
std::vector<std::pair<std::string, std::string>> tool_homes(std::string_view exe) {
    if (exe == "lrat-check" || exe == "drat-trim") return {{"drat-trim", std::string(exe)}};
    return {{std::string(exe), std::string(exe)}};
}

std::optional<ToolInfo> in_root(const fs::path& root, std::string_view exe) {
    std::error_code ec;
    std::optional<ToolInfo> best;
    fs::file_time_type best_t{};
    for (const auto& [dir, bin] : tool_homes(exe)) {
        fs::path home = root / dir;
        if (!fs::is_directory(home, ec)) continue;
        for (const auto& e : fs::directory_iterator(home, ec)) {
            if (!e.is_directory(ec)) continue;
            for (const char* sub : {"bin", "build", ""}) {
                fs::path cand = sub[0] ? e.path() / sub / bin : e.path() / bin;
                if (!fs::is_regular_file(cand, ec)) continue;
#ifndef _WIN32
                if (access(cand.c_str(), X_OK) != 0) continue;
#endif
                auto t = fs::last_write_time(cand, ec);
                if (!best || t > best_t) {
                    best = ToolInfo{std::string(exe), cand, e.path().filename().string()};
                    best_t = t;
                }
                break;
            }
        }
    }
    return best;
}

// The Lean executables of proofs/techniques (`lake build` in that directory)
// are found in its build directory when they are not installed elsewhere.
std::optional<ToolInfo> lean_build_tool(std::string_view name) {
#ifdef PRISM_LEAN_BIN_DIR
    if (name != "prism-bitblast" && name != "prism-lrat-check") return std::nullopt;
    fs::path cand = fs::path(PRISM_LEAN_BIN_DIR) / std::string(name);
    std::error_code ec;
    if (!fs::is_regular_file(cand, ec)) return std::nullopt;
#ifndef _WIN32
    if (access(cand.c_str(), X_OK) != 0) return std::nullopt;
#endif
    return ToolInfo{std::string(name), cand, "proofs/techniques"};
#else
    (void)name;
    return std::nullopt;
#endif
}

}  // namespace

std::optional<ToolInfo> find_tool(std::string_view name, const SolveOptions& opt) {
    for (const auto& d : opt.tool_dirs)
        if (auto t = in_root(d, name)) return t;
    if (!opt.search_default_tools) return std::nullopt;
    if (auto t = in_root(fs::path(detail::home_dir()) / ".prism" / "tools", name)) return t;
    const char* path = std::getenv("PATH");
    if (!path) return lean_build_tool(name);
    std::string p = path;
    std::size_t s = 0;
    while (s <= p.size()) {
        std::size_t e = p.find(':', s);
        if (e == std::string::npos) e = p.size();
        std::string dir = p.substr(s, e - s);
        s = e + 1;
        if (dir.empty()) continue;
        fs::path cand = fs::path(dir) / std::string(name);
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) {
#ifndef _WIN32
            if (access(cand.c_str(), X_OK) != 0) continue;
#endif
            return ToolInfo{std::string(name), cand, "PATH"};
        }
    }
    return lean_build_tool(name);
}

// ---------------------------------------------------------------- LRAT
std::size_t lrat_steps(const fs::path& lrat) {
    std::ifstream in(lrat);
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string id, second;
        if (!(ls >> id)) continue;
        if (id[0] == 'c') continue;
        if (ls >> second && second == "d") continue;
        ++n;
    }
    return n;
}

namespace {
bool has_line(std::string_view out, std::string_view want) {
    std::size_t s = 0;
    while (s < out.size()) {
        std::size_t e = out.find('\n', s);
        if (e == std::string_view::npos) e = out.size();
        auto line = out.substr(s, e - s);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line == want) return true;
        s = e + 1;
    }
    return false;
}
std::string tail(std::string_view s, std::size_t n = 400) {
    return std::string(s.size() > n ? s.substr(s.size() - n) : s);
}
}  // namespace

CheckOutcome check_lrat(const ToolInfo& checker, const fs::path& cnf, const fs::path& lrat,
                        double timeout_s, unsigned heap_mb) {
    CheckOutcome o;
    o.checker = checker.name;
    o.version = checker.version;
    std::vector<std::string> argv{checker.path.string()};
    if (heap_mb > 0 && checker.name == "cake_lpr") argv.push_back("--CML_HEAP_SIZE=" + std::to_string(heap_mb));
    argv.push_back(cnf.string());
    argv.push_back(lrat.string());
    // cake_lpr caps its own heap; any other checker is capped on its
    // resident memory.
    const std::uint64_t rss_cap = checker.name == "cake_lpr" ? 0 : std::uint64_t(heap_mb) << 20;
    auto p = detail::run(argv, timeout_s, nullptr, 64u << 20, {}, {}, rss_cap);
    if (p.failed) { o.detail = p.out; return o; }
    o.ran = true;
    if (p.timed_out) { o.detail = "checker timed out"; return o; }
    if (auto m = detail::out_of_memory(p)) {
        o.detail = *m + (heap_mb > 0 && checker.name == "cake_lpr"
                             ? " (heap cap " + std::to_string(heap_mb) + " MB; PRISM_CHECKER_MEM raises it)"
                             : std::string());
        return o;
    }
    // Both checkers exit 0 on rejection; only their verdict line counts.
    if (checker.name == "cake_lpr" || checker.name == "prism-lrat-check") {
        o.verified = has_line(p.out, "s VERIFIED UNSAT");
    } else {  // drat-trim's lrat-check
        o.verified = has_line(p.out, "c VERIFIED") && p.out.find("NOT VERIFIED") == std::string::npos;
    }
    o.detail = o.verified ? "accepted" : "rejected: " + tail(p.out);
    return o;
}

}  // namespace prism::solver
