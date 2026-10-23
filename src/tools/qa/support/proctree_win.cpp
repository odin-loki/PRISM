// Windows counterpart of proctree.cpp (POSIX sessions: setsid / killpg / /proc).
//
// run_tree is prism_core's one process runner (src/prism/proc.cpp, detail::run)
// with its Windows implementation: the command and every descendant run in a
// Job Object, so a timeout terminates the whole tree (the counterpart of
// killpg on the command's session), and a handle list limits what the command
// inherits to its three standard handles. Closing the scorer's last handle to
// the job also ends the tree (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), which is
// what install_interrupt_cleanup does with SIGINT/SIGTERM on POSIX: there is
// nothing to install.
//
// A command that cannot be started, or cannot be placed in the job, is
// start_failed with the reason (Laws 1 and 7: never a quiet clean run).
// which() is PATH + PATHEXT.

#include "proctree.hpp"

#include "../../../prism/proc.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <system_error>

namespace prism::qa {

namespace fs = std::filesystem;

// Sessions are a POSIX notion; the Job Object is private to one run_tree call.
std::vector<int> session_members(int) { return {}; }

void kill_session(int) noexcept {}

void install_interrupt_cleanup() noexcept {}

namespace {

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

}  // namespace

std::string which(const std::string& name) {
    if (name.empty()) return {};
    std::vector<std::string> exts = {""};
    if (fs::path(name).extension().empty()) {
        std::string pe = ".COM;.EXE;.BAT;.CMD";
        if (const char* e = std::getenv("PATHEXT"); e && *e) pe = e;
        std::size_t a = 0;
        while (a <= pe.size()) {
            std::size_t b = pe.find(';', a);
            if (b == std::string::npos) b = pe.size();
            if (b > a) exts.push_back(pe.substr(a, b - a));
            a = b + 1;
        }
    }
    auto probe = [&](const fs::path& base) -> std::string {
        for (const auto& x : exts) {
            fs::path cand = base;
            cand += x;
            if (is_file(cand)) return cand.string();
        }
        return {};
    };
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return probe(name);
    const char* path = std::getenv("PATH");
    if (!path) return {};
    const std::string p = path;
    std::size_t a = 0;
    while (a <= p.size()) {
        std::size_t b = p.find(';', a);
        if (b == std::string::npos) b = p.size();
        std::string dir = p.substr(a, b - a);
        if (dir.empty()) dir = ".";
        if (auto hit = probe(fs::path(dir) / name); !hit.empty()) return hit;
        a = b + 1;
    }
    return {};
}

RunResult run_tree(const std::vector<std::string>& argv, const RunOpts& opts) {
    RunResult r;
    if (argv.empty()) {
        r.start_failed = true;
        r.err = "no argv";
        return r;
    }
    prism::detail::RunSpec spec;
    spec.argv = argv;
    spec.cwd = opts.cwd;
    spec.env = opts.env;
    spec.timeout_s = opts.timeout_s;  // 0: none (detail::run treats <= 0 as unbounded)
    spec.merge_stderr = false;
    spec.mem_limit_mb = opts.mem_limit_mb;
    prism::detail::RunOut o;
    try {
        o = prism::detail::run(spec);
    } catch (const std::exception& e) {  // Law 8: a --no-*-check flag is refused, and says so
        r.start_failed = true;
        r.err = std::string("refused: ") + e.what();
        return r;
    }
    r.out = std::move(o.out);
    r.err = std::move(o.err);
    r.rc = o.rc;
    r.timed_out = o.timed_out;
    if (o.failed) {
        r.start_failed = true;
        r.err = argv[0] + ": " + (o.err.empty() ? std::string("could not start") : o.err);
    }
    return r;
}

}  // namespace prism::qa
