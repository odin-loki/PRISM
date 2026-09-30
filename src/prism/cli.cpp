// `prism PATH [options]`: argument parsing, testable without a process
// (tests/cpp/test_cli.cpp). main.cpp handles subcommands and --gui first.

#include "prism/cli.hpp"

#include "prism/pipeline.hpp"
#include "prism/threads.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace prism {
namespace fs = std::filesystem;

namespace {

std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> out;
    std::string t;
    std::stringstream ss{std::string(s)};
    while (std::getline(ss, t, ',')) {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

std::string stage_list() {
    std::string o;
    for (auto* p = STAGE_ORDER; *p; ++p) o += (o.empty() ? "" : ", ") + std::string(*p);
    return o;
}

// Every name must be a pipeline stage: `--stage bcm` would otherwise skip
// every stage and exit 0 (Law 7).
std::optional<std::string> unknown_stage(const std::vector<std::string>& names) {
    for (auto& n : names) {
        bool ok = false;
        for (auto* p = STAGE_ORDER; *p; ++p) ok = ok || n == *p;
        if (!ok) return n;
    }
    return std::nullopt;
}

bool looks_like_option(std::string_view v) {
    if (v.size() < 2 || v[0] != '-') return false;
    double d;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
    return !(ec == std::errc() && p == v.data() + v.size());  // "-1" is a value
}

struct Args {
    std::span<const char* const> args;
    std::size_t i = 0;
    std::string name;                  // option name without "=value"
    std::optional<std::string> value;  // inline "=value" / "-jN"
    std::string error;

    // The option's value: inline, else the next argument (never an option).
    std::optional<std::string> take() {
        if (value) return value;
        if (i + 1 < args.size() && !looks_like_option(args[i + 1])) return std::string(args[++i]);
        error = "argument " + name + ": expected one argument";
        return std::nullopt;
    }
    std::optional<std::string> take_nonempty() {
        auto v = take();
        if (v && v->empty()) {
            error = "argument " + name + ": expected a non-empty value";
            return std::nullopt;
        }
        return v;
    }
    // A switch takes no value (argparse: "ignored explicit argument").
    bool flag() {
        if (!value) return true;
        error = "argument " + name + ": ignored explicit argument '" + *value + "'";
        return false;
    }
    std::optional<int> take_int(int min) {
        auto v = take();
        if (!v) return std::nullopt;
        int n = 0;
        auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
        if (v->empty() || ec != std::errc() || p != v->data() + v->size()) {
            error = "argument " + name + ": invalid int value: '" + *v + "'";
            return std::nullopt;
        }
        if (n < min) {
            error = "argument " + name + ": must be at least " + std::to_string(min) + ", got " + *v;
            return std::nullopt;
        }
        return n;
    }
    std::optional<double> take_float() {
        auto v = take();
        if (!v) return std::nullopt;
        double d = 0;
        auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), d);
        if (v->empty() || ec != std::errc() || p != v->data() + v->size() || !std::isfinite(d)) {
            error = "argument " + name + ": invalid float value: '" + *v + "'";
            return std::nullopt;
        }
        if (d < 0) {
            error = "argument " + name + ": must not be negative, got " + *v;
            return std::nullopt;
        }
        return d;
    }
};

// Splits "--name=value" and "-jN"; false for a positional argument.
bool split_option(std::string_view a, std::string& name, std::optional<std::string>& value) {
    value.reset();
    if (a.size() < 2 || a[0] != '-') return false;
    if (a.starts_with("--")) {
        auto eq = a.find('=');
        name = std::string(a.substr(0, eq));
        if (eq != std::string_view::npos) value = std::string(a.substr(eq + 1));
        return true;
    }
    name = std::string(a.substr(0, 2));
    if (a.size() > 2) value = std::string(a.substr(a[2] == '=' ? 3 : 2));
    return true;
}

}  // namespace

