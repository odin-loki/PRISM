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
#  include <fcntl.h>
#  include <poll.h>
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

void kill_session(int sid) noexcept {
#ifndef _WIN32
    if (sid <= 1) return;
    ::killpg(sid, SIGKILL);
    // members that moved to process groups of their own (PRISM's solver children)
    try {
        for (int round = 0; round < 3; ++round) {
            auto left = session_members(sid);
            std::erase(left, static_cast<int>(::getpid()));
            if (left.empty()) break;
            for (int p : left) ::kill(p, SIGKILL);
        }
    } catch (...) {
    }
    ::kill(sid, SIGKILL);
#else
    (void)sid;
#endif
}

SessionOut run_session(const std::vector<std::string>& args, double timeout_s, const std::filesystem::path& cwd) {
    SessionOut r;
#ifdef _WIN32
    auto p = run_process(args, timeout_s > 0 ? timeout_s : 1e9, cwd);
    r.out = p.text;
    r.rc = p.rc;
    r.timed_out = p.timed_out;
    r.failed = p.failed;
    return r;
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
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const std::string dir = cwd.empty() ? std::string() : cwd.string();
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out_p[0]);
        ::close(out_p[1]);
        r.failed = true;
        return r;
    }
    if (pid == 0) {
        ::setsid();
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDERR_FILENO);
        }
        ::dup2(out_p[1], STDOUT_FILENO);
        ::close(out_p[0]);
        ::close(out_p[1]);
        if (!dir.empty() && ::chdir(dir.c_str()) != 0) ::_exit(127);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    ChildGroup tracked(pid);  // the session leader's group dies with PRISM on SIGINT/SIGTERM
    ::close(out_p[1]);
    int flags = ::fcntl(out_p[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(out_p[0], F_SETFL, flags | O_NONBLOCK);
    using clock = std::chrono::steady_clock;
    const bool bounded = timeout_s > 0;
    const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                             std::chrono::duration<double>(bounded ? timeout_s : 0.0));
    bool eof = false, reaped = false;
    int st = 0;
    auto drain = [&] {
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(out_p[0], buf, sizeof buf);
            if (n > 0) {
                r.out.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) eof = true;
            else if (errno == EINTR) continue;
            break;
        }
    };
    // like communicate(): wait for EOF on stdout and for the child
    while (!(eof && reaped)) {
        if (!eof) drain();
        if (!reaped) {
            pid_t w = ::waitpid(pid, &st, WNOHANG);
            if (w == pid) reaped = true;
        }
        if (eof && reaped) break;
        if (bounded && clock::now() >= deadline) {
            r.timed_out = true;
            kill_session(pid);
            break;
        }
        int wait_ms = 50;
        if (bounded) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            wait_ms = static_cast<int>(std::max<long long>(1, std::min<long long>(50, left)));
        }
        if (!eof) {
            pollfd pfd{out_p[0], POLLIN, 0};
            ::poll(&pfd, 1, wait_ms);
        } else {
            ::usleep(static_cast<useconds_t>(wait_ms) * 1000);
        }
    }
    if (r.timed_out) {
        // a descendant outside the session could still hold the pipe open: bounded wait
        const auto until = clock::now() + std::chrono::seconds(5);
        while (!(eof && reaped) && clock::now() < until) {
            if (!eof) drain();
            if (!reaped && ::waitpid(pid, &st, WNOHANG) == pid) reaped = true;
            if (!(eof && reaped)) ::usleep(20000);
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            if (::waitpid(pid, &st, 0) == pid) reaped = true;
        }
    }
    ::close(out_p[0]);
    if (reaped) {
        if (WIFEXITED(st)) r.rc = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) r.rc = -WTERMSIG(st);
    }
    if (r.rc == 127 && !r.timed_out && r.out.empty()) r.failed = true;
    return r;
#endif
}

}  // namespace prism::detail
