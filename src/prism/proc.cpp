// The one process runner of prism_core (proc.hpp): CreateProcessW with a
// quoted command line on Windows, fork + execvp in a process group of its own
// on POSIX. Stdin, an environment overlay for the child only, a working
// directory, sandbox rlimits and merged or split stdout/stderr.
#include "proc.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
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
#  include <csignal>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace prism::detail {
namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring upper_w(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

// PRISM's environment with the overlay applied, as a CREATE_UNICODE_ENVIRONMENT
// block (sorted case-insensitively, double-NUL terminated).
std::vector<wchar_t> env_block(const std::vector<std::pair<std::string, std::string>>& overlay) {
    std::vector<std::wstring> entries;
    if (wchar_t* base = GetEnvironmentStringsW()) {
        for (const wchar_t* p = base; *p; p += wcslen(p) + 1) entries.emplace_back(p);
        FreeEnvironmentStringsW(base);
    }
    for (const auto& [k, v] : overlay) {
        const auto key = upper_w(widen(k));
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& e) {
                                         // "=C:=C:\x" style entries start with '='
                                         auto eq = e.find(L'=', 1);
                                         return upper_w(e.substr(0, eq)) == key;
                                     }),
                      entries.end());
        entries.push_back(widen(k) + L"=" + widen(v));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return upper_w(a) < upper_w(b);
    });
    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace

RunOut run(const RunSpec& spec) {
    RunOut r;
    const auto& args = spec.argv;
    if (args.empty()) {
        r.failed = true;
        r.err = "no argv";
        return r;
    }
    // No cmd.exe in between: CreateProcessW gets one command line quoted for
    // CommandLineToArgvW; a .bat/.cmd target (which Windows runs through
    // cmd.exe anyway) is quoted for cmd.exe or refused.
    std::string cl;
    if (sandbox::is_batch_file(args[0])) {
        auto bl = sandbox::batch_command_line(args);
        if (!bl) {
            r.failed = true;
            r.err = "refusing to pass %, !, \" or a newline to a batch file";
            return r;
        }
        cl = *bl;
    } else {
        cl = sandbox::windows_command_line(args);
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr,
           err_w = nullptr;
    auto close_h = [](HANDLE& h) {
        if (h) CloseHandle(h);
        h = nullptr;
    };
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) ||
        (!spec.merge_stderr && !CreatePipe(&err_r, &err_w, &sa, 0))) {
        close_h(in_r), close_h(in_w), close_h(out_r), close_h(out_w), close_h(err_r), close_h(err_w);
        r.failed = true;
        r.err = "pipe";
        return r;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    if (err_r) SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = spec.merge_stderr ? out_w : err_w;
    PROCESS_INFORMATION pi{};
    std::wstring wcl = widen(cl);
    std::vector<wchar_t> clbuf(wcl.begin(), wcl.end());
    clbuf.push_back(L'\0');
    const std::wstring wcwd = spec.cwd.empty() ? std::wstring() : spec.cwd.wstring();
    std::vector<wchar_t> envb;
    DWORD flags = CREATE_NO_WINDOW;
    if (!spec.env.empty()) {
        envb = env_block(spec.env);
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }
    BOOL ok = CreateProcessW(nullptr, clbuf.data(), nullptr, nullptr, TRUE, flags,
                             envb.empty() ? nullptr : envb.data(), wcwd.empty() ? nullptr : wcwd.c_str(),
                             &si, &pi);
    close_h(in_r);
    close_h(out_w);
    close_h(err_w);
    if (!ok) {
        close_h(in_w), close_h(out_r), close_h(err_r);
        r.failed = true;
        r.err = "CreateProcess failed";
        return r;
    }
    if (!spec.input.empty()) {
        DWORD wr = 0;
        WriteFile(in_w, spec.input.data(), static_cast<DWORD>(spec.input.size()), &wr, nullptr);
    }
    close_h(in_w);
    auto drain = [](HANDLE h, std::string& into) {
        if (!h) return;
        char buf[4096];
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
            DWORD got = 0;
            const DWORD want = std::min<DWORD>(avail, static_cast<DWORD>(sizeof buf));
            if (!ReadFile(h, buf, want, &got, nullptr) || got == 0) return;
            into.append(buf, got);
        }
    };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(std::max(0.1, spec.timeout_s));
    for (;;) {
        drain(out_r, r.out);
        drain(err_r, r.err);
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            r.timed_out = true;
            break;
        }
    }
    drain(out_r, r.out);
    drain(err_r, r.err);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.rc = r.timed_out ? -1 : static_cast<int>(code);
    if (!r.timed_out && code >= 0xC0000000u) r.crashed = true;
    close_h(out_r);
    close_h(err_r);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

