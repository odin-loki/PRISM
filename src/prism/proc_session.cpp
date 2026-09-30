// run_session: a child in a session of its own, killed with every process of
// that session on a timeout or a signal to this process (proc.hpp).

#include "proc.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace prism::detail {

namespace {
// Registered sessions, read by the signal handler without locks (as the
// process groups in adapters.cpp).
constexpr int kSessionSlots = 256;
std::atomic<int> g_child_sessions[kSessionSlots];

#ifndef _WIN32
// Session id of /proc/<pid>/stat, parsed without allocation; -1 when unreadable.
long stat_session(const char* pid_name) noexcept {
    char path[64] = "/proc/";
    std::size_t n = 6;
    for (const char* p = pid_name; *p && n + 6 < sizeof path; ++p) path[n++] = *p;
    std::memcpy(path + n, "/stat", 6);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[1024];
    ssize_t got = 0;
    for (;;) {
        got = ::read(fd, buf, sizeof buf - 1);
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fd);
    if (got <= 0) return -1;
    buf[got] = '\0';
    // comm (field 2) may contain blanks and ')': fields restart after the last ')'
    char* rp = nullptr;
    for (char* p = buf; *p; ++p)
        if (*p == ')') rp = p;
    if (!rp) return -1;
    // after ") ": state ppid pgrp session
    const char* p = rp + 1;
    for (int field = 0; field < 4; ++field) {
        while (*p == ' ') ++p;
        if (field == 3) break;
        while (*p && *p != ' ') ++p;
    }
    if (*p < '0' || *p > '9') return -1;
    long v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
    return v;
}

bool all_digits(const char* s) noexcept {
    if (!*s) return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9') return false;
    return true;
}

// Calls fn(pid) for every process whose session id is `sid`. Raw getdents64
// on Linux (async-signal-safe); nothing elsewhere.
template <class Fn>
void for_each_member(int sid, Fn&& fn) noexcept {
#ifdef __linux__
    const int dfd = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    alignas(8) char buf[8192];
    for (;;) {
        const long n = ::syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (n <= 0) break;
        for (long off = 0; off < n;) {
            // struct linux_dirent64: ino (8), off (8), reclen (2), type (1), name
            unsigned short reclen = 0;
            std::memcpy(&reclen, buf + off + 16, sizeof reclen);
            const char* name = buf + off + 19;
            if (reclen == 0) break;
            off += reclen;
            if (!all_digits(name)) continue;
            if (stat_session(name) != sid) continue;
            long pid = 0;
            for (const char* p = name; *p; ++p) pid = pid * 10 + (*p - '0');
            fn(static_cast<int>(pid));
        }
    }
    ::close(dfd);
#else
    (void)sid;
    (void)fn;
#endif
}

std::string resolve_exe(const std::string& name) {
    if (name.find('/') != std::string::npos) return name;
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    std::size_t a = 0;
    while (a <= dirs.size()) {
        auto b = dirs.find(':', a);
        if (b == std::string::npos) b = dirs.size();
        std::string d = dirs.substr(a, b - a);
        if (d.empty()) d = ".";
        const std::string cand = d + "/" + name;
        if (::access(cand.c_str(), X_OK) == 0) {
            struct stat st {};
            if (::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return cand;
        }
        a = b + 1;
    }
    return name;  // execve fails: rc 127
}

void set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
#endif
}  // namespace

std::vector<int> session_members(int sid) {
    std::vector<int> out;
#ifndef _WIN32
    // collected first, then copied: for_each_member must not allocate
    int buf[4096];
    std::size_t n = 0;
    for_each_member(sid, [&](int pid) {
        if (n < sizeof buf / sizeof buf[0]) buf[n++] = pid;
    });
    out.assign(buf, buf + n);
#else
    (void)sid;
#endif
    return out;
}

void kill_session(int sid) noexcept {
#ifndef _WIN32
    if (sid <= 1) return;
    ::killpg(sid, SIGKILL);
    // members that moved to process groups of their own (PRISM's solver children)
    const int self = static_cast<int>(::getpid());
    for (int round = 0; round < 3; ++round) {
        bool any = false;
        for_each_member(sid, [&](int pid) {
            if (pid == self) return;
            any = true;
            ::kill(pid, SIGKILL);
        });
        if (!any) break;
    }
#else
    (void)sid;
#endif
}

void track_child_session(int sid) noexcept {
    if (sid <= 0) return;
    for (auto& slot : g_child_sessions) {
        int z = 0;
        if (slot.compare_exchange_strong(z, sid)) return;
    }
}

void untrack_child_session(int sid) noexcept {
    if (sid <= 0) return;
    for (auto& slot : g_child_sessions) {
        int v = sid;
        if (slot.compare_exchange_strong(v, 0)) return;
    }
}

void kill_child_sessions() noexcept {
    for (auto& slot : g_child_sessions) {
        const int s = slot.load();
        if (s > 0) kill_session(s);
    }
}

SessionResult run_session(const std::vector<std::string>& args, const SessionOptions& opt) {
    SessionResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
#ifdef _WIN32
    (void)args;
    (void)opt;
    r.failed = true;
    r.err = "run_session: POSIX sessions only";
    return r;
#else
    if (args.empty()) {
        r.failed = true;
        return r;
    }
    // everything the child needs is built before fork (no allocation after it)
    const std::string exe = resolve_exe(args[0]);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> env_store;
    for (char** e = environ; e && *e; ++e) {
        std::string_view kv(*e);
        const auto eq = kv.find('=');
        const auto key = kv.substr(0, eq);
        bool replaced = false;
        for (const auto& [k, v] : opt.env)
            if (k == key) replaced = true;
        if (!replaced) env_store.emplace_back(kv);
    }
    for (const auto& [k, v] : opt.env) env_store.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);
    const std::string cwd = opt.cwd.string();

