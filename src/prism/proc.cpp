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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <cwchar>
#  include <cwctype>
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
#  include <pthread.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
extern char** environ;
#endif

namespace prism::detail {
namespace fs = std::filesystem;

void refuse_disabled_checks(const std::vector<std::string>& argv) {
    for (const auto& flag : argv) {
        if (flag.starts_with("--no-") && flag.ends_with("-check"))
            throw std::runtime_error("refusing to disable a check: " + flag);
    }
}

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
    refuse_disabled_checks(spec.argv);
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
    HANDLE stdout_file = nullptr;
    HANDLE stdin_file = nullptr;
    auto close_h = [](HANDLE& h) {
        if (h) CloseHandle(h);
        h = nullptr;
    };
    const bool capture_stdout = spec.stdout_path.empty();
    const bool stdin_from_file = spec.input.empty() && !spec.stdin_path.empty();
    if ((!stdin_from_file && !CreatePipe(&in_r, &in_w, &sa, 0)) ||
        (capture_stdout && !CreatePipe(&out_r, &out_w, &sa, 0)) ||
        (!spec.merge_stderr && !CreatePipe(&err_r, &err_w, &sa, 0))) {
        close_h(in_r), close_h(in_w), close_h(out_r), close_h(out_w), close_h(err_r), close_h(err_w);
        close_h(stdout_file);
        close_h(stdin_file);
        r.failed = true;
        r.err = "pipe";
        return r;
    }
    if (stdin_from_file) {
        stdin_file = CreateFileW(spec.stdin_path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!stdin_file || stdin_file == INVALID_HANDLE_VALUE) {
            close_h(in_r), close_h(in_w), close_h(out_r), close_h(out_w), close_h(err_r), close_h(err_w);
            close_h(stdout_file);
            close_h(stdin_file);
            r.failed = true;
            r.err = "cannot open stdin file";
            return r;
        }
        in_r = stdin_file;
    }
    if (!capture_stdout) {
        stdout_file = CreateFileW(spec.stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!stdout_file || stdout_file == INVALID_HANDLE_VALUE) {
            close_h(in_r), close_h(in_w), close_h(out_r), close_h(out_w), close_h(err_r), close_h(err_w);
            close_h(stdout_file);
            close_h(stdin_file);
            r.failed = true;
            r.err = "cannot open stdout file";
            return r;
        }
        out_w = stdout_file;
    }
    if (in_w) SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    if (out_r) SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    if (err_r) SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    auto fail_start = [&](const char* why) {
        // stdin_file / stdout_file are the same handles as in_r / out_w
        close_h(in_r), close_h(in_w), close_h(out_r), close_h(out_w), close_h(err_r), close_h(err_w);
        stdin_file = nullptr;
        stdout_file = nullptr;
        r.failed = true;
        r.rc = 127;
        r.err = why;
        return r;
    };
    // The child inherits exactly its three standard handles (an explicit
    // handle list), never the other pipe ends of runs going on in other
    // threads: a stray copy of a write end would keep a pipe open and hang
    // a reader after its child is gone.
    HANDLE inherit[3];
    DWORD n_inherit = 0;
    for (HANDLE h : {in_r, out_w, spec.merge_stderr ? out_w : err_w}) {
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        if (std::find(inherit, inherit + n_inherit, h) == inherit + n_inherit) inherit[n_inherit++] = h;
    }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (attr_size == 0 || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size))
        return fail_start("cannot build the handle inheritance list (InitializeProcThreadAttributeList)");
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attr_guard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   n_inherit * sizeof(HANDLE), nullptr, nullptr))
        return fail_start("cannot build the handle inheritance list (UpdateProcThreadAttribute)");
    // A Job Object holds the whole process tree: a timeout, a stop request or
    // PRISM dying ends every descendant, not just the child (the Windows
    // counterpart of a POSIX process group / session).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return fail_start("cannot create a Job Object for the process tree");
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (spec.mem_limit_mb > 0) {
            li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            li.ProcessMemoryLimit = static_cast<SIZE_T>(spec.mem_limit_mb) << 20;
        }
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
            CloseHandle(job);
            return fail_start("cannot configure the Job Object for the process tree");
        }
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = spec.merge_stderr ? out_w : err_w;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring wcl = widen(cl);
    std::vector<wchar_t> clbuf(wcl.begin(), wcl.end());
    clbuf.push_back(L'\0');
    const std::wstring wcwd = spec.cwd.empty() ? std::wstring() : spec.cwd.wstring();
    std::vector<wchar_t> envb;
    // Suspended until it is in the job, so it cannot start a descendant outside it.
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (!spec.env.empty()) {
        envb = env_block(spec.env);
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }
    BOOL ok = CreateProcessW(nullptr, clbuf.data(), nullptr, nullptr, TRUE, flags,
                             envb.empty() ? nullptr : envb.data(), wcwd.empty() ? nullptr : wcwd.c_str(),
                             &si.StartupInfo, &pi);
    close_h(in_r);
    if (stdin_file) stdin_file = nullptr;  // in_r was the same handle
    close_h(out_w);
    if (stdout_file) stdout_file = nullptr;  // out_w was the same handle
    close_h(err_w);
    if (!ok) {
        CloseHandle(job);
        close_h(in_w), close_h(out_r), close_h(err_r);
        r.failed = true;
        r.rc = 127;  // same as a failed exec on POSIX (see run() in proc.hpp)
        r.err = "CreateProcess failed";
        return r;
    }
    // A child that cannot be put in the job would escape its timeout: that is
    // a start failure, not a run without a kill switch (Laws 1 and 7).
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        close_h(in_w), close_h(out_r), close_h(err_r);
        r.failed = true;
        r.rc = 127;
        r.err = "cannot assign the child to a Job Object";
        return r;
    }
    ResumeThread(pi.hThread);
    // Stdin is written from a thread of its own: a WriteFile larger than the
    // pipe buffer blocks until the child reads, and the child may be blocked
    // on a full stdout pipe that only the loop below drains. The writer ends
    // when the input is written or the child is gone (broken pipe).
    std::thread writer;
    if (!spec.input.empty()) {
        writer = std::thread([h = in_w, &input = spec.input] {
            std::size_t off = 0;
            while (off < input.size()) {
                DWORD wr = 0;
                const DWORD want = static_cast<DWORD>(std::min<std::size_t>(input.size() - off, 1u << 16));
                if (!WriteFile(h, input.data() + off, want, &wr, nullptr) || wr == 0) break;
                off += wr;
            }
            CloseHandle(h);
        });
        in_w = nullptr;  // owned by the writer
    } else {
        close_h(in_w);
    }
    const std::size_t cap = spec.max_output_bytes;
    auto drain = [&](HANDLE h, std::string& into) {
        if (!h) return;
        char buf[4096];
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
            DWORD got = 0;
            const DWORD want = std::min<DWORD>(avail, static_cast<DWORD>(sizeof buf));
            if (!ReadFile(h, buf, want, &got, nullptr) || got == 0) return;
            if (cap == 0 || into.size() < cap) {
                const std::size_t room = cap == 0 ? got : cap - into.size();
                into.append(buf, std::min<std::size_t>(got, room));
            }
        }
    };
    const bool limit_time = spec.timeout_s > 0;
    const auto deadline =
        limit_time ? std::chrono::steady_clock::now() + std::chrono::duration<double>(spec.timeout_s)
                   : std::chrono::steady_clock::time_point::max();
    // Resident-memory watchdog (Linux: /proc/<pid>/statm). Sampled about every
    // 100 ms; a child whose working set cannot be read is not capped, which
    // peak_rss == 0 shows to the caller (it is never reported as under the cap).
    auto next_rss = std::chrono::steady_clock::now();
    auto over_rss = [&]() -> bool {
        if (spec.rss_cap_bytes == 0) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_rss) return false;
        next_rss = now + std::chrono::milliseconds(100);
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof pmc;
        if (!K32GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof pmc)) return false;
        r.peak_rss = std::max<std::uint64_t>(r.peak_rss, pmc.WorkingSetSize);
        return pmc.WorkingSetSize > spec.rss_cap_bytes;
    };
    for (;;) {
        drain(out_r, r.out);
        drain(err_r, r.err);
        if (over_rss()) {
            TerminateJobObject(job, 1);  // the child and every descendant
            WaitForSingleObject(pi.hProcess, 2000);
            r.rss_exceeded = true;
            break;
        }
        if (spec.stop && spec.stop->load()) {
            TerminateJobObject(job, 1);  // the child and every descendant
            WaitForSingleObject(pi.hProcess, 2000);
            r.cancelled = true;
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) break;
        if (limit_time && std::chrono::steady_clock::now() >= deadline) {
            TerminateJobObject(job, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            r.timed_out = true;
            break;
        }
    }
    if (spec.kill_descendants) {
        TerminateJobObject(job, 1);  // whatever the child left running
    } else {
        // Closing the job below must not kill a descendant the child left
        // behind on purpose (as on POSIX, where only a timeout kills the group).
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li, nullptr);
        li.BasicLimitInformation.LimitFlags &= ~static_cast<DWORD>(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    CloseHandle(job);
    drain(out_r, r.out);
    drain(err_r, r.err);
    // A pending WriteFile normally fails once the child is gone, but a
    // descendant left running (or a crashed ASan child whose handles linger)
    // can keep the read end open and block the writer for ever. Cancel its
    // synchronous I/O until it ends, so the join is bounded.
    if (writer.joinable()) {
        HANDLE wt = static_cast<HANDLE>(writer.native_handle());
        while (WaitForSingleObject(wt, 50) == WAIT_TIMEOUT) CancelSynchronousIo(wt);
        writer.join();
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.rc = r.timed_out ? -1 : static_cast<int>(code);
    if (!r.timed_out && code >= 0xC0000000u) r.crashed = true;
    close_h(out_r);
    close_h(err_r);
    close_h(stdout_file);
    close_h(stdin_file);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

#else

RunOut run(const RunSpec& spec) {
    refuse_disabled_checks(spec.argv);
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
    // exec_p reports why the child could not start: the child writes errno
    // and the stage ("chdir" or "exec") before _exit(127); CLOEXEC closes it
    // on a successful exec, so the parent reads EOF. It is created CLOEXEC
    // atomically where the OS can: a child forked meanwhile by another --jobs
    // thread must not keep the write end open (the read would wait for it).
    int exec_p[2] = {-1, -1};
    auto cloexec_pipe = [](int fds[2]) {
#if defined(__APPLE__)
        if (::pipe(fds) != 0) return false;
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        return true;
#else
        return ::pipe2(fds, O_CLOEXEC) == 0;
#endif
    };
    // Every pipe is close-on-exec: runs started from several threads at once
    // (parallel sanitize jobs, --jobs) must not inherit each other's pipe
    // ends, or a reader waits for an EOF another child holds back. dup2 onto
    // 0/1/2 clears the flag on the copies the child uses.
    if (!cloexec_pipe(in_p) || !cloexec_pipe(out_p) || (!spec.merge_stderr && !cloexec_pipe(err_p)) ||
        !cloexec_pipe(exec_p)) {
        close_fd(exec_p[0]), close_fd(exec_p[1]);
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
        close_fd(exec_p[0]), close_fd(exec_p[1]);
        close_all();
        r.failed = true;
        r.err = "fork failed";
        return r;
    }
    if (pid == 0) {
        // a process group of its own: a timeout kills the child's own children
        // too (a compiler driver's cc1/ld, a solver's workers), not just the child
        ::setpgid(0, 0);
        // async-signal-safe only: a raw write of {stage, errno}
        auto die = [&](int stage) {
            int msg[2] = {stage, errno};
            (void)!::write(exec_p[1], msg, sizeof msg);
            ::_exit(127);
        };
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) die(1);
        sandbox::apply_child_limits(spec.limits);
        // dup2 onto itself keeps FD_CLOEXEC: clear it by hand then.
        auto onto = [](int from, int to) {
            if (from == to) ::fcntl(to, F_SETFD, 0);
            else ::dup2(from, to);
        };
        onto(in_p[0], STDIN_FILENO);
        onto(out_p[1], STDOUT_FILENO);
        onto(spec.merge_stderr ? out_p[1] : err_p[1], STDERR_FILENO);
        for (int fd : {in_p[0], in_p[1], out_p[0], out_p[1], err_p[0], err_p[1], exec_p[0]})
            if (fd > STDERR_FILENO) ::close(fd);
        if (!envp.empty()) environ = envp.data();
        ::execvp(argv.at(0), argv.data());
        die(2);
    }
    close_fd(exec_p[1]);
    int start_err[2] = {0, 0};
    ssize_t got = 0;
    do {
        got = ::read(exec_p[0], start_err, sizeof start_err);
    } while (got < 0 && errno == EINTR);
    close_fd(exec_p[0]);
    if (got == static_cast<ssize_t>(sizeof start_err)) {
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        close_all();
        r.failed = true;
        r.rc = 127;
        r.err = (start_err[0] == 1 ? "cannot enter " + cwd : "cannot execute " + args[0]) + ": " +
                std::strerror(start_err[1]);
        return r;
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
            // A child that exits without reading its stdin makes the write
            // raise SIGPIPE, whose default action would kill PRISM. SIGPIPE is
            // blocked on this thread for the write (not ignored process-wide:
            // children would inherit SIG_IGN), so the write fails with EPIPE
            // instead; a SIGPIPE this write left pending is consumed.
            sigset_t pipe_set, old_mask, pending;
            sigemptyset(&pipe_set);
            sigaddset(&pipe_set, SIGPIPE);
            ::pthread_sigmask(SIG_BLOCK, &pipe_set, &old_mask);
            sigemptyset(&pending);
            ::sigpending(&pending);
            const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
            bool broke = false;
            while (in_off < input.size()) {
                ssize_t n = ::write(in_p[1], input.data() + in_off, input.size() - in_off);
                if (n > 0) {
                    in_off += static_cast<std::size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EPIPE) {
                    in_off = input.size();  // child closed stdin
                    broke = true;
                }
                break;
            }
            if (broke && !was_pending) {
                const timespec zero{0, 0};
                sigemptyset(&pending);
                ::sigpending(&pending);
                if (sigismember(&pending, SIGPIPE) == 1) (void)::sigtimedwait(&pipe_set, nullptr, &zero);
            }
            ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
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
        // bwrap (the sandbox wrapper) runs the command as a child and exits
        // 128 + N when that child is killed by signal N, so a crash inside
        // the sandbox would otherwise read as a plain nonzero exit.
        if (!r.timed_out && !args.empty() && fs::path(args[0]).filename() == "bwrap" && r.rc > 128 &&
            r.rc < 128 + 65) {
            r.rc = -(r.rc - 128);
            r.crashed = true;
        }
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
    std::string text = std::move(r.out);
    if (!r.err.empty()) {
        if (!text.empty()) text += '\n';
        text += r.err;
    }
    return {std::move(text), r.rc, r.timed_out, r.failed};
}

namespace {
// Registered child process groups. A fixed array of atomics: the signal
// handler reads it without locks or allocation. More children than slots only
// means the extra ones are not killed by the handler (their own runner still
// kills them on timeout).
constexpr int kChildSlots = 512;
std::atomic<int> g_child_groups[kChildSlots];
std::atomic<int> g_child_sessions[kChildSlots];

void track_in(std::atomic<int> (&slots)[kChildSlots], int id) noexcept {
    if (id <= 0) return;
    for (auto& slot : slots) {
        int z = 0;
        if (slot.compare_exchange_strong(z, id)) return;
    }
}

void untrack_in(std::atomic<int> (&slots)[kChildSlots], int id) noexcept {
    if (id <= 0) return;
    for (auto& slot : slots) {
        int v = id;
        if (slot.compare_exchange_strong(v, 0)) return;
    }
}

#if defined(__linux__)
// Session id (field 6 of /proc/PID/stat) of the process named by the decimal
// string name; -1 when unreadable. open/read/close only: async-signal-safe.
int stat_session(const char* name) noexcept {
    char path[64] = "/proc/";
    std::size_t n = 6;
    for (const char* p = name; *p && n + 6 < sizeof path; ++p) path[n++] = *p;
    std::memcpy(path + n, "/stat", 6);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[1024];
    ssize_t len = 0;
    for (;;) {
        len = ::read(fd, buf, sizeof buf - 1);
        if (len >= 0 || errno != EINTR) break;
    }
    ::close(fd);
    if (len <= 0) return -1;
    buf[len] = '\0';
    // comm (field 2) may contain blanks and ')': fields restart after the last ')'
    const char* close = nullptr;
    for (ssize_t i = 0; i < len; ++i)
        if (buf[i] == ')') close = buf + i;
    if (!close || close[1] != ' ' || close[2] == 'Z') return -1;  // zombies need no kill
    const char* p = close + 1;
    for (int field = 0; field < 3; ++field) {  // state, ppid, pgrp
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
    }
    while (*p == ' ') ++p;
    int sid = 0;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; ++p) {
        sid = sid * 10 + (*p - '0');
        any = true;
    }
    return any ? sid : -1;
}

struct LinuxDirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

// SIGKILL every process whose session id is sid, except the caller. Returns
// how many were signalled. getdents64 + stat_session: async-signal-safe.
int kill_session_members_raw(int sid) noexcept {
    const int dfd = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return 0;
    const pid_t self = ::getpid();
    int hit = 0;
    alignas(8) char buf[8192];
    for (;;) {
        const long got = ::syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (got <= 0) break;
        for (long off = 0; off < got;) {
            auto* d = reinterpret_cast<LinuxDirent64*>(buf + off);
            off += d->d_reclen;
            const char* name = d->d_name;
            if (*name < '0' || *name > '9') continue;
            int pid = 0;
            for (const char* p = name; *p >= '0' && *p <= '9'; ++p) pid = pid * 10 + (*p - '0');
            if (pid <= 1 || pid == self) continue;
            if (stat_session(name) == sid) {
                ::kill(pid, SIGKILL);
                ++hit;
            }
        }
    }
    ::close(dfd);
    return hit;
}
#endif
}  // namespace

void track_child_group(int pgid) noexcept { track_in(g_child_groups, pgid); }
void untrack_child_group(int pgid) noexcept { untrack_in(g_child_groups, pgid); }
void track_child_session(int sid) noexcept { track_in(g_child_sessions, sid); }
void untrack_child_session(int sid) noexcept { untrack_in(g_child_sessions, sid); }

void kill_session_now(int sid) noexcept {
#ifndef _WIN32
    if (sid <= 1) return;
    ::kill(-sid, SIGKILL);
#  if defined(__linux__)
    // members that moved to process groups of their own (PRISM's solver children)
    for (int round = 0; round < 3; ++round)
        if (kill_session_members_raw(sid) == 0) break;
#  endif
    ::kill(sid, SIGKILL);
#else
    (void)sid;
#endif
}

void kill_child_groups() noexcept {
#ifndef _WIN32
    for (auto& slot : g_child_groups) {
        const int g = slot.load();
        if (g > 0) ::kill(-g, SIGKILL);
    }
    for (auto& slot : g_child_sessions) {
        const int s = slot.load();
        if (s > 0) kill_session_now(s);
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