#else

RunOut run(const RunSpec& spec) {
    RunOut r;
    const auto& args = spec.argv;
    if (args.empty()) {
        r.failed = true;
        r.err = "no argv";
        return r;
    }
    int in_p[2] = {-1, -1};
    int out_p[2] = {-1, -1};
    int err_p[2] = {-1, -1};
    auto close_fd = [](int& fd) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    };
    auto close_all = [&] {
        close_fd(in_p[0]), close_fd(in_p[1]), close_fd(out_p[0]), close_fd(out_p[1]);
        close_fd(err_p[0]), close_fd(err_p[1]);
    };
    if (::pipe(in_p) != 0 || ::pipe(out_p) != 0 || (!spec.merge_stderr && ::pipe(err_p) != 0)) {
        close_all();
        r.failed = true;
        r.err = "pipe";
        return r;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // The child's environment is built here, before fork: the child only
    // swaps the environ pointer (no allocation after fork in a threaded
    // process). PRISM's own environment is never changed.
    std::vector<std::string> env_strings;
    std::vector<char*> envp;
    if (!spec.env.empty()) {
        for (char** e = environ; e && *e; ++e) {
            std::string_view kv(*e);
            auto eq = kv.find('=');
            auto key = kv.substr(0, eq);
            bool replaced = false;
            for (const auto& [k, v] : spec.env)
                if (key == k) replaced = true;
            if (!replaced) env_strings.emplace_back(kv);
        }
        for (const auto& [k, v] : spec.env) env_strings.push_back(k + "=" + v);
        for (auto& s : env_strings) envp.push_back(s.data());
        envp.push_back(nullptr);
    }
    const std::string cwd = spec.cwd.string();
    pid_t pid = ::fork();
    if (pid < 0) {
        close_all();
        r.failed = true;
        r.err = "fork failed";
        return r;
    }
    if (pid == 0) {
        // a process group of its own: a timeout kills the child's own children
        // too (a compiler driver's cc1/ld, a solver's workers), not just the child
        ::setpgid(0, 0);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);
        sandbox::apply_child_limits(spec.limits);
        ::dup2(in_p[0], STDIN_FILENO);
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(spec.merge_stderr ? out_p[1] : err_p[1], STDERR_FILENO);
        for (int fd : {in_p[0], in_p[1], out_p[0], out_p[1], err_p[0], err_p[1]})
            if (fd > STDERR_FILENO) ::close(fd);
        if (!envp.empty()) environ = envp.data();
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ChildGroup tracked(pid);  // killed with PRISM on SIGINT/SIGTERM
    auto kill_tree = [pid] {
        if (::killpg(pid, SIGKILL) != 0) ::kill(pid, SIGKILL);
    };
    close_fd(in_p[0]);
    close_fd(out_p[1]);
    close_fd(err_p[1]);
    auto set_nb = [](int fd) {
        if (fd < 0) return;
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    };
    set_nb(in_p[1]);
    set_nb(out_p[0]);
    set_nb(err_p[0]);
    std::size_t in_off = 0;
    const std::string& input = spec.input;
    auto pump = [&]() {
        char buf[4096];
        if (in_p[1] >= 0) {
            while (in_off < input.size()) {
                ssize_t n = ::write(in_p[1], input.data() + in_off, input.size() - in_off);
                if (n > 0) {
                    in_off += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EPIPE) in_off = input.size();  // child closed stdin
                break;
            }
            if (in_off >= input.size()) close_fd(in_p[1]);
        }
        // EOF closes our end, so poll() does not spin on a hung-up pipe.
        auto slurp = [&](int& fd, std::string& into) {
            if (fd < 0) return;
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof buf);
                if (n > 0) {
                    into.append(buf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n == 0) close_fd(fd);
                break;
            }
        };
        slurp(out_p[0], r.out);
        slurp(err_p[0], r.err);
    };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(std::max(0.1, spec.timeout_s));
    int st = 0;
    for (;;) {
        pump();
        pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline || (w < 0 && errno != EINTR)) {
            kill_tree();
            r.timed_out = true;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        const auto remain_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfds[3]{};
        nfds_t nfd = 0;
        if (in_p[1] >= 0) pfds[nfd++] = {in_p[1], POLLOUT, 0};
        if (out_p[0] >= 0) pfds[nfd++] = {out_p[0], POLLIN, 0};
        if (err_p[0] >= 0) pfds[nfd++] = {err_p[0], POLLIN, 0};
        (void)::poll(pfds, nfd, static_cast<int>(std::clamp<long long>(remain_ms, 1, 50)));
    }
    close_fd(in_p[1]);
    // The child is gone: read what is left in the pipes without blocking on
    // a grandchild that still holds them open.
    pump();
    close_fd(out_p[0]);
    close_fd(err_p[0]);
    if (WIFEXITED(st)) {
        r.rc = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        r.rc = -WTERMSIG(st);
        if (!r.timed_out) r.crashed = true;
    } else {
        r.rc = st;
    }
    return r;
}

