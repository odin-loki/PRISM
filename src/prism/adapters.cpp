#include "prism/stages.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/sandbox.hpp"
#include "proc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <io.h>
#  include <windows.h>
#  ifdef min
#    undef min
#  endif
#  ifdef max
#    undef max
#  endif
#  ifdef ERROR
#    undef ERROR
#  endif
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace prism {
namespace fs = std::filesystem;
namespace {

struct ProcResult {
    std::string text;
    int rc = -1;
    bool timed_out = false;
    bool failed = false;
};

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string ext_of(const fs::path& p) { return lower_copy(p.extension().string()); }

bool is_c_like(const fs::path& p) {
    auto e = ext_of(p);
    return e == ".c" || e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".h" || e == ".hpp";
}

bool is_tu_file(const fs::path& p) {
    auto e = ext_of(p);
    return e == ".c" || e == ".cc" || e == ".cpp" || e == ".cxx";
}

std::string trim_copy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string tail(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    return s.substr(s.size() - n);
}

#ifdef _WIN32
std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

void refuse_disabled_checks(const std::vector<std::string>& cmd) {
    for (const auto& flag : cmd) {
        if (flag.starts_with("--no-") && flag.ends_with("-check"))
            throw std::runtime_error("refusing to disable a check: " + flag);
    }
}

Finding finding(std::string stage, std::string_view status, std::string file, std::string cls,
                std::string message, std::string_view strength) {
    Finding f;
    f.stage = std::move(stage);
    f.status = std::string(status);
    f.file = std::move(file);
    f.cls = std::move(cls);
    f.message = std::move(message);
    f.strength = std::string(strength);
    return f;
}

Finding notrun(std::string stage, std::string binary, std::string how) {
    auto f = finding(std::move(stage), laws::NOTRUN, "", "",
                     binary + " not found (config, ~/.prism/tools, PATH)",
                     laws::STRENGTH_FINDS);
    f.extra["install"] = std::move(how);
    return f;
}

bool write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix) {
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
        throw std::runtime_error("could not create temp directory");
    }
    ~TempDir() {
        std::error_code ec;
        if (!path.empty()) fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// limits: rlimits applied in the forked child (Law 9 sandbox for built
// binaries; the caller wraps argv with sandbox::wrap_argv). Default: none.
ProcResult run_argv(const std::vector<std::string>& args, double timeout_s, const fs::path& cwd = {},
                    const sandbox::Limits& limits = sandbox::Limits()) {
    refuse_disabled_checks(args);
    ProcResult r;
#ifdef _WIN32
    // No cmd.exe in between (the old _popen path let a file name such as
    // `a&calc&.c` or `%PATH%.c` reach cmd.exe's parser). CreateProcessW gets
    // one command line quoted for CommandLineToArgvW; a .bat/.cmd target
    // (which Windows runs through cmd.exe anyway) is quoted for cmd.exe or
    // refused. stdout and stderr share one pipe, like the POSIX branch.
    (void)limits;  // Windows has no rlimits; the sandbox kind is "none".
    if (args.empty()) {
        r.failed = true;
        return r;
    }
    std::string cl;
    if (sandbox::is_batch_file(args[0])) {
        auto bl = sandbox::batch_command_line(args);
        if (!bl) {
            r.failed = true;
            return r;
        }
        cl = *bl;
    } else {
        cl = sandbox::windows_command_line(args);
    }
    std::wstring wcl = widen_utf8(cl);
    std::vector<wchar_t> clbuf(wcl.begin(), wcl.end());
    clbuf.push_back(L'\0');
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE out_r = nullptr;
    HANDLE out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        r.failed = true;
        return r;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in != INVALID_HANDLE_VALUE ? nul_in : nullptr;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi{};
    const std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, clbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(out_w);
    if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    if (!ok) {
        CloseHandle(out_r);
        r.failed = true;
        return r;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(0.1, timeout_s));
    char buf[4096];
    auto drain = [&]() {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(out_r, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
            DWORD got = 0;
            const DWORD want = avail < static_cast<DWORD>(sizeof buf) ? avail
                                                                      : static_cast<DWORD>(sizeof buf);
            if (!ReadFile(out_r, buf, want, &got, nullptr) || got == 0) return;
            r.text.append(buf, got);
        }
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            r.timed_out = true;
            break;
        }
    }
    drain();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.rc = r.timed_out ? -1 : static_cast<int>(code);
    CloseHandle(out_r);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    if (args.empty()) {
        r.failed = true;
        return r;
    }
    int out_p[2] = {-1, -1};
    if (::pipe(out_p) != 0) {
        r.failed = true;
        return r;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out_p[0]);
        ::close(out_p[1]);
        r.failed = true;
        return r;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);
        sandbox::apply_child_limits(limits);
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(out_p[1], STDERR_FILENO);
        ::close(out_p[0]);
        ::close(out_p[1]);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(out_p[1]);
    out_p[1] = -1;
    int flags = ::fcntl(out_p[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(out_p[0], F_SETFL, flags | O_NONBLOCK);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(0.1, timeout_s));
    char buf[4096];
    int st = 0;
    bool reaped = false;
    auto slurp = [&]() {
        for (;;) {
            const ssize_t n = ::read(out_p[0], buf, sizeof buf);
            if (n > 0) {
                r.text.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
    };
    auto kill_group = [pid]() {
        if (::killpg(pid, SIGKILL) != 0) ::kill(pid, SIGKILL);
    };
    for (;;) {
        slurp();
        pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            kill_group();
            r.timed_out = true;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            reaped = true;
            break;
        }
        if (w < 0 && errno != EINTR) {
            kill_group();
            r.timed_out = true;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            reaped = true;
            break;
        }
        const auto remain_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{};
        pfd.fd = out_p[0];
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, static_cast<int>(std::max<long long>(1, remain_ms)));
        (void)pr;
    }
    if (!reaped) {
        kill_group();
        r.timed_out = true;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
    if (flags >= 0) ::fcntl(out_p[0], F_SETFL, flags);
    slurp();
    ::close(out_p[0]);
    if (WIFEXITED(st)) r.rc = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) r.rc = -WTERMSIG(st);
    else r.rc = st;
#endif
    return r;
}

bool is_fake_adapter(const std::string& text) {
    // Python engine _is_fake_adapter: Catch2/doctest (or unknown --timeout/--unwind)
    // is not CBMC/ESBMC/cppcheck/dafny.
    auto low = lower_copy(text);
    return low.find("doctest version") != std::string::npos ||
           low.find("catch2 v") != std::string::npos ||
           low.find("unknown option") != std::string::npos;
}

bool tool_unusable(const std::string& text, int rc) {
    // Python engine _tool_unusable: doctest/Catch2 / shell 126/127 / not-found is a
    // missing tool, not a verdict.
    if (is_fake_adapter(text)) return true;
    if (rc == 126 || rc == 127) return true;
    auto low = lower_copy(text);
    return low.find("command not found") != std::string::npos ||
           low.find("no such file") != std::string::npos ||
           low.find(": not found") != std::string::npos ||
           low.find("is not recognized as") != std::string::npos ||
           low.find("cannot execute") != std::string::npos;
}

bool probe_looks_missing(const ProcResult& r) {
    // Python engine _probe_looks_missing: doctest/Catch2 masquerading as CBMC/spatch is
    // missing even when --help returns 0 or 1. Scan those strings first.
    // Shell 'not found' / cannot-execute text is missing on any rc — Windows
    // cmd often exits 1 with "is not recognized" on stderr, never a help page.
    auto low = lower_copy(r.text);
    if (is_fake_adapter(r.text) ||
        low.find("doctest version") != std::string::npos ||
        low.find("catch2 v") != std::string::npos ||
        low.find("unknown option: --timeout") != std::string::npos)
        return true;
    if (low.find("command not found") != std::string::npos ||
        low.find("no such file") != std::string::npos ||
        low.find(": not found") != std::string::npos ||
        low.find("is not recognized as") != std::string::npos ||
        low.find("cannot execute") != std::string::npos)
        return true;
    if (r.rc == 0 || r.rc == 1) return false;
    if (r.rc == 126 || r.rc == 127) return true;
    return false;
}

bool frama_c_probe_present(const ProcResult& r) {
    // A Frama-C stub that writes any stderr/error text is missing, not
    // "present". Real --help/--version names Frama-C. Empty rc 0/1 is still
    // a successful probe (the Python engine _probe), never CLEAN.
    if (r.failed || probe_looks_missing(r) || tool_unusable(r.text, r.rc))
        return false;
    auto trimmed = trim_copy(r.text);
    if (trimmed.empty()) return r.rc == 0 || r.rc == 1;
    return lower_copy(trimmed).find("frama") != std::string::npos;
}

bool adapter_start_or_fake_missing(const ProcResult& r) {
    // Process didn't start, or PATH pointed at doctest/Catch2 / rc 126/127.
    if (r.failed) return true;
    return probe_looks_missing(r);
}

std::optional<ProcResult> probe_exe(const std::string& exe) {
    const char* flags[] = {"--help", "-h", "--version", "-version"};
    for (const char* fl : flags) {
        auto r = run_argv({exe, fl}, 12.0);
        if (r.timed_out || r.failed) continue;
        if (probe_looks_missing(r)) continue;
        if (!trim_copy(r.text).empty() || r.rc == 0 || r.rc == 1) return r;
    }
    return std::nullopt;
}

std::vector<fs::path> c_files_of(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    for (const auto& p : paths)
        if (is_c_like(p)) out.push_back(p);
    return out;
}

std::set<fs::path> source_roots(const std::vector<fs::path>& paths) {
    std::set<fs::path> roots;
    if (paths.empty()) {
        roots.insert(fs::path("."));
        return roots;
    }
    for (const auto& p : paths) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) roots.insert(p.parent_path());
        else roots.insert(p);
    }
    return roots;
}

