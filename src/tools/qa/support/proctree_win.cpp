// Windows stand-in for proctree.cpp (POSIX sessions: setsid / killpg / /proc).
//
// There is no Job Object implementation yet, so run_tree does not start
// anything: it reports start_failed with a NOTRUN reason (Law 1: a missing
// capability is NOTRUN, never a clean result; Law 7: written down, not skipped
// quietly). Callers (replay, engine, fetch, conformance) already treat
// start_failed as "tool did not run".  which() is real: it is PATH + PATHEXT.

#include "proctree.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace prism::qa {

namespace fs = std::filesystem;

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

RunResult run_tree(const std::vector<std::string>& argv, const RunOpts&) {
    RunResult r;
    r.start_failed = true;
    r.err = "NOTRUN: prism-qa run_tree has no Windows process-tree runner yet (needs a Job Object); not run: " +
            (argv.empty() ? std::string("(no argv)") : argv[0]);
    return r;
}

}  // namespace prism::qa
