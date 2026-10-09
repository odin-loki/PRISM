// run_session (src/prism/proc.hpp): a command in a session of its own whose
// whole process tree is killed on a timeout.
#include "proc.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <cerrno>
#  include <csignal>
#  include <cstdlib>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace prism::detail {

std::vector<int> session_members(int sid) {
    std::vector<int> out;
#if defined(__linux__)
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string n = e.path().filename().string();
        if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream in(e.path() / "stat", std::ios::binary);
        if (!in) continue;
        std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // comm (field 2) may contain blanks and ')': split after the last ')'
        auto close = stat.rfind(')');
        if (close == std::string::npos || close + 2 > stat.size()) continue;
        std::vector<std::string> rest;
        std::string cur;
        for (std::size_t i = close + 2; i < stat.size(); ++i) {
            char c = stat[i];
            if (c == ' ' || c == '\n') {
                if (!cur.empty()) rest.push_back(cur);
                cur.clear();
                if (rest.size() > 3) break;
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) rest.push_back(cur);
        // rest[0] state, [1] ppid, [2] pgrp, [3] session
        if (rest.size() > 3 && rest[3] == std::to_string(sid)) out.push_back(std::stoi(n));
    }
#else
    (void)sid;
#endif
    return out;
}

void kill_session(int sid) noexcept { kill_session_now(sid); }

SessionOut run_session(const std::vector<std::string>& args, double timeout_s,
                       const std::filesystem::path& cwd) {
    SessionOptions opt;
    opt.timeout_s = timeout_s;
    opt.cwd = cwd;
    return run_session(args, opt);
}

SessionOut run_session(const std::vector<std::string>& args, const SessionOptions& opt) {
    SessionOut r;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
#ifdef _WIN32
    RunSpec spec;
    spec.argv = args;
    spec.timeout_s = opt.timeout_s > 0 ? opt.timeout_s : 1e9;
    spec.cwd = opt.cwd;
    spec.env = opt.env;
    spec.merge_stderr = true;
    auto p = run(spec);
    r.out = p.out;
    r.err = p.err;
    r.rc = p.rc;
    r.timed_out = p.timed_out;
    r.failed = p.failed;
    r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return r;
#else
    if (args.empty()) {
        r.failed = true;
        r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
        return r;
    }
    int out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, in_p[2] = {-1, -1};
    const bool use_in = !opt.input.empty();
    if (::pipe(out_p) != 0 || ::pipe(err_p) != 0 || (use_in && ::pipe(in_p) != 0)) {
        if (out_p[0] >= 0) ::close(out_p[0]), ::close(out_p[1]);
        if (err_p[0] >= 0) ::close(err_p[0]), ::close(err_p[1]);
        if (in_p[0] >= 0) ::close(in_p[0]), ::close(in_p[1]);
        r.failed = true;
        r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
        return r;
    }
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const std::string dir = opt.cwd.empty() ? std::string() : opt.cwd.string();
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out_p[0]);
        ::close(out_p[1]);
        ::close(err_p[0]);
        ::close(err_p[1]);
        if (use_in) ::close(in_p[0]), ::close(in_p[1]);
        r.failed = true;
        r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
        return r;
    }
    if (pid == 0) {
        ::setsid();
        if (use_in) {
            ::dup2(in_p[0], STDIN_FILENO);
            ::close(in_p[0]);
            ::close(in_p[1]);
        } else {
            int devnull = ::open("/dev/null", O_RDWR);
            if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        }
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(err_p[1], STDERR_FILENO);
        ::close(out_p[0]);
        ::close(out_p[1]);
        ::close(err_p[0]);
        ::close(err_p[1]);
        if (opt.rlimit_as_mb > 0) {
            rlimit rl{};
            const auto bytes = static_cast<rlim_t>(opt.rlimit_as_mb) * 1024 * 1024;
            rl.rlim_cur = rl.rlim_max = bytes;
            ::setrlimit(RLIMIT_AS, &rl);
        }
        if (!dir.empty() && ::chdir(dir.c_str()) != 0) ::_exit(127);
        for (const auto& [k, v] : opt.env) ::setenv(k.c_str(), v.c_str(), 1);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    ChildSession tracked(pid);  // the whole session dies with PRISM on SIGINT/SIGTERM
    ::close(out_p[1]);
    ::close(err_p[1]);
    if (use_in) {
        ::close(in_p[0]);
        const char* p = opt.input.data();
        std::size_t left = opt.input.size();
        while (left > 0) {
            ssize_t n = ::write(in_p[1], p, left);
            if (n > 0) {
                p += n;
                left -= static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        ::close(in_p[1]);
    }
    for (int fd : {out_p[0], err_p[0]}) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    const bool bounded = opt.timeout_s > 0;
    const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                             std::chrono::duration<double>(bounded ? opt.timeout_s : 0.0));
    bool out_eof = false, err_eof = false, exited = false;
    int st = 0;
    // the leader stays a zombie until reaped, so its pid (= the session id)
    // cannot be reused while the rest of the session is killed
    auto leader_exited = [&] {
        siginfo_t si{};
        if (::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) != 0) return false;
        return si.si_pid == pid;
    };
    auto drain = [&](int fd, std::string& into, bool& eof) {
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0) {
                into.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) eof = true;
            else if (errno == EINTR) continue;
            break;
        }
    };
    for (;;) {
        if (!out_eof) drain(out_p[0], r.out, out_eof);
        if (!err_eof) drain(err_p[0], r.err, err_eof);
        if (!exited) exited = leader_exited();
        if (exited) break;
        if (bounded && clock::now() >= deadline) {
            r.timed_out = true;
            break;
        }
        int wait_ms = 50;
        if (bounded) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            wait_ms = static_cast<int>(std::max<long long>(1, std::min<long long>(50, left)));
        }
        pollfd pfds[2];
        int nfd = 0;
        if (!out_eof) pfds[nfd++] = pollfd{out_p[0], POLLIN, 0};
        if (!err_eof) pfds[nfd++] = pollfd{err_p[0], POLLIN, 0};
        if (nfd) ::poll(pfds, nfd, wait_ms);
        else ::usleep(static_cast<useconds_t>(wait_ms) * 1000);
    }
    // whatever the leader left behind (background children, solvers in groups
    // of their own) dies with the session; then read what is left in the pipes
    kill_session(pid);
    const auto until = clock::now() + std::chrono::seconds(5);
    while (!(out_eof && err_eof) && clock::now() < until) {
        if (!out_eof) drain(out_p[0], r.out, out_eof);
        if (!err_eof) drain(err_p[0], r.err, err_eof);
        if (!(out_eof && err_eof)) ::usleep(5000);
    }
    ::close(out_p[0]);
    ::close(err_p[0]);
    pid_t w = -1;
    do {
        w = ::waitpid(pid, &st, 0);
    } while (w < 0 && errno == EINTR);
    if (r.timed_out) {
        r.rc = -1;
    } else if (w == pid) {
        if (WIFEXITED(st)) r.rc = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) r.rc = -WTERMSIG(st);
    }
    if (r.rc == 127 && !r.timed_out && r.out.empty()) r.failed = true;
    r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return r;
#endif  // !_WIN32
}

}  // namespace prism::detail