std::string xml_unescape(std::string s) {
    auto repl = [&](std::string_view a, std::string_view b) {
        std::size_t pos = 0;
        while ((pos = s.find(a, pos)) != std::string::npos) {
            s.replace(pos, a.size(), b);
            pos += b.size();
        }
    };
    repl("&apos;", "'");
    repl("&lt;", "<");
    repl("&gt;", ">");
    return s;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

Finding help_ok(const std::string& stage, const std::string& exe, const ProcResult& r) {
    auto f = finding(stage, laws::UNKNOWN, "", "",
                     stage + " present at " + exe + "; ran a help/version probe (not a code verdict)",
                     laws::STRENGTH_FINDS);
    f.extra["exe"] = exe;
    f.extra["help_exit"] = std::to_string(r.rc);
    return f;
}

// `// prism: run` or `/* prism: run */` on the definition (signature lines up
// to the opening brace) or in the comment block directly above it. Same
// rule as the Python engine prism/sanitize.py marked_run.
bool has_run_marker(const std::string& line) {
    static const Regex re(R"((?://|/\*)\s*prism:\s*run\b)");
    return re.search(line);
}

bool run_word(const std::string& line) {
    static const Regex re(R"(\bprism:\s*run\b)");
    return re.search(line);
}

bool comment_line(const std::string& line) {
    static const Regex re(R"(^\s*(?://|/\*|\*)|\*/\s*$)");
    return re.search(line);
}

}  // namespace

bool marked_run(const std::vector<std::string>& lines, int line) {
    const std::size_t i = line > 0 ? static_cast<std::size_t>(line - 1) : 0;
    for (std::size_t j = i; j < lines.size(); ++j) {
        if (has_run_marker(lines[j])) return true;
        if (lines[j].find('{') != std::string::npos || j - i >= 8) break;
    }
    for (std::size_t k = i; k > 0; --k) {
        const auto& ln = lines[k - 1];
        if (trim_copy(ln).empty() || !comment_line(ln)) break;
        if (run_word(ln)) return true;
    }
    return false;
}

