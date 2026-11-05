// Law 9 exec policy + process sandbox. Twin of the Python engine prism/sandbox.py.

#include "prism/exec.hpp"
#include "prism/sandbox.hpp"

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/exec.hpp"
#include "prism/sandbox.hpp"

#include <forge/os/process.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <random>

#ifndef _WIN32
#include <unistd.h>  // mkdtemp (glibc also declares it in <cstdlib>)
#endif

namespace prism::sandbox {
namespace fs = std::filesystem;

namespace {
std::atomic<bool> g_allowed{false};
std::atomic<bool> g_held{false};
}  // namespace

bool held_no_sandbox() { return g_held.load(); }

bool set_held_no_sandbox(bool value) { return g_held.exchange(value); }

std::string exec_message(std::string_view what) {
    return std::string(what) + ": executes code from the scanned tree; " +
           (held_no_sandbox() ? NO_SANDBOX_INSTALL : EXEC_INSTALL);
}

Finding exec_notrun(std::string stage, std::string_view what,
                    std::map<std::string, std::string> extra) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(laws::NOTRUN);
    f.message = exec_message(what);
    f.strength = std::string(laws::STRENGTH_FINDS);
    extra.emplace("install", held_no_sandbox() ? NO_SANDBOX_INSTALL : EXEC_INSTALL);
    extra.emplace("reason", EXEC_REASON);
    f.extra = std::move(extra);
    return f;
}

bool allowed() { return g_allowed.load(); }

bool set_allowed(bool value) { return g_allowed.exchange(value); }

std::vector<std::string> bwrap_argv(const std::string& bwrap, const std::vector<std::string>& argv,
                                    const fs::path& scratch, const std::vector<std::string>& ro_mounts) {
    const std::string s = scratch.string();
    std::vector<std::string> out{bwrap};
    out.insert(out.end(), ro_mounts.begin(), ro_mounts.end());
    out.insert(out.end(), {"--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp", "--bind", s, s,
                           "--unshare-all", "--die-with-parent", "--"});
    // The fresh /tmp hides a program that lives there (a tool unpacked or
    // built under the temp dir): keep its directory visible, read-only.
    if (!argv.empty()) {
        const fs::path exe(argv[0]);
        const auto rel = exe.lexically_relative("/tmp");
        const auto in_scratch = exe.lexically_relative(scratch);
        const bool under_tmp = exe.is_absolute() && !rel.empty() && *rel.begin() != "..";
        const bool under_s = !in_scratch.empty() && *in_scratch.begin() != "..";
        if (under_tmp && !under_s) {
            const std::string d = exe.parent_path().string();
            out.insert(out.end() - 3, {"--ro-bind", d, d});
        }
    }
    out.insert(out.end(), argv.begin(), argv.end());
    return out;
}

std::vector<std::string> bwrap_argv(const std::string& bwrap, const std::vector<std::string>& argv,
                                    const fs::path& scratch) {
    return bwrap_argv(bwrap, argv, scratch, {"--ro-bind", "/", "/"});
}

namespace {

// d is "/" or holds the home directory: binding it would show ~/.ssh.
bool exposes_home(const fs::path& d) {
    if (d.empty() || d == d.root_path()) return true;
    const char* h = std::getenv("HOME");
    if (!h || !*h) return false;
    const auto rel = fs::path(h).lexically_normal().lexically_relative(d.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";  // d is home or an ancestor of it
}

// The install prefix of a program outside the system dirs (<prefix>/bin/x ->
// <prefix>, so <prefix>/lib is visible too), else its own directory.
std::optional<fs::path> prefix_of(const fs::path& exe) {
    if (!exe.is_absolute()) return std::nullopt;
    std::error_code ec;
    auto real = fs::weakly_canonical(exe, ec);
    if (ec) real = exe;
    auto dir = real.parent_path();
    if (dir.filename() == "bin" && !exposes_home(dir.parent_path())) dir = dir.parent_path();
    if (exposes_home(dir)) return std::nullopt;
    return dir;
}

}  // namespace

std::vector<std::string> jail_mounts(const std::vector<std::string>& argv) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const char* d : {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/libx32"}) {
        if (fs::is_symlink(d, ec)) {
            auto target = fs::read_symlink(d, ec);
            if (!ec) out.insert(out.end(), {"--symlink", target.string(), d});
        } else if (fs::is_directory(d, ec)) {
            out.insert(out.end(), {"--ro-bind", d, d});
        }
    }
    // Only what the loader and libc read from /etc, not the whole of it.
    for (const char* f : {"/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/alternatives",
                          "/etc/localtime", "/etc/passwd", "/etc/group", "/etc/nsswitch.conf"})
        out.insert(out.end(), {"--ro-bind-try", f, f});
    std::vector<std::string> seen;
    auto bind = [&](const fs::path& d) {
        const std::string s = d.string();
        // /usr is bound already; a /tmp dir would sit under the fresh /tmp
        // (bwrap_argv binds a program's /tmp directory after it).
        if (s == "/usr" || s.starts_with("/usr/") || s == "/tmp" || s.starts_with("/tmp/") ||
            std::ranges::find(seen, s) != seen.end())
            return;
        if (!fs::is_directory(d, ec)) return;
        seen.push_back(s);
        out.insert(out.end(), {"--ro-bind", s, s});
    };
    // The LLVM toolchain (<llvm>/bin/clang -> <llvm>) and the program's prefix.
    if (auto clang = Config{}.llvm_tool("clang"))
        if (auto p = prefix_of(*clang)) bind(*p);
    if (!argv.empty())
        if (auto p = prefix_of(argv[0])) bind(*p);
    if (auto th = tools_home(); !exposes_home(th)) bind(th);
    return out;
}