CliResult<CliOptions> parse_cli(std::span<const char* const> args) {
    CliOptions o;
    o.cfg = default_config();
    auto& cfg = o.cfg;
    bool have_path = false, positional_only = false;
    Args in{args};
    for (in.i = 0; in.i < args.size(); ++in.i) {
        const std::string_view raw = args[in.i];
        if (positional_only || !split_option(raw, in.name, in.value)) {
            if (raw == "--" && !positional_only) {
                positional_only = true;
                continue;
            }
            if (have_path) return CliError("unrecognized arguments: " + std::string(raw));
            o.path = std::string(raw);
            have_path = true;
            continue;
        }
        if (raw == "--") {
            positional_only = true;
            continue;
        }
        const std::string& a = in.name;
        bool ok = true;
        if (a == "--no-llm") { ok = in.flag(); cfg.llm = false; }
        else if (a == "--gui") { ok = in.flag(); cfg.gui = true; }
        else if (a == "--resume") { ok = in.flag(); cfg.resume = true; }
        else if (a == "--allow-exec") { ok = in.flag(); cfg.allow_exec = true; }
        else if (a == "--strict-aliasing") { ok = in.flag(); cfg.strict_aliasing = true; }
        else if (a == "--pir-drafts") { ok = in.flag(); cfg.pir_drafts = true; }
        else if (a == "--fp-checks") { ok = in.flag(); cfg.fp_checks = true; }
        else if (a == "--certified") { ok = in.flag(); cfg.certified = true; }
        else if (a == "--z3-only") { ok = in.flag(); o.z3_only = true; }
        else if (a == "--list-stages") { ok = in.flag(); o.list_stages = true; }
        else if (a == "--version" || a == "-V") {
            if (!in.flag()) return CliError(in.error);
            o.version = true;
            return o;
        } else if (a == "-h" || a == "--help") {
            if (!in.flag()) return CliError(in.error);
            o.help = true;
            return o;
        } else if (a == "--out") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.out = *v;
        } else if (a == "--pbsd") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.pbsd_root = fs::absolute(*v);
        } else if (a == "--solver-cache") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.solver_cache = fs::absolute(*v);
        } else if (a == "--requirements") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.requirements.push_back(fs::absolute(*v));
        } else if (a == "--contracts-approved") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.contracts_approved = fs::absolute(*v);
        } else if (a == "--pir-vcs") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) o.pir_vcs_src = *v;
        } else if (a == "--solve-smt2") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) o.solve_smt2 = *v;
        } else if (a == "--stage" || a == "--skip") {
            auto v = in.take();
            if ((ok = v.has_value())) {
                auto names = split_csv(*v);
                if (auto bad = unknown_stage(names))
                    return CliError("argument " + a + ": unknown stage '" + *bad +
                                           "' (stages: " + stage_list() + ")");
                // An empty --stage means every stage, as if it were not given.
                if (a == "--stage") cfg.stages = names.empty() ? std::nullopt : std::optional(names);
                else cfg.skip = names;
            }
        } else if (a == "--unwind") {
            auto v = in.take_int(1);
            if ((ok = v.has_value())) cfg.unwind = *v;
        } else if (a == "--fuzz-iters") {
            auto v = in.take_int(0);
            if ((ok = v.has_value())) cfg.fuzz_iters = *v;
        } else if (a == "--repair-rounds") {
            auto v = in.take_int(0);
            if ((ok = v.has_value())) cfg.repair_rounds = *v;
        } else if (a == "--jobs" || a == "-j") {
            auto v = in.take_int(0);
            // 0 = half the cores (threads.hpp clamp_jobs), resolved here so
            // report.json records the count that ran.
            if ((ok = v.has_value())) cfg.jobs = *v == 0 ? clamp_jobs(0) : *v;
        } else if (a == "--fuzz-budget") {
            auto v = in.take_float();
            if ((ok = v.has_value())) cfg.fuzz_budget = *v;
        } else if (a == "--timeout") {
            auto v = in.take_float();
            if ((ok = v.has_value())) cfg.timeout = *v;
        } else if (a == "--fail-on") {
            auto v = in.take();
            if ((ok = v.has_value())) {
                if (*v != "never" && *v != "defect" && *v != "gap")
                    return CliError("argument --fail-on: invalid choice: '" + *v +
                                           "' (choose from never, defect, gap)");
                o.fail_on = *v;
            }
        } else if (a == "--tool") {
            auto v = in.take();
            if ((ok = v.has_value())) {
                auto eq = v->find('=');
                auto trim = [](std::string s) {
                    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                    return s;
                };
                std::string k = eq == std::string::npos ? "" : trim(v->substr(0, eq));
                std::string p = eq == std::string::npos ? "" : trim(v->substr(eq + 1));
                if (k.empty() || p.empty())
                    return CliError("argument --tool: expects NAME=PATH, got '" + *v + "'");
                cfg.tools[k] = p;
            }
        } else {
            return CliError("unrecognized arguments: " + std::string(raw));
        }
        if (!ok) return CliError(in.error);
    }
    return o;
}

CliResult<TriageCli> parse_triage_cli(std::span<const char* const> args) {
    TriageCli t;
    bool have_dir = false;
    Args in{args};
    for (in.i = 0; in.i < args.size(); ++in.i) {
        const std::string_view raw = args[in.i];
        if (!split_option(raw, in.name, in.value)) {
            if (have_dir) return CliError("unrecognized arguments: " + std::string(raw));
            t.report_dir = std::string(raw);
            have_dir = true;
            continue;
        }
        const std::string& a = in.name;
        if (a == "--no-embed") {
            if (!in.flag()) return CliError(in.error);
            t.use_embedder = false;
        } else if (a == "--threshold") {
            auto v = in.take_float();
            if (!v) return CliError(in.error);
            if (*v > 1) return CliError("argument --threshold: expects a similarity in [0, 1]");
            t.threshold = *v;
        } else {
            return CliError("unrecognized arguments: " + std::string(raw));
        }
    }
    return t;
}