// Never "any void(void)": a hostile tree's cleanup_everything() is not
// called. Only a zero-argument, non-static function the author marked.
std::optional<std::string> opted_in_callable(const fs::path& path) {
    std::vector<std::string> lines;
    {
        std::istringstream in(read_text(path));
        std::string ln;
        while (std::getline(in, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(ln);
        }
    }
    const std::string rel = path.string();
    for (const auto& fn : extract_functions(path, rel)) {
        if (!fn.params.empty() || fn.is_static || fn.name == "main") continue;
        if ((fn.kind == "SCALAR" || fn.kind == "VOID") && marked_run(lines, fn.line)) return fn.name;
    }
    return std::nullopt;
}

namespace {

const char* const kOptInHint =
    "mark a zero-argument function with a `// prism: run` comment (trusted code only)";

std::string no_opt_in_message(std::size_t n) {
    return "no function marked `// prism: run` in " + std::to_string(n) +
           " .c file(s); sanitize calls only opted-in zero-argument functions";
}

std::string wrapper_main(const std::optional<std::string>& call) {
    if (call) return "int main(void){\n    (void)" + *call + "();\n    return 0;\n}\n";
    return "int main(void){ return 0; }\n";
}

bool sanitizer_runtime_unusable(const std::string& text) {
    auto low = lower_copy(text);
    return low.find("unexpected memory mapping") != std::string::npos;
}

bool sanitizer_hit(const std::string& text, int rc) {
    if (sanitizer_runtime_unusable(text)) return false;
    auto low = lower_copy(text);
    if (low.find("undefinedbehaviorsanitizer") != std::string::npos) return true;
    if (low.find("threadsanitizer") != std::string::npos) return true;
    if (low.find("addresssanitizer") != std::string::npos) return true;
    if (low.find("heap-buffer-overflow") != std::string::npos) return true;
    if (low.find("runtime error:") != std::string::npos) return true;
    return rc < 0;
}

std::string dumpmachine(const std::string& cc) {
    auto r = run_argv({cc, "-dumpmachine"}, 15.0);
    if (r.timed_out || r.failed) return {};
    return lower_copy(trim_copy(r.text));
}

bool is_mingw(const std::string& cc) {
    // Python engine _is_mingw: MinGW / mingw-w64 triples and paths. Never treat as UBSan.
    auto path = lower_copy(cc);
    for (char& c : path)
        if (c == '\\') c = '/';
    if (path.find("mingw") != std::string::npos) return true;
    return dumpmachine(cc).find("mingw") != std::string::npos;
}

const char* const* sanitizer_lib_names(const std::vector<std::string>& flags) {
    static const char* ts[] = {"libtsan.so",
                               "libtsan.so.1",
                               "libtsan.a",
                               "libtsan.dll",
                               "libtsan.dll.a",
                               "libclang_rt.tsan.a",
                               "libclang_rt.tsan-x86_64.a",
                               nullptr};
    static const char* as[] = {"libasan.so",
                               "libasan.so.1",
                               "libasan.a",
                               "libasan.dll",
                               "libasan.dll.a",
                               "libclang_rt.asan.a",
                               "libclang_rt.asan-x86_64.a",
                               "clang_rt.asan-x86_64.lib",
                               nullptr};
    static const char* ub[] = {"libubsan.so",
                               "libubsan.so.1",
                               "libubsan.a",
                               "libubsan.dll",
                               "libubsan.dll.a",
                               "libclang_rt.ubsan_standalone.a",
                               "libclang_rt.ubsan_standalone-x86_64.a",
                               "clang_rt.ubsan_standalone-x86_64.lib",
                               nullptr};
    std::string joined;
    for (const auto& f : flags) {
        if (!joined.empty()) joined += ' ';
        joined += f;
    }
    if (joined.find("thread") != std::string::npos) return ts;
    if (joined.find("address") != std::string::npos) return as;
    return ub;
}

bool has_sanitizer_lib(const std::string& cc, const std::vector<std::string>& flags) {
    // Python engine _has_sanitizer_lib: -print-file-name must resolve a real runtime, not echo.
    for (const char* const* n = sanitizer_lib_names(flags); *n; ++n) {
        auto r = run_argv({cc, std::string("-print-file-name=") + *n}, 15.0);
        if (r.timed_out || r.failed) continue;
        auto printed = trim_copy(r.text);
        // Some compilers print the name on its own line; take the first line.
        auto nl = printed.find('\n');
        if (nl != std::string::npos) printed = trim_copy(printed.substr(0, nl));
        if (printed.empty()) continue;
        auto norm = printed;
        for (char& c : norm)
            if (c == '\\') c = '/';
        if (norm == *n) continue;
        std::error_code ec;
        if (fs::is_regular_file(printed, ec) && !ec) return true;
    }
    return false;
}

bool probe_sanitizer(const std::string& cc, const std::vector<std::string>& flags) {
    // MinGW without libubsan/libtsan/libasan is False even if -fsanitize=* is
    // accepted. A trivial compile is not a sanitizer. Do not treat MinGW as UBSan.
    if (is_mingw(cc) && !has_sanitizer_lib(cc, flags)) return false;
    try {
        TempDir td("prism_san_probe_");
        auto src = td.path / "probe.c";
        if (!write_text(src, "int main(void){return 0;}\n")) return false;
#ifdef _WIN32
        auto exe = td.path / "probe.exe";
#else
        auto exe = td.path / "probe";
#endif
        auto compile_ok = [&](const fs::path& s, const fs::path& e) {
            std::vector<std::string> cmd{cc};
            cmd.insert(cmd.end(), flags.begin(), flags.end());
            cmd.push_back(s.string());
            cmd.push_back("-o");
            cmd.push_back(e.string());
            auto r = run_argv(cmd, 15.0);
            std::error_code ec;
            return !r.timed_out && !r.failed && r.rc == 0 && fs::is_regular_file(e, ec);
        };
        if (!compile_ok(src, exe)) return false;
        auto has_flag = [&](std::string_view needle) {
            for (const auto& f : flags)
                if (f == needle) return true;
            return false;
        };
        auto fire_hits = [&](const fs::path& fsrc, const fs::path& fexe, std::string_view fire_src) {
            if (!write_text(fsrc, fire_src)) return false;
            if (!compile_ok(fsrc, fexe)) return false;
            auto run = run_argv({fexe.string()}, 15.0);
            if (run.timed_out || run.failed) return false;
            return sanitizer_hit(run.text, run.rc);
        };
        if (has_flag("-fsanitize=undefined")) {
#ifdef _WIN32
            auto fexe = td.path / "ub_fire.exe";
#else
            auto fexe = td.path / "ub_fire";
#endif
            return fire_hits(td.path / "ub_fire.c", fexe,
                             "int main(void){\n"
                             "    volatile int a = 2147483647;\n"
                             "    volatile int b = 1;\n"
                             "    volatile int c = a + b;\n"
                             "    (void)c;\n"
                             "    return 0;\n"
                             "}\n");
        }
        if (has_flag("-fsanitize=address")) {
#ifdef _WIN32
            auto fexe = td.path / "as_fire.exe";
#else
            auto fexe = td.path / "as_fire";
#endif
            return fire_hits(td.path / "as_fire.c", fexe,
                             "#include <stdlib.h>\n"
                             "int main(void){\n"
                             "    char *p = (char*)malloc(1);\n"
                             "    if (!p) return 1;\n"
                             "    p[8] = 1;\n"
                             "    free(p);\n"
                             "    return 0;\n"
                             "}\n");
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::tuple<std::string, std::string, std::string> compile_and_run_san(
    const std::string& cc, const fs::path& source, const std::vector<std::string>& flags,
    double timeout, const std::optional<std::string>& call) {
    if (!call) return {std::string(laws::NOTRUN), no_opt_in_message(1), ""};
    try {
        TempDir td("prism_san_run_");
        auto wrapper = td.path / "prism_san_main.c";
        write_text(wrapper, wrapper_main(call));
#ifdef _WIN32
        auto exe = td.path / "run.exe";
#else
        auto exe = td.path / "run";
#endif
        std::vector<std::string> cmd{cc, "-std=c11"};
        cmd.insert(cmd.end(), flags.begin(), flags.end());
        cmd.push_back(source.string());
        cmd.push_back(wrapper.string());
        cmd.push_back("-o");
        cmd.push_back(exe.string());
        const double cap = std::min(timeout, 15.0);
        auto comp = run_argv(cmd, cap);
        if (comp.timed_out)
            return {std::string(laws::TIMEOUT), "sanitizer compile/run timeout", ""};
        if (comp.failed)
            return {std::string(laws::NOTRUN), "sanitizer compile failed to start", ""};
        if (comp.rc != 0) {
            auto text = comp.text;
            return {std::string(laws::ERROR),
                    tail(text, 800).empty() ? "compile failed (exit " + std::to_string(comp.rc) + ")"
                                            : tail(text, 800),
                    text};
        }
        std::error_code ec;
        if (!fs::is_regular_file(exe, ec))
            return {std::string(laws::ERROR), "compile produced no binary", comp.text};
        // Law 9 sandbox; sanitizer shadow memory needs an unlimited address space.
        auto run = run_argv(sandbox::wrap_argv({exe.string()}, td.path), cap, {},
                            sandbox::limits_for(cap, /*limit_as=*/false));
        if (run.timed_out) return {std::string(laws::TIMEOUT), "sanitizer run timeout", ""};
        if (run.failed) return {std::string(laws::NOTRUN), "sanitizer run failed to start", ""};
        if (sanitizer_runtime_unusable(run.text))
            return {std::string(laws::NOTRUN),
                    "sanitizer runtime unusable (unexpected memory mapping); not a defect finding",
                    run.text};
        if (sanitizer_hit(run.text, run.rc))
            return {std::string(laws::FAILED),
                    tail(run.text, 800).empty() ? "sanitizer abort" : tail(run.text, 800), run.text};
        if (run.rc != 0)
            return {std::string(laws::FAILED),
                    tail(run.text, 800).empty() ? "exit " + std::to_string(run.rc) : tail(run.text, 800),
                    run.text};
        return {std::string(laws::CLEAN),
                "ran under sanitizer with exit 0 (not a proof of absence)", run.text};
    } catch (const std::exception& ex) {
        return {std::string(laws::NOTRUN), ex.what(), ""};
    }
}

std::vector<Finding> run_sanitizer_on_paths(const std::string& cc, const std::vector<fs::path>& paths,
                                            const std::vector<std::string>& flags,
                                            const std::string& sanitizer, const Config& cfg) {
    std::vector<fs::path> c_files;
    for (const auto& p : paths) {
        std::error_code ec;
        if (ext_of(p) == ".c" && fs::is_regular_file(p, ec)) c_files.push_back(p);
    }
    if (c_files.empty()) {
        auto f = finding("sanitize", laws::UNKNOWN, "", sanitizer,
                         sanitizer + " supported by " + cc + "; no .c files in scope",
                         laws::STRENGTH_FINDS);
        f.extra["exe"] = cc;
        f.extra["sanitizer"] = sanitizer;
        return {f};
    }
    std::vector<std::pair<fs::path, std::string>> targets;
    for (const auto& p : c_files)
        if (auto fn = opted_in_callable(p)) targets.emplace_back(p, *fn);
    if (targets.empty()) {
        auto f = finding("sanitize", laws::NOTRUN, "", sanitizer,
                         no_opt_in_message(c_files.size()), laws::STRENGTH_FINDS);
        f.extra["install"] = kOptInHint;
        f.extra["sanitizer"] = sanitizer;
        return {f};
    }
    std::vector<Finding> out;
    for (const auto& [p, fn] : targets) {
        auto [st, msg, evidence] = compile_and_run_san(cc, p, flags, cfg.timeout, fn);
        auto f = finding("sanitize", st, p.string(), sanitizer, msg, laws::STRENGTH_FINDS);
        f.function = fn;
        f.evidence = tail(evidence, 1500);
        f.extra["exe"] = cc;
        f.extra["sanitizer"] = sanitizer;
        f.extra["sandbox"] = sandbox::kind();
        out.push_back(std::move(f));
    }
    return out;
}

Finding sanitizer_notrun(const std::string& message, const std::string& sanitizer) {
    auto f = finding("sanitize", laws::NOTRUN, "", sanitizer, message, laws::STRENGTH_FINDS);
    f.extra["install"] =
        "install gcc or clang with sanitizer support (https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)";
    f.extra["sanitizer"] = sanitizer;
    return f;
}

std::vector<Finding> run_clang_tidy(const std::string& exe, const std::vector<fs::path>& paths,
                                    const Config& cfg) {
    std::vector<fs::path> files;
    for (const auto& p : c_files_of(paths))
        if (is_tu_file(p)) files.push_back(p);
    if (files.empty()) {
        auto f = finding("clang-tidy", laws::UNKNOWN, "", "",
                         "clang-tidy present at " + exe + "; no C/C++ translation units",
                         laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    std::vector<Finding> out;
    for (const auto& p : files) {
        auto ext = ext_of(p);
        const char* stdv = (ext == ".cc" || ext == ".cpp" || ext == ".cxx") ? "-std=c++11" : "-std=c11";
        auto r = run_argv({exe, p.string(), "--", stdv}, std::min(60.0, cfg.timeout + 15));
        if (r.timed_out) {
            out.push_back(finding("clang-tidy", laws::TIMEOUT, p.string(), "", "clang-tidy timeout",
                                  laws::STRENGTH_FINDS));
            continue;
        }
        // Python engine _run_clang_tidy: is_fake_adapter / probe_looks_missing first.
        if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r)) {
            auto f = finding("clang-tidy", laws::NOTRUN, p.string(), "",
                             "clang-tidy at PATH is not clang-tidy (not a proof)",
                             laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("clang-tidy");
            out.push_back(std::move(f));
            continue;
        }
        int hits = 0;
        for (const auto& ln : split_lines(r.text)) {
            if (ln.find(": warning:") != std::string::npos || ln.find(": error:") != std::string::npos) {
                ++hits;
                out.push_back(finding("clang-tidy", laws::FAILED, p.string(), "clang-tidy",
                                      trim_copy(ln).substr(0, 400), laws::STRENGTH_FINDS));
            }
        }
        if (hits == 0) {
            if (r.rc != 0 && r.rc != 1) {
                auto msg = tail(r.text, 400);
                if (msg.empty()) msg = "clang-tidy exit " + std::to_string(r.rc);
                out.push_back(finding("clang-tidy", laws::ERROR, p.string(), "", msg, laws::STRENGTH_FINDS));
            }
        }
    }
    if (out.empty()) {
        auto f = finding("clang-tidy", laws::UNKNOWN, "", "",
                         "clang-tidy present at " + exe + "; no diagnostics (not a proof)",
                         laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    return out;
}

std::vector<Finding> run_cbmc(const std::string& exe, const std::vector<fs::path>& paths,
                              const Config& cfg) {
    std::vector<fs::path> files;
    for (const auto& p : paths)
        if (ext_of(p) == ".c") files.push_back(p);
    if (files.empty()) {
        auto f = finding("cbmc", laws::UNKNOWN, "", "",
                         "cbmc present at " + exe + "; no .c files in scope", laws::STRENGTH_PROVES);
        f.extra["exe"] = exe;
        return {f};
    }
    std::vector<Finding> out;
    for (const auto& p : files) {
        std::vector<std::string> cmd{exe, p.string(), "--unwind", std::to_string(cfg.unwind),
                                     "--timeout", std::to_string(static_cast<int>(cfg.timeout))};
        auto r = run_argv(cmd, cfg.timeout + 5);
        if (r.timed_out) {
            out.push_back(finding("cbmc", laws::TIMEOUT, p.string(), "", "cbmc timeout",
                                  laws::STRENGTH_PROVES));
            continue;
        }
        const auto upper = upper_copy(r.text);
        const auto low = lower_copy(r.text);
        Finding f;
        f.stage = "cbmc";
        f.file = p.string();
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.evidence = tail(r.text, 1500);
        // Python engine _run_cbmc: Catch2/doctest answering as cbmc is NOTRUN, not a proof.
        if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r) ||
            low.find("unknown option") != std::string::npos ||
            low.find("doctest version") != std::string::npos) {
            f.status = std::string(laws::NOTRUN);
            f.message = "cbmc at PATH is not CBMC (not a proof)";
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("cbmc");  // fetch_deps.py --tool cbmc
        } else if (upper.find("VERIFICATION SUCCESSFUL") != std::string::npos) {
            f.status = std::string(laws::BOUNDED);
            f.message = "CBMC: BOUNDED (unwind limited; not a proof)";
        } else if (upper.find("VERIFICATION FAILED") != std::string::npos) {
            f.status = std::string(laws::FAILED);
            f.message = "CBMC verification failed";
        } else if (upper.find("VERIFICATION UNKNOWN") != std::string::npos) {
            f.status = std::string(laws::UNKNOWN);
            f.message = "CBMC unknown";
        } else {
            f.status = std::string(laws::ERROR);
            auto msg = tail(r.text, 400);
            f.message = msg.empty() ? "no verdict line (not a proof)" : msg;
        }
        out.push_back(std::move(f));
    }
    return out;
}

Finding libfuzzer_probe(const Config& cfg) {
    auto clang = cfg.which({"clang"});
    const char* install = "clang -fsanitize=fuzzer is a system tool: apt install clang-18 (see third_party/MANIFEST.toml)";
    if (!clang) {
        auto f = notrun("libfuzzer", "clang", install);
        return f;
    }
#ifdef _WIN32
    const char* null_out = "NUL";
#else
    const char* null_out = "/dev/null";
#endif
    const char* probe_src =
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {"
        " (void)d; (void)n; return 0; }\n";
    try {
        TempDir td("prism_libfuzzer_");
        auto src = td.path / "fuzz_probe.c";
        write_text(src, probe_src);
        auto obj = td.path / "fuzz_probe.o";
        auto r = run_argv({clang->string(), "-fsanitize=fuzzer", "-x", "c", "-c", src.string(), "-o",
                           obj.string()},
                          12.0);
        if (r.rc != 0) {
            auto low = lower_copy(r.text);
            if (low.find("unsupported") != std::string::npos || low.find("unknown") != std::string::npos ||
                low.find("unrecognized") != std::string::npos) {
                auto f = finding("libfuzzer", laws::NOTRUN, "", "",
                                 "clang has no libFuzzer (-fsanitize=fuzzer)", laws::STRENGTH_FINDS);
                f.extra["install"] = install;
                f.extra["exe"] = clang->string();
                return f;
            }
            auto r2 = run_argv({clang->string(), "-fsanitize=fuzzer", "-x", "c", "-c", "-o", null_out,
                                src.string()},
                               12.0);
            if (r2.rc != 0) {
                auto f = finding("libfuzzer", laws::NOTRUN, "", "",
                                 "clang has no libFuzzer (-fsanitize=fuzzer)", laws::STRENGTH_FINDS);
                f.extra["install"] = install;
                f.extra["exe"] = clang->string();
                return f;
            }
        }
    } catch (const std::exception& ex) {
        auto f = finding("libfuzzer", laws::NOTRUN, "", "",
                         std::string("libFuzzer probe failed: ") + ex.what(), laws::STRENGTH_FINDS);
        f.extra["install"] = install;
        f.extra["exe"] = clang->string();
        return f;
    }
    auto f = finding("libfuzzer", laws::UNKNOWN, "", "",
                     "libFuzzer supported by " + clang->string() + "; probe only (not a code verdict)",
                     laws::STRENGTH_FINDS);
    f.extra["exe"] = clang->string();
    return f;
}

std::vector<fs::path> glob_ext(const fs::path& dir, std::string_view ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower_copy(it->path().extension().string()) == ext) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path resolved(const fs::path& p) {
    std::error_code ec;
    auto r = fs::weakly_canonical(p, ec);
    return ec ? fs::absolute(p) : r;
}

std::vector<fs::path> cocci_rules(const std::vector<fs::path>& paths, const Config& cfg) {
    std::vector<fs::path> rules;
    std::set<fs::path> seen;
    auto add_dir = [&](const fs::path& dir) {
        for (const auto& rule : glob_ext(dir, ".cocci")) {
            auto rp = resolved(rule);
            if (seen.insert(rp).second) rules.push_back(rule);
        }
    };
    std::vector<fs::path> pkg;
    auto consider = [&](fs::path base) {
        for (int i = 0; i < 8 && !base.empty(); ++i) {
            pkg.push_back(base / "prism" / "cocci");
            if (base.parent_path() == base) break;
            base = base.parent_path();
        }
    };
    std::error_code ec;
    consider(fs::current_path(ec));
    consider(cfg.root);
#ifdef _WIN32
    char buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) consider(fs::path(buf).parent_path());
#endif
    for (const auto& d : pkg) add_dir(d);
    for (const auto& root : source_roots(paths)) add_dir(root);
    return rules;
}

struct SpatchHit {
    std::string path;
    std::optional<int> line;
    std::string msg;
};

std::vector<SpatchHit> parse_spatch_hits(const std::string& text) {
    static Regex re("^(.+):(\\d+):\\s*(.*)$");
    std::vector<SpatchHit> hits;
    for (const auto& ln : split_lines(text)) {
        auto stripped = trim_copy(ln);
        auto m = re.search_match(stripped);
        if (!m) continue;
        SpatchHit h;
        h.path = m->group(1);
        try {
            h.line = std::stoi(m->group(2));
        } catch (...) {
        }
        h.msg = stripped;
        hits.push_back(std::move(h));
    }
    return hits;
}

bool cocci_has_script(const fs::path& rule) {
    // Same regex as the Python engine adapters_extra._COCCI_SCRIPT_RE.
    static const Regex re(R"(^\s*@\s*(?:script|initialize|finalize)\s*:)", /*multiline=*/true);
    return re.search(read_text(rule));
}

std::vector<Finding> run_spatch(const std::string& exe, const std::vector<fs::path>& paths,
                                const Config& cfg) {
    auto rules = cocci_rules(paths, cfg);
    std::vector<Finding> held;
    if (!cfg.allow_exec) {
        std::vector<fs::path> keep;
        for (const auto& r : rules) {
            if (cocci_has_script(r))
                held.push_back(sandbox::exec_notrun(
                    "spatch", "spatch (" + r.filename().string() + " has a script block)",
                    {{"exe", exe}, {"rule", r.filename().string()}}));
            else
                keep.push_back(r);
        }
        rules = std::move(keep);
        if (rules.empty() && !held.empty()) return held;
    }
    if (rules.empty()) {
        auto f = finding("spatch", laws::UNKNOWN, "", "",
                         "spatch present; no .cocci rules (not a verdict)", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    std::vector<fs::path> c_files;
    for (const auto& p : paths)
        if (ext_of(p) == ".c") {
            c_files.push_back(p);
            if (c_files.size() == 3) break;
        }
    if (c_files.empty()) {
        auto f = finding("spatch", laws::UNKNOWN, "", "",
                         "spatch present at " + exe + "; no .c files in scope", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    std::vector<Finding> out;
    bool any_hit = false;
    for (const auto& rule : rules) {
        const std::string cls = rule.stem().string();
        for (const auto& p : c_files) {
            auto r = run_argv({exe, "--sp-file", rule.string(), "--no-show-diff", p.string()},
                              cfg.timeout + 15);
            if (r.timed_out) {
                auto f = finding("spatch", laws::TIMEOUT, p.string(), cls, "spatch timeout",
                                 laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                f.extra["rule"] = rule.filename().string();
                out.push_back(std::move(f));
                continue;
            }
            if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r)) {
                auto f = finding("spatch", laws::NOTRUN, p.string(), cls,
                                 "spatch at PATH is not Coccinelle (not a proof)",
                                 laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                f.extra["rule"] = rule.filename().string();
                f.extra["install"] = adapter_install("spatch");
                out.push_back(std::move(f));
                continue;
            }
            auto hits = parse_spatch_hits(r.text);
            if (!hits.empty()) {
                any_hit = true;
                for (const auto& h : hits) {
                    auto f = finding("spatch", laws::FAILED, h.path, cls, h.msg.substr(0, 400),
                                     laws::STRENGTH_FINDS);
                    f.line = h.line;
                    f.extra["exe"] = exe;
                    f.extra["rule"] = rule.filename().string();
                    out.push_back(std::move(f));
                }
                continue;
            }
            auto low = lower_copy(r.text);
            // Match-only rules often exit non-zero with "No rules apply".
            // That is silence, not a defect and not a broken spatch.
            if (low.find("no rules apply") != std::string::npos) continue;
            if (r.rc != 0) {
                if (is_fake_adapter(r.text) || probe_looks_missing(r)) {
                    auto f = finding("spatch", laws::NOTRUN, p.string(), cls,
                                     "spatch at PATH is not Coccinelle (not a proof)",
                                     laws::STRENGTH_FINDS);
                    f.extra["exe"] = exe;
                    f.extra["rule"] = rule.filename().string();
                    f.extra["install"] = adapter_install("spatch");
                    out.push_back(std::move(f));
                    continue;
                }
                auto msg = tail(r.text, 400);
                if (msg.empty()) msg = "spatch exit " + std::to_string(r.rc);
                auto f = finding("spatch", laws::ERROR, p.string(), cls, msg, laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                f.extra["rule"] = rule.filename().string();
                out.push_back(std::move(f));
            }
        }
    }
    out.insert(out.begin(), held.begin(), held.end());
    if (any_hit) return out;
    if (out.size() > held.size()) {
        bool only_bad = true;
        for (const auto& f : out)
            if (f.status != laws::TIMEOUT && f.status != laws::ERROR && f.status != laws::NOTRUN)
                only_bad = false;
        if (only_bad) return out;
    }
    auto f = finding("spatch", laws::UNKNOWN, "", "", "spatch ran; no matches (not a proof)",
                     laws::STRENGTH_FINDS);
    f.extra["exe"] = exe;
    return {f};
}

std::string extract_json_object(const std::string& text) {
    auto first = text.find('{');
    auto last = text.rfind('}');
    if (first != std::string::npos && last != std::string::npos && last > first)
        return text.substr(first, last - first + 1);
    return text.empty() ? "{}" : text;
}

std::optional<std::vector<Finding>> parse_semgrep(const std::string& stdout_text,
                                                  const std::string& exe) {
    try {
        auto data = nlohmann::json::parse(extract_json_object(stdout_text));
        auto results = data.contains("results") && data["results"].is_array() ? data["results"]
                                                                              : nlohmann::json::array();
        if (results.empty()) {
            auto f = finding("semgrep", laws::UNKNOWN, "", "",
                             "semgrep ran; no matches (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            return std::vector<Finding>{f};
        }
        std::vector<Finding> out;
        for (const auto& item : results) {
            std::string check_id = "semgrep";
            if (item.contains("check_id") && !item["check_id"].is_null())
                check_id = item["check_id"].is_string() ? item["check_id"].get<std::string>()
                                                        : item["check_id"].dump();
            std::string path;
            if (item.contains("path") && item["path"].is_string()) path = item["path"].get<std::string>();
            std::optional<int> line;
            if (item.contains("start") && item["start"].is_object() && item["start"].contains("line") &&
                item["start"]["line"].is_number_integer())
                line = item["start"]["line"].get<int>();
            std::string msg = check_id;
            if (item.contains("extra") && item["extra"].is_object() && item["extra"].contains("message") &&
                item["extra"]["message"].is_string())
                msg = item["extra"]["message"].get<std::string>();
            auto f = finding("semgrep", laws::FAILED, path, check_id, msg.substr(0, 400),
                             laws::STRENGTH_FINDS);
            f.line = line;
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Finding> run_semgrep(const std::string& exe, const std::vector<fs::path>& paths,
                                 const Config& cfg) {
    std::vector<std::string> files;
    for (const auto& p : c_files_of(paths)) files.push_back(p.string());
    if (files.empty()) {
        auto f = finding("semgrep", laws::UNKNOWN, "", "",
                         "semgrep present at " + exe + "; no C/C++ files in scope", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    const int timeout_s = std::max(1, static_cast<int>(cfg.timeout));
    const char* configs[] = {"p/c", "auto"};
    std::string last_text;
    for (int idx = 0; idx < 2; ++idx) {
        std::vector<std::string> cmd{exe, "--config", configs[idx], "--json", "--quiet", "--timeout",
                                     std::to_string(timeout_s)};
        cmd.insert(cmd.end(), files.begin(), files.end());
        auto r = run_argv(cmd, cfg.timeout + 15);
        if (r.timed_out) {
            auto f = finding("semgrep", laws::TIMEOUT, "", "", "semgrep timeout", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            return {f};
        }
        last_text = r.text;
        if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r)) {
            auto f = finding("semgrep", laws::NOTRUN, "", "",
                             "semgrep at PATH is not semgrep (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("semgrep");
            return {f};
        }
        auto parsed = parse_semgrep(r.text, exe);
        if (parsed) {
            if (r.rc == 0) return *parsed;
            bool any_fail = false;
            for (const auto& f : *parsed)
                if (f.status == laws::FAILED) any_fail = true;
            if (any_fail) return *parsed;
        }
        auto low = lower_copy(r.text);
        const bool ruleset_missing =
            low.find("invalid configuration") != std::string::npos ||
            low.find("could not find config") != std::string::npos ||
            low.find("failed to download") != std::string::npos ||
            low.find("no config") != std::string::npos ||
            low.find("unknown configuration") != std::string::npos;
        if (ruleset_missing && idx + 1 < 2) continue;
        if (parsed) return *parsed;
        if (ruleset_missing) {
            auto f = finding("semgrep", laws::NOTRUN, "", "",
                             tail(r.text, 400).empty()
                                 ? "semgrep ruleset unavailable (not a code verdict)"
                                 : tail(r.text, 400),
                             laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("semgrep");
            return {f};
        }
        auto f = finding("semgrep", laws::ERROR, "", "",
                         tail(r.text, 400).empty() ? "semgrep failed (not a proof)" : tail(r.text, 400),
                         laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    auto f = finding("semgrep", laws::ERROR, "", "",
                     tail(last_text, 400).empty() ? "semgrep config failed (not a proof)"
                                                  : tail(last_text, 400),
                     laws::STRENGTH_FINDS);
    f.extra["exe"] = exe;
    return {f};
}

std::vector<Finding> run_infer(const std::string& exe, const std::vector<fs::path>& paths,
                               const Config& cfg) {
    std::vector<fs::path> c_files;
    for (const auto& p : paths)
        if (ext_of(p) == ".c") {
            c_files.push_back(p);
            if (c_files.size() == 3) break;
        }
    if (c_files.empty()) {
        auto f = finding("infer", laws::UNKNOWN, "", "",
                         "infer present at " + exe + "; no .c files in scope", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    auto compiler = cfg.which({"gcc", "clang"});
    if (!compiler) {
        auto f = finding("infer", laws::NOTRUN, "", "", "infer present but gcc/clang not on PATH",
                         laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        f.extra["install"] = "install gcc or clang";
        return {f};
    }
    std::vector<Finding> out;
    for (const auto& p : c_files) {
        auto abs = fs::absolute(p);
        // Each file gets its own scratch dir for infer-out/ and the object:
        // `infer run -- cc -c` would otherwise write both into the cwd
        // (often the user's tree). Same as the Python engine _run_infer.
        TempDir scratch("prism_infer_");
        auto r = run_argv({exe, "run", "--results-dir", (scratch.path / "infer-out").string(), "--",
                           compiler->string(), "-c", abs.string(), "-o",
                           (scratch.path / "unit.o").string()},
                          cfg.timeout + 15);
        if (r.timed_out) {
            out.push_back(finding("infer", laws::TIMEOUT, p.string(), "", "infer timeout",
                                  laws::STRENGTH_FINDS));
            continue;
        }
        if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r)) {
            auto f = finding("infer", laws::NOTRUN, p.string(), "",
                             "infer at PATH is not Infer (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("infer");
            out.push_back(std::move(f));
            continue;
        }
        bool file_failed = false;
        for (const auto& ln : split_lines(r.text)) {
            if (!re_search("(?i)\\berror:\\s", ln)) continue;
            file_failed = true;
            auto f = finding("infer", laws::FAILED, p.string(), "infer", trim_copy(ln).substr(0, 400),
                             laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        }
        if (file_failed) continue;
        auto low = lower_copy(r.text);
        if (low.find("no issues found") != std::string::npos || trim_copy(r.text).empty()) {
            auto f = finding("infer", laws::UNKNOWN, p.string(), "",
                             "infer ran; no issues (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        } else if (r.rc != 0) {
            auto msg = tail(r.text, 400);
            if (msg.empty()) msg = "infer exit " + std::to_string(r.rc);
            auto f = finding("infer", laws::ERROR, p.string(), "", msg, laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        } else {
            auto f = finding("infer", laws::UNKNOWN, p.string(), "",
                             "infer ran; no issues (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Finding> run_frama_c(const std::string& exe, const std::vector<fs::path>& paths,
                                 const Config& cfg) {
    std::vector<fs::path> c_files;
    for (const auto& p : paths)
        if (ext_of(p) == ".c") {
            c_files.push_back(p);
            if (c_files.size() == 3) break;
        }
    if (c_files.empty()) {
        auto f = finding("frama-c", laws::UNKNOWN, "", "",
                         "frama-c present at " + exe + "; no .c files in scope", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    std::vector<Finding> out;
    for (const auto& p : c_files) {
        auto r = run_argv({exe, "-eva", "-eva-no-progress", "-eva-verbose", "0", p.string()},
                          cfg.timeout + 15);
        if (r.timed_out) {
            out.push_back(finding("frama-c", laws::TIMEOUT, p.string(), "", "frama-c timeout",
                                  laws::STRENGTH_FINDS));
            continue;
        }
        if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r) ||
            tool_unusable(r.text, r.rc)) {
            auto f = finding("frama-c", laws::NOTRUN, p.string(), "",
                             "frama-c at PATH is not Frama-C (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("frama-c");
            out.push_back(std::move(f));
            continue;
        }
        if (trim_copy(r.text).empty() && r.rc != 0 && r.rc != 1) {
            if (probe_looks_missing(r) || tool_unusable(r.text, r.rc)) {
                auto f = finding("frama-c", laws::NOTRUN, p.string(), "",
                                 "frama-c at PATH is not Frama-C (not a proof)",
                                 laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                f.extra["install"] = adapter_install("frama-c");
                out.push_back(std::move(f));
                continue;
            }
            auto f = finding("frama-c", laws::ERROR, p.string(), "",
                             "frama-c exit " + std::to_string(r.rc) + " (parse failure)",
                             laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
            continue;
        }
        bool file_failed = false;
        for (const auto& ln : split_lines(r.text)) {
            auto low = lower_copy(ln);
            if (low.find("warning:") != std::string::npos) {
                file_failed = true;
                auto f = finding("frama-c", laws::FAILED, p.string(), "FUNC-CONTRACT",
                                 trim_copy(ln).substr(0, 400), laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                out.push_back(std::move(f));
            } else if (low.find("alarm") != std::string::npos &&
                       !re_search("(?i)\\b0\\s+alarm", ln)) {
                file_failed = true;
                std::string cls = low.find("uninit") != std::string::npos ? "UNINIT-READ" : "FUNC-CONTRACT";
                auto f = finding("frama-c", laws::FAILED, p.string(), cls, trim_copy(ln).substr(0, 400),
                                 laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                out.push_back(std::move(f));
            }
        }
        if (file_failed) continue;
        if (re_search("(?i)\\b0\\s+alarm", r.text)) {
            auto f = finding("frama-c", laws::UNKNOWN, p.string(), "",
                             "frama-c EVA: 0 alarms (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        } else if (r.rc != 0) {
            auto msg = tail(r.text, 400);
            if (msg.empty()) msg = "frama-c exit " + std::to_string(r.rc);
            auto f = finding("frama-c", laws::ERROR, p.string(), "", msg, laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        } else {
            auto f = finding("frama-c", laws::UNKNOWN, p.string(), "",
                             "frama-c ran; no alarms parsed (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Finding> run_klee(const std::string& exe, const std::vector<fs::path>& paths,
                              const Config& cfg) {
    auto clang = cfg.which({"clang"});
    std::vector<fs::path> c_files;
    for (const auto& p : paths)
        if (ext_of(p) == ".c") {
            c_files.push_back(p);
            if (c_files.size() == 2) break;
        }
    if (c_files.empty()) {
        auto f = finding("klee", laws::UNKNOWN, "", "",
                         "klee present at " + exe + "; no .c files in scope", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    if (!clang) {
        auto f = finding("klee", laws::UNKNOWN, "", "",
                         "klee present; no bitcode toolchain (not a verdict)", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    if (!cfg.allow_exec) {
        // KLEE interprets bitcode but performs external calls (unlink,
        // system, ...) natively on the host.
        return {sandbox::exec_notrun("klee", "klee (native external calls)", {{"exe", exe}})};
    }
    std::vector<Finding> out;
    bool bitcode_ok = false;
    for (const auto& p : c_files) {
        try {
            TempDir td("prism_klee_");
            auto bc = td.path / "x.bc";
            auto br = run_argv({clang->string(), "-emit-llvm", "-c", "-g", "-o", bc.string(),
                                fs::absolute(p).string()},
                               std::min(30.0, cfg.timeout + 10));
            std::error_code ec;
            if (br.rc != 0 || !fs::is_regular_file(bc, ec)) continue;
            bitcode_ok = true;
            const double kcap = std::min(20.0, cfg.timeout + 10);
            auto kr = run_argv(sandbox::wrap_argv({exe, "--max-time=5", "--max-forks=16", bc.string()},
                                                  td.path),
                               kcap, td.path, sandbox::limits_for(kcap, /*limit_as=*/false));
            if (kr.timed_out) {
                out.push_back(finding("klee", laws::TIMEOUT, p.string(), "", "klee timeout",
                                      laws::STRENGTH_FINDS));
                continue;
            }
            if (kr.failed || is_fake_adapter(kr.text) || probe_looks_missing(kr)) {
                auto f = finding("klee", laws::NOTRUN, p.string(), "",
                                 "klee at PATH is not KLEE (not a proof)", laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                f.extra["install"] = adapter_install("klee");
                out.push_back(std::move(f));
                continue;
            }
            bool crash = kr.text.find("KLEE: ERROR") != std::string::npos;
            if (!crash) {
                for (fs::recursive_directory_iterator it(td.path, ec), end; it != end && !ec;
                     it.increment(ec)) {
                    auto name = lower_copy(it->path().filename().string());
                    auto ext = lower_copy(it->path().extension().string());
                    if (name.find("error") != std::string::npos || ext == ".err") crash = true;
                }
            }
            if (crash) {
                auto f = finding("klee", laws::FAILED, p.string(), "klee",
                                 tail(kr.text, 400).empty() ? "klee error path" : tail(kr.text, 400),
                                 laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                f.evidence = tail(kr.text, 1500);
                out.push_back(std::move(f));
            } else {
                auto f = finding("klee", laws::UNKNOWN, p.string(), "",
                                 "klee ran; no path dumped a error (not a proof)", laws::STRENGTH_FINDS);
                f.extra["exe"] = exe;
                out.push_back(std::move(f));
            }
        } catch (const std::exception& ex) {
            auto f = finding("klee", laws::NOTRUN, p.string(), "",
                             std::string("klee unusable: ") + ex.what(), laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("klee");
            out.push_back(std::move(f));
        }
    }
    if (out.empty() && !bitcode_ok) {
        auto f = finding("klee", laws::UNKNOWN, "", "",
                         "klee present; no bitcode toolchain (not a verdict)", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe;
        return {f};
    }
    return out;
}

std::vector<fs::path> strix_specs(const std::vector<fs::path>& paths) {
    std::vector<fs::path> specs;
    std::set<fs::path> seen;
    for (const auto& root : source_roots(paths)) {
        for (const char* ext : {".ltl", ".tlsf"}) {
            for (const auto& spec : glob_ext(root, ext)) {
                if (seen.insert(spec).second) specs.push_back(spec);
            }
        }
    }
    return specs;
}

std::vector<Finding> run_strix(const std::string& exe, const std::vector<fs::path>& paths,
                               const Config& cfg, const ProcResult& probed) {
    auto specs = strix_specs(paths);
    if (specs.empty()) return {help_ok("strix", exe, probed)};
    std::vector<Finding> out;
    for (std::size_t i = 0; i < specs.size() && i < 3; ++i) {
        const auto& spec = specs[i];
        auto r = run_argv({exe, spec.string()}, cfg.timeout + 10);
        if (r.timed_out) {
            out.push_back(finding("strix", laws::TIMEOUT, spec.string(), "", "strix timeout",
                                  laws::STRENGTH_FINDS));
            continue;
        }
        if (r.failed || is_fake_adapter(r.text) || probe_looks_missing(r)) {
            auto f = finding("strix", laws::NOTRUN, spec.string(), "",
                             "strix at PATH is not Strix (not a proof)", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            f.extra["install"] = adapter_install("strix");
            out.push_back(std::move(f));
            continue;
        }
        auto low = lower_copy(r.text);
        if (low.find("counterexample") != std::string::npos || low.find("violation") != std::string::npos ||
            low.find("falsified") != std::string::npos) {
            auto f = finding("strix", laws::FAILED, spec.string(), "strix",
                             tail(r.text, 400).empty() ? "strix spec failed" : tail(r.text, 400),
                             laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        } else {
            auto f = finding("strix", laws::UNKNOWN, spec.string(), "",
                             "strix ran; result is not a proof", laws::STRENGTH_FINDS);
            f.extra["exe"] = exe;
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Finding> dispatch_optional(const std::string& stage, const std::string& exe,
                                       const std::vector<fs::path>& paths, const Config& cfg,
                                       const ProcResult& probed) {
    auto c_files = c_files_of(paths);
    if (stage == "clang-tidy") return run_clang_tidy(exe, paths, cfg);
    if (stage == "cbmc") return run_cbmc(exe, paths, cfg);
    if (stage == "afl-fuzz") return {help_ok(stage, exe, probed)};
    if (stage == "strix") return run_strix(exe, paths, cfg, probed);
    if (stage == "spatch") return run_spatch(exe, paths, cfg);
    if (c_files.empty()) return {help_ok(stage, exe, probed)};
    if (stage == "semgrep") return run_semgrep(exe, paths, cfg);
    if (stage == "infer") return run_infer(exe, paths, cfg);
    if (stage == "frama-c") return run_frama_c(exe, paths, cfg);
    if (stage == "klee") return run_klee(exe, paths, cfg);
    return {help_ok(stage, exe, probed)};
}

struct OptionalTool {
    const char* stage;
    const char* names[3];
};

}  // namespace

std::string compiler_key(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::canonical(p, ec);
    if (ec) r = fs::absolute(p, ec);
    if (ec) r = p;
    auto s = lower_copy(r.string());
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

std::vector<Finding> run_compiler(const std::vector<fs::path>& paths, const Config& cfg) {
    std::vector<fs::path> compilers;
    if (auto gcc = cfg.which({"gcc"})) compilers.push_back(*gcc);
    if (auto clang = cfg.which({"clang"})) {
        auto key = compiler_key(*clang);
        bool dup = false;
        for (const auto& c : compilers)
            if (compiler_key(c) == key) {
                dup = true;
                break;
            }
        if (!dup) compilers.push_back(*clang);
    }
    if (compilers.empty()) {
        return {notrun("warnings", "gcc", "install gcc or clang")};
    }
    std::vector<fs::path> units;
    for (const auto& p : paths) {
        auto ext = ext_of(p);
        if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx") units.push_back(p);
    }
    if (units.empty()) {
        return {finding("warnings", laws::UNKNOWN, "", "",
                        "gcc/clang present; no .c/.cc/.cpp/.cxx files in scope", laws::STRENGTH_SOME)};
    }
    std::vector<Finding> out;
    std::set<std::tuple<std::string, std::optional<int>, std::string, std::string, std::string>> seen;
    auto add = [&](Finding f) {
        auto key = std::make_tuple(f.file, f.line, f.cls, f.status, f.message);
        if (!seen.insert(key).second) return;
        out.push_back(std::move(f));
    };
    static Regex wrn("^(.+):(\\d+):(\\d+):\\s+(warning|error):\\s+(.*)$", true);
    for (const auto& cc : compilers) {
        for (const auto& p : units) {
            auto ext = ext_of(p);
            const char* stdv = (ext == ".cc" || ext == ".cpp" || ext == ".cxx") ? "-std=c++11" : "-std=c11";
            std::vector<std::string> cmd{cc.string(), stdv, "-Wall", "-Wextra", "-Wconversion",
                                         "-Wsign-compare", "-Wshift-overflow", "-fsyntax-only", p.string()};
            for (const auto& flag : cmd) {
                if (flag == "-w" || flag.rfind("-Wno-", 0) == 0)
                    throw std::runtime_error("refusing to disable a check: " + flag);
            }
            auto r = run_argv(cmd, 30.0);
            if (r.timed_out) {
                add(finding("warnings", laws::TIMEOUT, p.string(), "",
                            "compiler syntax-check timeout", laws::STRENGTH_SOME));
                continue;
            }
            if (r.failed || tool_unusable(r.text, r.rc)) {
                auto f = finding("warnings", laws::NOTRUN, p.string(), "",
                                 cc.string() + " unusable: failed to start", laws::STRENGTH_SOME);
                f.extra["install"] = "install gcc or clang";
                add(std::move(f));
                continue;
            }
            int hits = 0;
            for (auto& m : wrn.finditer(r.text)) {
                ++hits;
                Finding f;
                f.stage = "warnings";
                f.status = std::string(laws::FAILED);
                f.file = m.group(1);
                try {
                    f.line = std::stoi(m.group(2));
                } catch (...) {
                }
                f.cls = std::string("compiler-") + m.group(4);
                f.message = m.group(5);
                f.strength = std::string(laws::STRENGTH_SOME);
                add(std::move(f));
            }
            if (hits == 0 && r.rc != 0) {
                auto msg = tail(r.text, 400);
                if (msg.empty()) msg = cc.string() + " exit " + std::to_string(r.rc);
                add(finding("warnings", laws::FAILED, p.string(), "compiler-error", msg,
                            laws::STRENGTH_SOME));
            }
        }
    }
    return out;
}

static std::vector<Finding> run_cppcheck_unstamped(const std::vector<fs::path>& paths,
                                            const Config& cfg) {
    auto exe = cfg.which_adapter("cppcheck", {"cppcheck", "cppcheck.exe"});
    if (!exe) return {notrun("cppcheck", "cppcheck", adapter_install("cppcheck"))};
    std::vector<std::string> files;
    for (const auto& p : paths) {
        auto e = ext_of(p);
        if (e == ".c" || e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".h" || e == ".hpp")
            files.push_back(p.string());
    }
    if (files.empty()) {
        auto f = finding("cppcheck", laws::UNKNOWN, "", "",
                         "cppcheck present; no C/C++ files in scope", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe->string();
        return {f};
    }
    std::vector<std::string> cmd{exe->string(), "--enable=warning,style,performance,portability", "--xml",
                                 "--xml-version=2"};
    cmd.insert(cmd.end(), files.begin(), files.end());
    auto r = run_argv(cmd, 120.0);
    if (r.timed_out)
        return {finding("cppcheck", laws::TIMEOUT, "", "", "cppcheck timeout", laws::STRENGTH_FINDS)};
    static Regex err(
        "<error id=\"([^\"]+)\" severity=\"([^\"]+)\" msg=\"([^\"]+)\"[^>]*>"
        "(?:\\s*<location file=\"([^\"]+)\" line=\"(\\d+)\")?");
    std::vector<Finding> out;
    for (auto& m : err.finditer(r.text)) {
        const auto eid = m.group(1);
        const auto sev = m.group(2);
        if ((sev == "information" || sev == "style") && eid.starts_with("unused")) continue;
        Finding f;
        f.stage = "cppcheck";
        f.status = std::string(laws::FAILED);
        f.file = m.group(4);
        if (!m.group(5).empty()) {
            try {
                f.line = std::stoi(m.group(5));
            } catch (...) {
            }
        }
        f.cls = eid;
        f.message = xml_unescape(m.group(3));
        f.strength = std::string(laws::STRENGTH_FINDS);
        out.push_back(std::move(f));
    }
    if (out.empty() && (tool_unusable(r.text, r.rc) || r.failed)) {
        auto f = finding("cppcheck", laws::NOTRUN, "", "",
                         "cppcheck at PATH is not cppcheck (not a proof)", laws::STRENGTH_FINDS);
        f.extra["exe"] = exe->string();
        f.extra["install"] = adapter_install("cppcheck");
        return {f};
    }
    if (out.empty() && r.rc != 0 && r.rc != 1) {
        auto msg = tail(r.text, 400);
        if (msg.empty()) msg = "cppcheck exit " + std::to_string(r.rc);
        auto f = finding("cppcheck", laws::ERROR, "", "", msg, laws::STRENGTH_FINDS);
        f.extra["exe"] = exe->string();
        return {f};
    }
    if (out.empty()) {
        auto f = finding("cppcheck", laws::UNKNOWN, "", "",
                         "cppcheck present at " + exe->string() + "; no diagnostics (not a proof)",
                         laws::STRENGTH_FINDS);
        f.extra["exe"] = exe->string();
        return {f};
    }
    (void)cfg;
    return out;
}

static std::vector<Finding> run_esbmc_unstamped(const std::vector<fs::path>& paths,
                                            const Config& cfg) {
    auto exe = cfg.which_adapter("esbmc", {"esbmc", "esbmc.exe"});
    if (!exe) return {notrun("esbmc", "esbmc", adapter_install("esbmc"))};
    std::vector<fs::path> files;
    for (const auto& p : paths)
        if (ext_of(p) == ".c") files.push_back(p);
    if (files.empty()) {
        auto f = finding("esbmc", laws::UNKNOWN, "", "",
                         "esbmc present at " + exe->string() + "; no .c files in scope",
                         laws::STRENGTH_PROVES);
        f.extra["exe"] = exe->string();
        return {f};
    }
    std::vector<Finding> out;
    for (const auto& p : files) {
        std::vector<std::string> cmd{exe->string(), p.string(), "--unwind", std::to_string(cfg.unwind),
                                     "--overflow-check", "--memory-leak-check", "--timeout",
                                     std::to_string(static_cast<int>(cfg.timeout))};
        auto r = run_argv(cmd, cfg.timeout + 5);
        if (r.timed_out) {
            out.push_back(finding("esbmc", laws::TIMEOUT, p.string(), "", "timeout", laws::STRENGTH_PROVES));
            continue;
        }
        const auto upper = upper_copy(r.text);
        Finding f;
        f.stage = "esbmc";
        f.file = p.string();
        f.strength = std::string(laws::STRENGTH_PROVES);
        f.evidence = tail(r.text, 1500);
        if (r.failed || tool_unusable(r.text, r.rc)) {
            f.status = std::string(laws::NOTRUN);
            f.message = "esbmc at PATH is not ESBMC (not a proof)";
            f.extra["exe"] = exe->string();
            f.extra["install"] = adapter_install("esbmc");
        } else if (upper.find("VERIFICATION SUCCESSFUL") != std::string::npos) {
            std::string st = upper.find("INDUCTION") == std::string::npos
                                 ? std::string(laws::PROVED)
                                 : std::string(laws::PROVED_UNBOUNDED);
            bool k_ind = false;
            for (const auto& flag : cmd)
                if (flag == "--k-induction") k_ind = true;
            if (!k_ind)
                st = upper.find("UNWINDING ASSERTION") != std::string::npos
                         ? std::string(laws::BOUNDED)
                         : std::string(laws::PROVED);
            f.status = st;
            f.message = "ESBMC: " + st;
        } else if (upper.find("VERIFICATION FAILED") != std::string::npos) {
            f.status = std::string(laws::FAILED);
            f.message = "ESBMC verification failed";
        } else if (upper.find("VERIFICATION UNKNOWN") != std::string::npos) {
            f.status = std::string(laws::UNKNOWN);
            f.message = "ESBMC unknown";
        } else {
            f.status = std::string(laws::ERROR);
            auto msg = tail(r.text, 400);
            f.message = msg.empty() ? "no verdict line (not a proof)" : msg;
        }
        out.push_back(std::move(f));
    }
    return out;
}

static std::vector<Finding> run_dafny_unstamped(const std::vector<fs::path>& paths,
                                            const Config& cfg) {
    auto exe = cfg.which_adapter("dafny", {"dafny", "dafny.exe"});
    if (!exe) return {notrun("dafny", "dafny", adapter_install("dafny"))};
    std::vector<Finding> out;
    for (const auto& p : paths) {
        if (ext_of(p) != ".dfy") continue;
        auto r = run_argv({exe->string(), "verify", p.string()}, 60.0);
        if (r.timed_out) {
            out.push_back(finding("dafny", laws::TIMEOUT, p.string(), "FUNC-CONTRACT", "dafny timeout",
                                  laws::STRENGTH_PROVES));
            continue;
        }
        if (r.failed || tool_unusable(r.text, r.rc)) {
            auto f = finding("dafny", laws::NOTRUN, p.string(), "FUNC-CONTRACT",
                             "dafny at PATH is not Dafny (not a proof)", laws::STRENGTH_PROVES);
            f.extra["exe"] = exe->string();
            f.extra["install"] = adapter_install("dafny");
            out.push_back(std::move(f));
            continue;
        }
        auto f = finding("dafny", r.rc == 0 ? laws::PROVED : laws::FAILED, p.string(), "FUNC-CONTRACT",
                         tail(r.text, 400), laws::STRENGTH_PROVES);
        out.push_back(std::move(f));
    }
    if (out.empty())
        out.push_back(finding("dafny", laws::NOTRUN, "", "", "no .dfy files in scope",
                              laws::STRENGTH_PROVES));
    (void)cfg;
    return out;
}

// Findings from an external tool carry extra["tool_sha"] (roadmap 1.1);
// prism/adapters.py @stamps_tool.
std::vector<Finding> run_cppcheck(const std::vector<fs::path>& paths, const Config& cfg) {
    auto out = run_cppcheck_unstamped(paths, cfg);
    if (auto exe = cfg.which_adapter("cppcheck", {"cppcheck", "cppcheck.exe"})) stamp_tool_sha(out, *exe);
    return out;
}

std::vector<Finding> run_esbmc(const std::vector<fs::path>& paths, const Config& cfg) {
    auto out = run_esbmc_unstamped(paths, cfg);
    if (auto exe = cfg.which_adapter("esbmc", {"esbmc", "esbmc.exe"})) stamp_tool_sha(out, *exe);
    return out;
}

std::vector<Finding> run_dafny(const std::vector<fs::path>& paths, const Config& cfg) {
    auto out = run_dafny_unstamped(paths, cfg);
    if (auto exe = cfg.which_adapter("dafny", {"dafny", "dafny.exe"})) stamp_tool_sha(out, *exe);
    return out;
}

std::vector<Finding> run_sanitize(const std::vector<fs::path>& paths, const Config& cfg) {
    // Law 9: the stage runs code from the scanned tree.
    if (!cfg.allow_exec) return {sandbox::exec_notrun("sanitize", "sanitize (ASan/UBSan/TSan runs)")};
    auto cc = cfg.which({"gcc", "clang"});
    if (!cc) {
        auto missing = sanitizer_notrun("gcc/clang not on PATH", "ubsan");
        missing.status = laws::NOTRUN;
        return {missing};
    }
    const std::vector<std::string> as_flags{"-fsanitize=address", "-fno-sanitize-recover=address", "-O0"};
    const std::vector<std::string> ub_flags{"-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-O0"};
    const std::vector<std::string> ts_flags{"-fsanitize=thread", "-O0"};
    std::vector<Finding> out;
    if (!probe_sanitizer(cc->string(), as_flags)) {
        out.push_back(sanitizer_notrun("compiler has no ASan", "asan"));
    } else {
        auto asan = run_sanitizer_on_paths(cc->string(), paths, as_flags, "asan", cfg);
        out.insert(out.end(), asan.begin(), asan.end());
    }
    if (!probe_sanitizer(cc->string(), ub_flags)) {
        out.push_back(sanitizer_notrun("compiler has no UBSan", "ubsan"));
    } else {
        auto ub = run_sanitizer_on_paths(cc->string(), paths, ub_flags, "ubsan", cfg);
        out.insert(out.end(), ub.begin(), ub.end());
    }
    if (!probe_sanitizer(cc->string(), ts_flags)) {
        out.push_back(sanitizer_notrun("compiler has no TSan", "tsan"));
    } else {
        auto ts = run_sanitizer_on_paths(cc->string(), paths, ts_flags, "tsan", cfg);
        out.insert(out.end(), ts.begin(), ts.end());
    }
    return out;
}

std::vector<Finding> run_optional_tools(const std::vector<fs::path>& paths, const Config& cfg) {
    const OptionalTool tools[] = {
        {"klee", {"klee", nullptr, nullptr}},
        {"afl-fuzz", {"afl-fuzz", "afl-fuzz.exe", nullptr}},
        {"frama-c", {"frama-c", "frama-c.exe", nullptr}},
        {"infer", {"infer", nullptr, nullptr}},
        {"clang-tidy", {"clang-tidy", "clang-tidy.exe", nullptr}},
        {"cbmc", {"cbmc", "cbmc.exe", nullptr}},
        {"strix", {"strix", "strix.exe", nullptr}},
        {"semgrep", {"semgrep", "semgrep.exe", nullptr}},
        {"spatch", {"spatch", "spatch.exe", nullptr}},
    };
    std::vector<Finding> out;
    for (const auto& tool : tools) {
        std::string first_name;
        std::vector<std::string_view> names;
        for (const char* n : tool.names) {
            if (!n) continue;
            if (first_name.empty()) first_name = n;
            names.emplace_back(n);
        }
        const auto install = adapter_install(tool.stage);
        std::optional<fs::path> exe;
        if (names.size() == 1)
            exe = cfg.which_adapter(tool.stage, {names[0]});
        else if (names.size() == 2)
            exe = cfg.which_adapter(tool.stage, {names[0], names[1]});
        else if (names.size() >= 3)
            exe = cfg.which_adapter(tool.stage, {names[0], names[1], names[2]});
        if (!exe) {
            out.push_back(notrun(tool.stage, first_name, install));
            continue;
        }
        std::optional<ProcResult> probed;
        try {
            probed = probe_exe(exe->string());
        } catch (const std::exception& ex) {
            auto f = notrun(tool.stage, first_name, install);
            f.message = std::string(tool.stage) + " probe failed: " + ex.what();
            f.extra["exe"] = exe->string();
            out.push_back(std::move(f));
            continue;
        }
        if (!probed) {
            auto f = notrun(tool.stage, first_name, install);
            f.message = std::string(tool.stage) + " at " + exe->string() +
                        " did not answer --help/-h/--version";
            f.extra["exe"] = exe->string();
            out.push_back(std::move(f));
            continue;
        }
        if (std::string(tool.stage) == "frama-c" && !frama_c_probe_present(*probed)) {
            auto f = notrun(tool.stage, first_name, install);
            f.message = std::string(tool.stage) + " at " + exe->string() +
                        " did not answer --help/-h/--version";
            f.extra["exe"] = exe->string();
            out.push_back(std::move(f));
            continue;
        }
        try {
            auto more = dispatch_optional(tool.stage, exe->string(), paths, cfg, *probed);
            stamp_tool_sha(more, *exe);
            out.insert(out.end(), more.begin(), more.end());
        } catch (const std::system_error& ex) {
            // Python engine adapters_extra.run_optional_tools: OSError → NOTRUN unusable.
            auto f = notrun(tool.stage, first_name, install);
            f.message = std::string(tool.stage) + " unusable: " + ex.what();
            f.extra["exe"] = exe->string();
            out.push_back(std::move(f));
        } catch (const std::exception& ex) {
            auto f = finding(tool.stage, laws::ERROR, "", "",
                             std::string(tool.stage) + " run failed: " + ex.what(), laws::STRENGTH_FINDS);
            f.extra["install"] = install;
            f.extra["exe"] = exe->string();
            out.push_back(std::move(f));
        }
    }
    out.push_back(libfuzzer_probe(cfg));
    return out;
}

namespace {

// Python engine prism/pbsd.py run_pbsd_lints (~349). Tree present is never CLEAN-as-proof
// and never PROVED. C++ cannot import tools/verify modules: extra.ported lists
// VERIFY_SCANNERS; extra.invoked lists restaged checker class names that fired.
// Without the tree, only PORTABLE_CLS (prism.portable). With the tree, also
// restage matching in-tree copies as prism.checkers. Do not wrap sweep_all.py as CLEAN.
// Behavioral tests: tests/test_pbsd.py (the Python engine).

constexpr const char* kPbsdCExts[] = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh"};

constexpr const char* kPbsdVerifyScanners[] = {
    "realloc_self",        "onesided_index", "capacity_first", "nowait_check",
    "noreturn_check",      "null_branch",    "lock_balance",   "masked_switch_check",
};

// Python engine PORTABLE_CLS - restage with or without the tree (via=prism.portable).
constexpr const char* kPbsdPortableCls[] = {
    "MEM-ONESIDED-INDEX",
    "MEM-CAPACITY-FIRST",
};

// In-tree copies of the Python engine-imported scanners (plus sibling_guard). Restage only
// when tools/verify is present (via=prism.checkers). PTR-NULL-DEREF /
// LOCK-IMBALANCE match prism/pbsd.py _run_null_branch / _run_lock_balance;
// other LOCK-* / NULL-* from C++ checkers ride the same prefix.
constexpr const char* kPbsdTreeCls[] = {
    "MEM-REALLOC-SELF",
    "MEM-NOWAIT",
    "FUNC-NORETURN",
    "UNINIT-SWITCH",
    "CTRL-SIBLING-ASYMMETRY",
    "PTR-NULL-DEREF",
    "LOCK-IMBALANCE",
};

struct PbsdHeavy {
    const char* binary;
    const char* tool;
    const char* how;
};

constexpr PbsdHeavy kPbsdHeavy[] = {
    {"clang", "analyze", "pkg install llvm / apt install clang"},
    {"goto-cc", "classify", "pkg install cbmc / apt install cbmc"},
    {"cbmc", "cbmc", "pkg install cbmc / apt install cbmc"},
};

fs::path pbsd_looked_root(const Config& cfg) {
    // Honor PRISM_PBSD even when cfg.pbsd_root is already set.
    if (const char* env = std::getenv("PRISM_PBSD"); env && *env) return fs::path(env);
    return cfg.pbsd_root;
}

bool pbsd_tree_present(const fs::path& looked) {
    std::error_code ec;
    return fs::is_directory(looked / "tools" / "verify", ec) && !ec;
}

std::vector<fs::path> pbsd_c_paths(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    for (const auto& p : paths) {
        auto e = ext_of(p);
        bool ok = false;
        for (auto* ext : kPbsdCExts) {
            if (e == ext) {
                ok = true;
                break;
            }
        }
        if (!ok) continue;
        std::error_code ec;
        if (fs::is_regular_file(p, ec) && !ec) out.push_back(p);
    }
    return out;
}

std::vector<Finding> pbsd_dedupe(std::vector<Finding> findings) {
    std::set<std::tuple<std::string, std::optional<int>, std::string, std::string>> seen;
    std::vector<Finding> out;
    out.reserve(findings.size());
    for (auto& f : findings) {
        auto key = std::make_tuple(f.file, f.line, f.cls, f.message);
        if (!seen.insert(key).second) continue;
        out.push_back(std::move(f));
    }
    return out;
}

Finding pbsd_not_run(std::string message, std::string install,
                     std::map<std::string, std::string> extra) {
    auto f = finding("pbsd", laws::NOTRUN, "", "", std::move(message), laws::STRENGTH_FINDS);
    f.extra = std::move(extra);
    f.extra["install"] = std::move(install);
    return f;
}

bool pbsd_portable_cls(std::string_view cls) {
    for (auto* n : kPbsdPortableCls)
        if (cls == n) return true;
    return false;
}

bool pbsd_tree_extra_cls(std::string_view cls) {
    for (auto* n : kPbsdTreeCls)
        if (cls == n) return true;
    if (cls.size() >= 5 &&
        (cls.compare(0, 5, "LOCK-") == 0 || cls.compare(0, 5, "NULL-") == 0))
        return true;
    return false;
}

std::vector<Finding> prism_portable(const std::vector<fs::path>& files, const Config& cfg,
                                    bool tree_present) {
    fs::path root = cfg.root;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) root = ".";
    std::vector<Finding> out;
    for (auto& f : run_lints(files, root, cfg.jobs)) {
        const bool portable = pbsd_portable_cls(f.cls);
        const bool checkers = tree_present && pbsd_tree_extra_cls(f.cls);
        if (!portable && !checkers) continue;
        if (laws::is_proof(f.status)) continue;  // Never PROVED.
        Finding g = f;
        g.stage = "pbsd";
        g.extra.clear();
        g.extra["via"] = portable ? "prism.portable" : "prism.checkers";
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<Finding> pbsd_heavy_notrun(const Config& cfg) {
    std::vector<Finding> out;
    for (const auto& h : kPbsdHeavy) {
        if (cfg.which({h.binary})) continue;
        auto f = pbsd_not_run(
            std::string(h.binary) + " not on PATH (needed for " + h.tool + "; not a clean sweep)",
            h.how, {{"tool", h.tool}, {"via", "missing-bin"}});
        out.push_back(std::move(f));
    }
    return out;
}

std::string pbsd_ported_json() {
    nlohmann::json j = nlohmann::json::array();
    for (auto* n : kPbsdVerifyScanners) j.push_back(n);
    return j.dump();
}

std::string pbsd_invoked_json(const std::vector<Finding>& findings) {
    nlohmann::json j = nlohmann::json::array();
    std::set<std::string> seen;
    for (const auto& f : findings) {
        if (f.cls.empty() || !seen.insert(f.cls).second) continue;
        j.push_back(f.cls);
    }
    return j.dump();
}

}  // namespace

std::vector<Finding> run_pbsd_lints(const std::vector<fs::path>& paths, const Config& cfg) {
    auto files = pbsd_c_paths(paths);
    auto looked = pbsd_looked_root(cfg);
    const bool tree = pbsd_tree_present(looked);
    auto portable = prism_portable(files, cfg, tree);

    if (!tree) {
        if (!portable.empty()) return pbsd_dedupe(std::move(portable));
        return {pbsd_not_run(
            "ParanoidBSD tree not found at " + looked.string(),
            "set PRISM_PBSD to the ParanoidBSD tree (looked at " + looked.string() + ")",
            {{"looked", looked.string()}})};
    }

    // Tree present: portable + prism.checkers restage + missing-bin NOTRUN.
    // Not a clean sweep, not CLEAN-as-proof. Empty list is OK (heavies on PATH).
    auto heavy = pbsd_heavy_notrun(cfg);
    portable.insert(portable.end(), heavy.begin(), heavy.end());
    auto out = pbsd_dedupe(std::move(portable));
    if (out.empty()) return {};
    const auto ported = pbsd_ported_json();
    const auto invoked = pbsd_invoked_json(out);
    for (auto& f : out) {
        f.extra["invoked"] = invoked;
        f.extra["ported"] = ported;
    }
    return out;
}

detail::ProcOut detail::run_process(const std::vector<std::string>& args, double timeout_s,
                                    const fs::path& cwd) {
    auto r = run_argv(args, timeout_s, cwd);
    return {std::move(r.text), r.rc, r.timed_out, r.failed};
}

}  // namespace prism
