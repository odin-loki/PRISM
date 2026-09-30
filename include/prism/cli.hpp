#pragma once

// Command line of `prism PATH [options]` (src/prism/main.cpp calls it).
// Strict like a POSIX getopt_long / argparse CLI: --flag=value and -jN are
// accepted; an unknown option, a missing value, a malformed number, a
// malformed --tool NAME=PATH, an unknown stage name or a second PATH is an
// error (exit 2). A typo never runs a different scan than the one asked
// for (Laws 7 and 8).

#include "prism/config.hpp"
#include "prism/export.hpp"

#include <optional>
#include <span>
#include <string>
#include <utility>

namespace prism {

struct CliError {
    std::string message;
};

// A parsed command line or the reason it is refused.
template <class T>
struct CliResult {
    std::optional<T> value;
    std::string message;
    CliResult(T v) : value(std::move(v)) {}
    CliResult(CliError e) : message(std::move(e.message)) {}
    explicit operator bool() const { return value.has_value(); }
    T* operator->() { return &*value; }
    const T* operator->() const { return &*value; }
    T& operator*() { return *value; }
    const std::string& error() const { return message; }
};

struct CliOptions {
    Config cfg;
    std::string path = "testdata";
    std::string fail_on = "never";
    bool help = false;         // -h / --help: print cli_usage()
    bool version = false;      // -V / --version
    bool list_stages = false;  // --list-stages
    // Hidden, for tools/solver_bench.py (not a scan; JSON on stdout).
    std::string pir_vcs_src;
    std::string solve_smt2;
    bool z3_only = false;
};

// args: argv[1..argc) of the scan command (no subcommand, no --gui: main
// hands those off first). The error text names the argument, e.g.
// "argument --unwind: invalid int value: 'x'".
PRISM_API CliResult<CliOptions> parse_cli(std::span<const char* const> args);

// `prism --help`.
PRISM_API std::string cli_usage();

// `prism triage [OUT] [--threshold T] [--no-embed]`.
struct TriageCli {
    std::string report_dir = "prism-out";
    double threshold = -1;  // < 0: the triage default
    bool use_embedder = true;
};
PRISM_API CliResult<TriageCli> parse_triage_cli(std::span<const char* const> args);

}  // namespace prism
