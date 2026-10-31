// A sandboxed run that reaches its wall-clock deadline keeps the stdout and
// stderr the child already wrote, and stays a timeout. rc 0 would be the
// CLEAN shape. A tool that never starts (failed, rc 127, timed_out clear)
// is the missing-tool NOTRUN shape. Neither is a deadline.
//
// The bytes are produced by a writer that exits (cat / type) before the
// hang, so they are in the pipes when the runner kills the child. The
// marker sits after 500 filler bytes: the last 400 bytes hold it, and the
// first 400 filler bytes do not.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/sandbox.hpp"

#include "prism/exec.hpp"
#include "prism/sandbox.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / ("prism-sandbox-" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
#ifndef _WIN32
        // Match the path bwrap bind-mounts (wrap_argv canonicalizes the scratch).
        std::error_code ec;
        auto canon = fs::weakly_canonical(dir, ec);
        if (!ec) dir = canon;
#endif
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void write_file(const fs::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out << body;
    REQUIRE(out.good());
}

std::string last_bytes(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    return s.substr(s.size() - n);
}

// failed -> missing-tool NOTRUN. timed_out -> TIMEOUT. rc 0 -> CLEAN.
std::string run_shape(const prism::detail::RunOut& r) {
    if (r.failed) return std::string(prism::laws::NOTRUN);
    if (r.timed_out) return std::string(prism::laws::TIMEOUT);
    if (r.rc == 0) return std::string(prism::laws::CLEAN);
    return std::string(prism::laws::FAILED);
}

}  // namespace

TEST_CASE("sandbox: a timeout keeps the tail of stdout and stderr") {
    Scratch td;
    const std::string stdout_filler(500, 'S');
    const std::string stderr_filler(500, 'E');
    const auto out_file = td.dir / "out.txt";
    const auto err_file = td.dir / "err.txt";
    write_file(out_file, stdout_filler + "STDOUT-TAIL\n");
    write_file(err_file, stderr_filler + "STDERR-TAIL\n");
    // The marker is past the first 400 bytes of what the child writes.
    CHECK(stdout_filler.size() > 400);
    CHECK(stdout_filler.find("STDOUT-TAIL") == std::string::npos);
    CHECK(stderr_filler.find("STDERR-TAIL") == std::string::npos);

    // A missing executable never started. That is NOTRUN, not a timeout and not rc 0.
    {
        prism::detail::RunSpec miss;
        miss.argv = {(td.dir / "no-such-prism-sandbox-tool").string()};
        miss.timeout_s = 2.0;
        miss.limits = prism::sandbox::limits_for(miss.timeout_s);
        REQUIRE(miss.limits.enabled);
        auto m = prism::detail::run(miss);
        CHECK(m.failed);
        CHECK_FALSE(m.timed_out);
        CHECK(m.rc == 127);
        CHECK(m.rc != 0);
        CHECK(run_shape(m) == prism::laws::NOTRUN);
        CHECK(run_shape(m) != prism::laws::CLEAN);
        CHECK(run_shape(m) != prism::laws::TIMEOUT);
    }

    std::vector<std::string> argv;
#ifdef _WIN32
    REQUIRE(out_file.string().find_first_of("%!\"&") == std::string::npos);
    REQUIRE(err_file.string().find_first_of("%!\"&") == std::string::npos);
    const auto script = td.dir / "hang.cmd";
    // type finishes, then ping holds the process past the deadline.
    write_file(script, "@echo off\r\n"
                       "type \"" +
                           out_file.string() +
                           "\"\r\n"
                           "type \"" +
                           err_file.string() +
                           "\" 1>&2\r\n"
                           "ping -n 10 127.0.0.1 >nul\r\n");
    argv = {script.string()};
#else
    REQUIRE(out_file.string().find('\'') == std::string::npos);
    REQUIRE(err_file.string().find('\'') == std::string::npos);
    const auto script = td.dir / "hang.sh";
    write_file(script, "cat '" + out_file.string() +
                           "'\n"
                           "cat '" +
                           err_file.string() +
                           "' >&2\n"
                           "sleep 8\n");
    argv = {"sh", script.string()};
#endif

    prism::detail::RunSpec spec;
    spec.argv = prism::sandbox::wrap_argv(argv, td.dir);
    spec.timeout_s = 2.0;
    spec.limits = prism::sandbox::limits_for(spec.timeout_s);
    spec.cwd = td.dir;
    REQUIRE(spec.limits.enabled);
    REQUIRE(spec.limits.limit_as);

    auto r = prism::detail::run(spec);
    const auto out_tail = last_bytes(r.out, 400);
    const auto err_tail = last_bytes(r.err, 400);
    CAPTURE(r.rc);
    CAPTURE(r.timed_out);
    CAPTURE(r.failed);
    CAPTURE(r.crashed);
    CAPTURE(out_tail);
    CAPTURE(err_tail);

    CHECK(r.timed_out);
    CHECK_FALSE(r.failed);
    CHECK_FALSE(r.crashed);
    CHECK(r.rc != 0);
    CHECK(run_shape(r) == prism::laws::TIMEOUT);
    CHECK(run_shape(r) != prism::laws::CLEAN);
    CHECK(run_shape(r) != prism::laws::NOTRUN);
    CHECK_FALSE(out_tail.empty());
    CHECK_FALSE(err_tail.empty());
    CHECK(out_tail.find("STDOUT-TAIL") != std::string::npos);
    CHECK(err_tail.find("STDERR-TAIL") != std::string::npos);
}