std::string cli_usage() {
    return
        "prism PATH [--gui] [--no-llm] [--jobs N] [--tool NAME=PATH] [--pbsd PATH]\n"
        "           [--stage a,b] [--skip a,b] [--out DIR] [--resume] [--unwind N]\n"
        "           [--fuzz-budget S] [--fuzz-iters N] [--repair-rounds N]\n"
        "           [--list-stages] [--version] [--fail-on never|defect|gap] [--allow-exec]\n"
        "           [--requirements PATH] [--contracts-approved PATH]\n"
        "           [--strict-aliasing] [--pir-drafts] [--fp-checks]\n"
        "           [--certified] [--solver-cache DIR] [--timeout S]\n"
        "prism prove FILE.lean THEOREM [--write] [--allow-exec] (Lean proof search; prove --help)\n"
        "PRISM = Performance, Regression, Integration and Security Module\n"
        "Checks any codebase (PATH: file or directory, any language): deep C/C++\n"
        "analysis (lints, compiler warnings, BMC, fuzzing, contracts) plus every other\n"
        "language through the polyglot stage (syntax, linters, type checkers) and a\n"
        "secrets/conflict-marker scan of every text file. What could not be checked\n"
        "is reported as NOTRUN, never as clean.\n"
        "A value follows its option (--out DIR) or is joined to it (--out=DIR, -j4). An unknown option,\n"
        "a missing or malformed value, an unknown stage name, or a PATH that does not\n"
        "exist is an error (exit 2), never a different scan.\n"
        "Defaults: PATH testdata, --out prism-out, --jobs half the cores (0 = half the\n"
        "  cores), --unwind 8, --fuzz-budget 8 (seconds), --fuzz-iters 2048,\n"
        "  --repair-rounds 3, --timeout 30, --fail-on never.\n"
        "--stage a,b runs only those stages, --skip a,b leaves them out (--list-stages\n"
        "  prints the names; an empty --stage runs every stage).\n"
        "Adapter search: --tool, then ~/.prism/tools/<name>/<commit>/bin (fetch_deps), then PATH.\n"
        "--resume reuses ok/NOTRUN stages from --out/stages.jsonl (report.json fallback).\n"
        "--fail-on: exit 1 on defect (FAILED/CRASH/SANFAIL, except findings with\n"
        "  extra.severity warning/note/style) or gap (defect, or anything\n"
        "  NOTRUN/ERROR/TIMEOUT). A crashed stage is exit 2.\n"
        "--allow-exec: run code from the scanned tree (sanitizer/fuzz/diff harnesses,\n"
        "  perl -c, cargo clippy, eslint, LLM programs) in a sandbox; only on code you\n"
        "  trust. Without it those steps are NOTRUN (Law 9).\n"
        "--strict-aliasing: the pir stage also checks effective types (C11 6.5p7);\n"
        "  off by default because real code often breaks strict aliasing on purpose.\n"
        "--pir-drafts: pir takes pointer sizes from the template harness draft when no\n"
        "  requires clause gives them (PROVED-ASSUMING at best; off: NEEDS-HARNESS).\n"
        "--fp-checks: pir also reports floating-point division by zero, invalid\n"
        "  operations (NaN) and overflow to infinity (defined by IEEE/Annex F).\n"
        "--certified: the pir stage asks for an LRAT certificate of every verification\n"
        "  condition (CaDiCaL proof checked by cake_lpr, and by Lean's LRAT checker when\n"
        "  the Lean-proved bit-blaster made the CNF); a function whose VCs are all\n"
        "  certified is PROVED-CERTIFIED, otherwise it stays PROVED with a certify_note.\n"
        "--solver-cache DIR: solver query cache (default $XDG_CACHE_HOME/prism/solver).\n"
        "--timeout S: solver seconds per query (default 30).\n"
        "Every run writes TRUSTED_BASE.md and VERDICTS.md next to the reports.\n"
        "--pbsd PATH: ParanoidBSD tree for the pbsd stage (else PRISM_PBSD; no default).\n"
        "  Importing its modules also needs --allow-exec.\n"
        "--requirements PATH: markdown/text requirement documents (file or directory,\n"
        "  repeatable); the review stage drafts contracts traced to their sentences.\n"
        "--contracts-approved PATH: approvals of drafted contracts (default\n"
        "  <root>/contracts.approved.json); only approved clauses give PROVED-ASSUMING.\n"
        "Writes report.json, report.md and report.sarif (SARIF 2.1.0) under --out, plus\n"
        "triage.json (root-cause clusters; ordering only, never a status change).\n"
        "Subcommands over a finished report (see docs/AI.md):\n"
        "  prism regress [--report OUT/report.json] [--write-tests DIR] [--run --allow-exec]\n"
        "  prism ask \"<question>\" [--report OUT/report.json] [--json] [--no-llm]\n"
        "  prism draft [--report OUT/report.json] [--kind report|assurance]\n"
        "  prism triage [OUT] [--threshold T] [--no-embed]\n"
        "SV-COMP (docs/SVCOMP.md): prism svcomp --prop P.prp TASK.c | svcomp score | svcomp pack\n";
}

}  // namespace prism