#endif

ProcOut run_process(const std::vector<std::string>& args, double timeout_s, const fs::path& cwd) {
    RunSpec spec;
    spec.argv = args;
    spec.timeout_s = timeout_s;
    spec.cwd = cwd;
    spec.merge_stderr = true;
    auto r = run(spec);
    return {std::move(r.out), r.rc, r.timed_out, r.failed};
}

namespace {
// Registered child process groups. A fixed array of atomics: the signal
// handler reads it without locks or allocation. More children than slots only
// means the extra ones are not killed by the handler (their own runner still
// kills them on timeout).
constexpr int kChildSlots = 512;
std::atomic<int> g_child_groups[kChildSlots];
}  // namespace

void track_child_group(int pgid) noexcept {
    if (pgid <= 0) return;
    for (auto& slot : g_child_groups) {
        int z = 0;
        if (slot.compare_exchange_strong(z, pgid)) return;
    }
}

void untrack_child_group(int pgid) noexcept {
    if (pgid <= 0) return;
    for (auto& slot : g_child_groups) {
        int v = pgid;
        if (slot.compare_exchange_strong(v, 0)) return;
    }
}

void kill_child_groups() noexcept {
#ifndef _WIN32
    for (auto& slot : g_child_groups) {
        const int g = slot.load();
        if (g > 0) ::kill(-g, SIGKILL);
    }
#endif
}

}  // namespace prism::detail

#ifndef _WIN32
extern "C" void prism_child_cleanup_handler(int sig) {
    const int saved = errno;
    prism::detail::kill_child_groups();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
    errno = saved;
}
#endif

void prism::detail::install_child_cleanup() noexcept {
#ifndef _WIN32
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
        struct sigaction old {};
        if (::sigaction(sig, nullptr, &old) != 0 || old.sa_handler == SIG_IGN) continue;
        struct sigaction sa {};
        sa.sa_handler = prism_child_cleanup_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(sig, &sa, nullptr);
    }
#endif
}
