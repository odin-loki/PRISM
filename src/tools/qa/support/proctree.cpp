#include "proctree.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace prism::qa {

namespace {

// Sessions currently running (lock-free: read by the signal handler).
constexpr int kMaxSessions = 256;
std::atomic<int> g_sessions[kMaxSessions];

void track(int sid) noexcept {
    for (auto& s : g_sessions) {
        int z = 0;
        if (s.compare_exchange_strong(z, sid)) return;
    }
}

void untrack(int sid) noexcept {
    for (auto& s : g_sessions) {
        int v = sid;
        if (s.compare_exchange_strong(v, 0)) return;
    }
}

// Async-signal-safe /proc scan: calls fn(pid) for each process whose session
// id is sid. No allocation (getdents64 + read into stack buffers).
template <class F>
void for_session_members(int sid, F&& fn) noexcept {
#ifdef __linux__
    int dfd = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    alignas(8) char buf[8192];
    for (;;) {
        long n = ::syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (n <= 0) break;
        for (long off = 0; off < n;) {
            struct Ent {
                unsigned long long ino;
                long long off;
                unsigned short reclen;
                unsigned char type;
                char name[1];
            };
            auto* e = reinterpret_cast<Ent*>(buf + off);
            off += e->reclen;
            const char* name = e->name;
            int pid = 0;
            bool digits = name[0] != 0;
            for (const char* p = name; *p; ++p) {
                if (*p < '0' || *p > '9') {
                    digits = false;
                    break;
                }
                pid = pid * 10 + (*p - '0');
            }
            if (!digits) continue;
            char path[64] = "/proc/";
            std::size_t pl = 6;
            for (const char* p = name; *p && pl < 50; ++p) path[pl++] = *p;
            const char* tail = "/stat";
            for (const char* p = tail; *p; ++p) path[pl++] = *p;
            path[pl] = 0;
            int fd = ::open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char st[1024];
            ssize_t got = ::read(fd, st, sizeof st - 1);
            ::close(fd);
            if (got <= 0) continue;
            st[got] = 0;
            // comm (field 2) may contain blanks and ')': fields after the last ')'
            char* rp = nullptr;
            for (char* p = st; *p; ++p)
                if (*p == ')') rp = p;
            if (!rp) continue;
            // rest: state ppid pgrp session
            char* p = rp + 1;
            int field = 0;
            long session = -1;
            while (*p && field < 4) {
                while (*p == ' ') ++p;
                char* start = p;
                while (*p && *p != ' ') ++p;
                ++field;
                if (field == 4) {
                    long v = 0;
                    bool neg = false;
                    char* q = start;
                    if (*q == '-') {
                        neg = true;
                        ++q;
                    }
                    for (; q < p; ++q) v = v * 10 + (*q - '0');
                    session = neg ? -v : v;
                }
            }
            if (session == sid) fn(pid);
        }
    }
    ::close(dfd);
#else
    (void)sid;
    (void)fn;
#endif
}

void on_signal(int sig) {
    for (auto& s : g_sessions) {
        int sid = s.load();
        if (sid > 0) kill_session(sid);
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}  // namespace

std::vector<int> session_members(int sid) {
    std::vector<int> out;
    for_session_members(sid, [&](int pid) { out.push_back(pid); });
    return out;
}

void kill_session(int sid) noexcept {
    if (sid <= 0) return;
    ::killpg(sid, SIGKILL);
    // members that moved to process groups of their own (PRISM's solver children)
    const int self = static_cast<int>(::getpid());
    for (int round = 0; round < 3; ++round) {
        bool any = false;
        for_session_members(sid, [&](int pid) {
            if (pid == self) return;
            any = true;
            ::kill(pid, SIGKILL);
        });
        if (!any) break;
    }
    ::kill(sid, SIGKILL);
}

void install_interrupt_cleanup() noexcept {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
        struct sigaction old {};
        ::sigaction(sig, nullptr, &old);
        if (old.sa_handler == SIG_IGN) continue;
        ::sigaction(sig, &sa, nullptr);
    }
}

std::string which(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    std::size_t a = 0;
    while (a <= p.size()) {
        std::size_t b = p.find(':', a);
        if (b == std::string::npos) b = p.size();
        std::string dir = p.substr(a, b - a);
        if (dir.empty()) dir = ".";
        std::string cand = dir + "/" + name;
        struct stat st {};
        if (::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(cand.c_str(), X_OK) == 0) return cand;
        a = b + 1;
    }
    return {};
}

RunResult run_tree(const std::vector<std::string>& args, const RunOpts& opts) {
    RunResult r;
    if (args.empty()) {
        r.start_failed = true;
        r.err = "no argv";
        return r;
    }
    // everything the child needs is built before fork (the scorer is threaded)
    std::vector<std::string> env_store;
    for (char** e = environ; e && *e; ++e) {
        std::string kv = *e;
        std::string k = kv.substr(0, kv.find('='));
        bool over = false;
        for (const auto& [ok, ov] : opts.env)
            if (ok == k) over = true;
        if (!over) env_store.push_back(std::move(kv));
    }
    for (const auto& [k, v] : opts.env) env_store.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> argv_store(args);
    std::vector<char*> argv;
    for (auto& s : argv_store) argv.push_back(s.data());
    argv.push_back(nullptr);
    std::string cwd = opts.cwd.empty() ? std::string() : opts.cwd.string();
    const rlim_t lim = opts.mem_limit_mb > 0 ? static_cast<rlim_t>(opts.mem_limit_mb) << 20 : 0;

    int out_p[2], err_p[2], exec_p[2];
    if (::pipe2(out_p, O_CLOEXEC) != 0) {
        r.start_failed = true;
        r.err = std::strerror(errno);
        return r;
    }
    if (::pipe2(err_p, O_CLOEXEC) != 0) {
        ::close(out_p[0]);
        ::close(out_p[1]);
        r.start_failed = true;
        r.err = std::strerror(errno);
        return r;
    }
    if (::pipe2(exec_p, O_CLOEXEC) != 0) {
        for (int fd : {out_p[0], out_p[1], err_p[0], err_p[1]}) ::close(fd);
        r.start_failed = true;
        r.err = std::strerror(errno);
        return r;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_p[0], out_p[1], err_p[0], err_p[1], exec_p[0], exec_p[1]}) ::close(fd);
        r.start_failed = true;
        r.err = std::strerror(errno);
        return r;
    }
    if (pid == 0) {
        ::setsid();
        if (lim) {
            struct rlimit rl {lim, lim};
            ::setrlimit(RLIMIT_AS, &rl);
        }
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(err_p[1], STDERR_FILENO);
        int e = 0;
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            e = errno;
        } else {
            ::execvpe(argv[0], argv.data(), envp.data());
            e = errno;
        }
        ssize_t w = ::write(exec_p[1], &e, sizeof e);
        (void)w;
        ::_exit(127);
    }
    track(pid);
    ::close(out_p[1]);
    ::close(err_p[1]);
    ::close(exec_p[1]);
    int child_errno = 0;
    ssize_t n = ::read(exec_p[0], &child_errno, sizeof child_errno);
    ::close(exec_p[0]);
    if (n == static_cast<ssize_t>(sizeof child_errno)) {
        int st = 0;
        ::waitpid(pid, &st, 0);
        untrack(pid);
        ::close(out_p[0]);
        ::close(err_p[0]);
        r.start_failed = true;
        r.err = std::string(args[0]) + ": " + std::strerror(child_errno);
        return r;
    }
    using clock = std::chrono::steady_clock;
    const bool has_deadline = opts.timeout_s > 0;
    auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                       std::chrono::duration<double>(has_deadline ? opts.timeout_s : 0));
    bool out_open = true, err_open = true;
    bool exited = false;
    int status = 0;
    auto read_some = [&](int fd, std::string& dst, bool& open) {
        char buf[65536];
        ssize_t k = ::read(fd, buf, sizeof buf);
        if (k > 0) dst.append(buf, static_cast<std::size_t>(k));
        else if (k == 0 || (errno != EINTR && errno != EAGAIN)) open = false;
    };
    auto pump = [&](int wait_ms) {
        struct pollfd fds[2];
        int nf = 0;
        if (out_open) fds[nf++] = {out_p[0], POLLIN, 0};
        if (err_open) fds[nf++] = {err_p[0], POLLIN, 0};
        if (nf == 0) return;
        int pr = ::poll(fds, static_cast<nfds_t>(nf), wait_ms);
        if (pr <= 0) return;
        for (int i = 0; i < nf; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (fds[i].fd == out_p[0]) read_some(out_p[0], r.out, out_open);
            else read_some(err_p[0], r.err, err_open);
        }
    };
    // like communicate(): wait for EOF on both pipes, then for the exit
    while (out_open || err_open || !exited) {
        int wait_ms = 100;
        if (has_deadline) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            if (left <= 0) {
                r.timed_out = true;
                break;
            }
            if (left < wait_ms) wait_ms = static_cast<int>(left);
        }
        if (out_open || err_open) pump(wait_ms);
        if (!exited) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid || (w < 0 && errno == ECHILD)) exited = true;
            if (!exited && !(out_open || err_open)) {
                // pipes closed: poll the exit without blocking past the deadline
                ::usleep(10000);
            }
        }
    }
    if (r.timed_out) {
        kill_session(pid);
        // a descendant outside the session could still hold a pipe open: bounded drain
        auto drain_end = clock::now() + std::chrono::seconds(5);
        while ((out_open || err_open) && clock::now() < drain_end) pump(100);
        if (!exited) {
            for (int i = 0; i < 250 && !exited; ++i) {
                if (::waitpid(pid, &status, WNOHANG) == pid) exited = true;
                else ::usleep(20000);
            }
        }
    }
    ::close(out_p[0]);
    ::close(err_p[0]);
    untrack(pid);
    if (exited) {
        if (WIFEXITED(status)) r.rc = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) r.rc = -WTERMSIG(status);
    }
    return r;
}

}  // namespace prism::qa