namespace {

std::optional<std::string> probe_bwrap() {
#if defined(__linux__)
    auto exe = Config{}.which({"bwrap"});
    if (!exe) return std::nullopt;
    // The probe starts the same jail a real run gets.
    std::error_code ec;
    auto scratch = fs::temp_directory_path(ec);
    if (ec) scratch = "/tmp";
    auto r = prism::exec::run_process(bwrap_argv(exe->string(), {"true"}, scratch, jail_mounts({})), 10.0);
    if (r.failed || r.timed_out || r.rc != 0) return std::nullopt;
    return exe->string();
#else
    return std::nullopt;
#endif
}

const std::optional<std::string>& bwrap_path() {
    static std::once_flag once;
    static std::optional<std::string> path;
    std::call_once(once, [] { path = probe_bwrap(); });
    return path;
}

}  // namespace

std::string kind() {
    if (bwrap_path()) return "bwrap";
#ifdef _WIN32
    // FORGE's spawn puts every child in a kill-on-close Job Object that
    // carries the memory and CPU caps (forge/os/process.hpp win_job).
    return "job-object";
#else
    return "rlimits-only";
#endif
}

bool isolates(std::string_view k) { return k == "bwrap"; }

Gate exec_gate(bool allow_exec, bool allow_unsandboxed, std::string_view k) {
    Gate g;
    if (!allow_exec) return g;
    if (isolates(k)) {
        g.run = true;
        return g;
    }
    if (allow_unsandboxed) {
        g.run = true;
        g.note = std::string(EXEC_UNSANDBOXED_FLAG) +
                 ": scanned code ran without file-system or network isolation (sandbox: " + std::string(k) +
                 ", resource limits only)";
        return g;
    }
    g.held_no_sandbox = true;
    g.note = std::string(EXEC_FLAG) + " held back: no isolating sandbox here (sandbox: " + std::string(k) +
             "), so steps that execute scanned code are NOTRUN. Install bubblewrap, or pass " +
             EXEC_UNSANDBOXED_FLAG + " to run them with resource limits only";
    return g;
}

bool jailed(const std::vector<std::string>& argv) {
    const auto& bw = bwrap_path();
    return bw && argv.size() > 1 && argv[0] == *bw;
}

int jail_exit_signal(int exit_code) { return exit_code > 128 && exit_code < 128 + 65 ? exit_code - 128 : 0; }

std::vector<std::string> wrap_argv(const std::vector<std::string>& argv, const fs::path& scratch) {
    const auto& bw = bwrap_path();
    if (!bw) return argv;
    std::error_code ec;
    auto abs = fs::weakly_canonical(fs::absolute(scratch, ec), ec);
    return bwrap_argv(*bw, argv, ec ? scratch : abs, jail_mounts(argv));
}

fs::path private_temp_dir(std::string_view prefix) {
    std::error_code ec;
    const auto base = fs::temp_directory_path(ec);
    if (ec) return {};
#ifndef _WIN32
    std::string tmpl = (base / (std::string(prefix) + "XXXXXX")).string();
    if (::mkdtemp(tmpl.data())) return fs::path(tmpl);
    return {};
#else
    std::random_device rd;
    for (int i = 0; i < 64; ++i) {
        auto cand = base / (std::string(prefix) + std::to_string(rd()) + std::to_string(rd()));
        if (fs::create_directory(cand, ec)) return cand;  // false when it exists: never shared
    }
    return {};
#endif
}

Limits limits_for(double timeout_s, bool limit_as) {
    Limits l;
    l.enabled = true;
    l.cpu_seconds = timeout_s;
    l.limit_as = limit_as;
    return l;
}

std::string quote_windows_arg(std::string_view arg, bool force) {
    if (!force && !arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos)
        return std::string(arg);
    std::string o = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 2n+1 backslashes + quote: n literal backslashes, a literal quote.
            o.append(backslashes * 2 + 1, '\\');
            o += '"';
        } else {
            o.append(backslashes, '\\');
            o += c;
        }
        backslashes = 0;
    }
    // Backslashes before the closing quote are doubled so it stays a quote.
    o.append(backslashes * 2, '\\');
    o += '"';
    return o;
}

std::string windows_command_line(const std::vector<std::string>& args) {
    std::string cl;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) cl += ' ';
        cl += quote_windows_arg(args.at(i));
    }
    return cl;
}

bool is_batch_file(std::string_view path) {
    if (path.size() < 4) return false;
    std::string ext(path.substr(path.size() - 4));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".bat" || ext == ".cmd";
}

std::optional<std::string> batch_command_line(const std::vector<std::string>& args) {
    std::string inner;
    for (std::size_t i = 0; i < args.size(); ++i) {
        // cmd.exe expands %var% and (with delayed expansion) !var! even
        // inside quotes, and a quote would end the quoted region.
        if (args.at(i).find_first_of("%!\"\r\n") != std::string::npos) return std::nullopt;
        if (i) inner += ' ';
        inner += quote_windows_arg(args.at(i), /*force=*/true);
    }
    // /s strips exactly the outer quote pair; every argument stays quoted, so
    // & | < > ^ ( ) in a file name are literal to cmd.exe.
    return "cmd.exe /d /s /c \"" + inner + "\"";
}

}  // namespace prism::sandbox