    int out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, in_p[2] = {-1, -1};
    auto close_all = [&] {
        for (int* p : {out_p, err_p, in_p})
            for (int i = 0; i < 2; ++i)
                if (p[i] >= 0) {
                    ::close(p[i]);
                    p[i] = -1;
                }
    };
    if (::pipe(out_p) != 0 || ::pipe(err_p) != 0 || (opt.input && ::pipe(in_p) != 0)) {
        close_all();
        r.failed = true;
        return r;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        close_all();
        r.failed = true;
        return r;
    }
    if (pid == 0) {
        ::setsid();
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);
        if (opt.rlimit_as_mb > 0) {
            struct rlimit rl {};
            rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(opt.rlimit_as_mb) * 1024 * 1024;
            ::setrlimit(RLIMIT_AS, &rl);
        }
        if (opt.input) {
            ::dup2(in_p[0], STDIN_FILENO);
        } else {
            const int dn = ::open("/dev/null", O_RDONLY);
            if (dn >= 0) {
                ::dup2(dn, STDIN_FILENO);
                ::close(dn);
            }
        }
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(err_p[1], STDERR_FILENO);
        for (int fd : {out_p[0], out_p[1], err_p[0], err_p[1], in_p[0], in_p[1]})
            if (fd > STDERR_FILENO) ::close(fd);
        // a handler this process installed must not run in the child
        for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) ::signal(sig, SIG_DFL);
        ::execve(exe.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    track_child_session(pid);  // killed with this process on SIGINT/SIGTERM
    ::close(out_p[1]);
    out_p[1] = -1;
    ::close(err_p[1]);
    err_p[1] = -1;
    if (opt.input) {
        ::close(in_p[0]);
        in_p[0] = -1;
        set_nonblock(in_p[1]);
    }
    set_nonblock(out_p[0]);
    set_nonblock(err_p[0]);
    std::size_t in_off = 0;
    if (opt.input && opt.input->empty()) {
        ::close(in_p[1]);
        in_p[1] = -1;
    }
    const bool limited = opt.timeout_s > 0;
    const auto deadline = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(limited ? opt.timeout_s : 0.0));
    int status = 0;
    bool exited = false;
    char buf[65536];
    auto pump = [&](int wait_ms) {
        pollfd fds[3];
        int nf = 0;
        if (out_p[0] >= 0) fds[nf++] = {out_p[0], POLLIN, 0};
        if (err_p[0] >= 0) fds[nf++] = {err_p[0], POLLIN, 0};
        if (in_p[1] >= 0) fds[nf++] = {in_p[1], POLLOUT, 0};
        if (nf == 0) return;
        const int pr = ::poll(fds, static_cast<nfds_t>(nf), wait_ms);
        if (pr <= 0) return;
        for (int i = 0; i < nf; ++i) {
            if (!fds[i].revents) continue;
            const int fd = fds[i].fd;
            if (fd == in_p[1]) {
                if (fds[i].revents & (POLLERR | POLLHUP)) {
                    ::close(in_p[1]);
                    in_p[1] = -1;
                    continue;
                }
                const ssize_t w = ::write(fd, opt.input->data() + in_off, opt.input->size() - in_off);
                if (w > 0) in_off += static_cast<std::size_t>(w);
                if ((w < 0 && errno != EAGAIN && errno != EINTR) || in_off >= opt.input->size()) {
                    ::close(in_p[1]);
                    in_p[1] = -1;
                }
                continue;
            }
            std::string& dst = fd == out_p[0] ? r.out : r.err;
            int& own = fd == out_p[0] ? out_p[0] : err_p[0];
            for (;;) {
                const ssize_t n = ::read(fd, buf, sizeof buf);
                if (n > 0) {
                    dst.append(buf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    ::close(own);
                    own = -1;
                }
                break;
            }
        }
    };
    for (;;) {
        if (!exited) {
            // not reaped yet (WNOWAIT): the pid stays the session's while
            // the members left behind are killed below
            siginfo_t si{};
            if (::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                si.si_pid == pid)
                exited = true;
        }
        if (exited && out_p[0] < 0 && err_p[0] < 0) break;
        if (limited && std::chrono::steady_clock::now() >= deadline) {
            r.timed_out = true;
            break;
        }
        pump(20);
    }
    // Timeout: everything in the session dies. Normal exit: the leader is
    // gone and every pipe is closed; members left behind (a daemonised
    // grandchild with its output redirected) die with it. Either way the
    // leader is still unreaped, so its pid cannot name another session yet.
    kill_session(pid);
    if (r.timed_out) {
        // a descendant outside the session could still hold a pipe open: bounded drain
        const auto drain_end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((out_p[0] >= 0 || err_p[0] >= 0) && std::chrono::steady_clock::now() < drain_end) pump(20);
    }
    // SIGKILL cannot be ignored: the leader is reaped promptly
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    untrack_child_session(pid);
    close_all();
    if (r.timed_out) r.rc = -1;
    else if (WIFEXITED(status)) r.rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.rc = -WTERMSIG(status);
    r.seconds = elapsed();
    return r;
#endif
}

}  // namespace prism::detail
