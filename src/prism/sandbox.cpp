// Law 9 exec policy + process sandbox. Twin of the Python engine prism/sandbox.py.

#include "prism/sandbox.hpp"

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "proc.hpp"

#include <forge/os/process.hpp>

#include <atomic>
#include <cctype>
#include <mutex>

#ifndef _WIN32
#  include <sys/resource.h>
#endif

namespace prism::sandbox {
namespace fs = std::filesystem;

std::string exec_message(std::string_view what) {
    return std::string(what) + ": executes code from the scanned tree; " + EXEC_INSTALL;
}

Finding exec_notrun(std::string stage, std::string_view what,
                    std::map<std::string, std::string> extra) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(laws::NOTRUN);
    f.message = exec_message(what);
    f.strength = std::string(laws::STRENGTH_FINDS);
    extra.emplace("install", EXEC_INSTALL);
    extra.emplace("reason", EXEC_REASON);
    f.extra = std::move(extra);
    return f;
}

namespace {
std::atomic<bool> g_allowed{false};
}  // namespace

bool allowed() { return g_allowed.load(); }

bool set_allowed(bool value) { return g_allowed.exchange(value); }

std::vector<std::string> bwrap_argv(const std::string& bwrap, const std::vector<std::string>& argv,
                                    const fs::path& scratch) {
    const std::string s = scratch.string();
    std::vector<std::string> out{bwrap,   "--ro-bind", "/",      "/",        "--dev",
                                 "/dev",  "--proc",    "/proc",  "--tmpfs",  "/tmp",
                                 "--bind", s,          s,        "--unshare-all",
                                 "--die-with-parent", "--"};
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

namespace {

std::optional<std::string> probe_bwrap() {
#if defined(__linux__)
    auto exe = Config{}.which({"bwrap"});
    if (!exe) return std::nullopt;
    auto r = detail::run_process(bwrap_argv(exe->string(), {"true"}, "/"), 10.0);
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
    return "none";
#else
    return "rlimits-only";
#endif
}

std::vector<std::string> wrap_argv(const std::vector<std::string>& argv, const fs::path& scratch) {
    const auto& bw = bwrap_path();
    if (!bw) return argv;
    std::error_code ec;
    auto abs = fs::weakly_canonical(fs::absolute(scratch, ec), ec);
    return bwrap_argv(*bw, argv, ec ? scratch : abs);
}

Limits limits_for(double timeout_s, bool limit_as) {
    Limits l;
    l.enabled = true;
    l.cpu_seconds = timeout_s;
    l.limit_as = limit_as;
    return l;
}

void apply_child_limits(const Limits& limits) noexcept {
#ifndef _WIN32
    if (!limits.enabled) return;
    // One second over the wall-clock timeout: the parent's timeout fires
    // first, so SIGXCPU never masquerades as a crash of the code under test.
    // forge::os::apply_limits lowers each request to an existing hard limit.
    auto cpu = static_cast<std::uint64_t>(limits.cpu_seconds + 0.999);
    if (cpu < 1) cpu = 1;
    forge::os::proc_limits l;
    l.cpu_seconds = cpu + 1;
    l.open_files = LIMIT_NOFILE;
    l.file_size_bytes = LIMIT_FSIZE_BYTES;
    l.no_core = true;
    if (limits.limit_as) l.memory_bytes = LIMIT_AS_BYTES;
    (void)forge::os::apply_limits(l);
#else
    (void)limits;
#endif
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
