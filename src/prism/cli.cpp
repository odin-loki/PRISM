// `prism PATH [options]`: argument parsing, testable without a process
// (tests/cpp/test_cli.cpp). main.cpp handles subcommands first.

#include "prism/cli.hpp"
#include "prism/complexity.hpp"

#include "prism/pipeline.hpp"
#include "prism/threads.hpp"

#include <algorithm>
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
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(0, 1);
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
    Args in{args, 0, {}, {}, {}};
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
        else if (a == "--resume") { ok = in.flag(); cfg.resume = true; }
        else if (a == "--allow-exec") { ok = in.flag(); cfg.allow_exec = true; }
        else if (a == "--strict-aliasing") { ok = in.flag(); cfg.strict_aliasing = true; }
        else if (a == "--pir-drafts") { ok = in.flag(); cfg.pir_drafts = true; }
        else if (a == "--pir-harness") { ok = in.flag(); cfg.pir_harness = true; }
        else if (a == "--pir-discharge") { ok = in.flag(); cfg.pir_discharge = true; }
        else if (a == "--pir-container-of") { ok = in.flag(); cfg.pir_container_of = true; }
        else if (a == "--pir-vacuity") { ok = in.flag(); cfg.pir_vacuity = true; }
        else if (a == "--pir-input-hash") { ok = in.flag(); cfg.pir_input_hash = true; }
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
        } else if (a == "--kernel-api") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) {
                if (*v == "linux" || *v == "freebsd") cfg.pir_kernel_api = *v;
                else {
                    ok = false;
                    in.error = "argument --kernel-api: expected linux or freebsd, got '" + *v + "'";
                }
            }
        } else if (a == "--assume-unknown-calls") {
            ok = in.flag();
            cfg.pir_assume_unknown_calls = true;
        } else if (a == "--callee-contracts") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.callee_contracts = fs::absolute(*v);
        } else if (a == "--pir-field-invariants") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.pir_field_invariants = fs::absolute(*v);
        } else if (a == "--pir-ops-auto") {
            ok = in.flag();
            cfg.pir_ops_auto = true;
        } else if (a == "--pir-addr-values") {
            ok = in.flag();
            cfg.pir_addr_values = true;
        } else if (a == "--pir-builtin-contracts") {
            ok = in.flag();
            cfg.pir_builtin_contracts = true;
        } else if (a == "--pir-build-semantics") {
            ok = in.flag();
            cfg.pir_build_semantics = true;
        } else if (a == "--pir-kernel-stops") {
            ok = in.flag();
            cfg.pir_kernel_stops = true;
        } else if (a == "--pir-kernel-current") {
            ok = in.flag();
            cfg.pir_kernel_current = true;
        } else if (a == "--pir-global-harness") {
            ok = in.flag();
            cfg.pir_global_harness = true;
        } else if (a == "--pir-lock-wrappers") {
            ok = in.flag();
            cfg.pir_lock_wrappers = true;
        } else if (a == "--pir-icall-havoc") {
            ok = in.flag();
            cfg.pir_icall_havoc = true;
        } else if (a == "--pir-harness-deep") {
            ok = in.flag();
            cfg.pir_harness_deep = true;
        } else if (a == "--ops-table") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.ops_table = fs::absolute(*v);
        } else if (a == "--lock-contracts") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.pir_lock_contracts = *v;
        } else if (a == "--pir-harness-depth") {
            auto v = in.take_int(0);
            if ((ok = v.has_value())) cfg.pir_harness_depth = *v;
        } else if (a == "--pir-harness-elems") {
            auto v = in.take_int(1);
            if ((ok = v.has_value())) cfg.pir_harness_elems = static_cast<unsigned long long>(*v);
        } else if (a == "--pir-harness-bytes") {
            auto v = in.take_int(1);
            if ((ok = v.has_value())) cfg.pir_harness_bytes = static_cast<unsigned long long>(*v);
        } else if (a == "--jsonl") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.jsonl = fs::absolute(*v);
        } else if (a == "--compile-commands") {
            auto v = in.take_nonempty();
            if ((ok = v.has_value())) cfg.compile_commands = fs::absolute(*v);
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
        } else if (a == "--preset") {
            // --preset prove: the proving path only (docs/UPGRADE_PLAN.md P1.4):
            // inventory, classify and pir, no language model. Adds to any
            // --stage names already given, like a repeated --stage.
            auto v = in.take();
            ok = v.has_value() && *v == "prove";
            if (!ok && v) in.error = "argument --preset: unknown preset '" + v.value_or("") + "' (presets: prove)";
            if (ok) {
                cfg.llm = false;
                if (!cfg.stages) cfg.stages.emplace();
                for (const char* st : {"inventory", "classify", "pir"})
                    if (std::find(cfg.stages->begin(), cfg.stages->end(), st) == cfg.stages->end()) cfg.stages->push_back(st);
            }
        } else if (a == "--stage" || a == "--skip") {
            auto v = in.take();
            if ((ok = v.has_value())) {
                auto names = split_csv(*v);
                if (auto bad = unknown_stage(names))
                    return CliError("argument " + a + ": unknown stage '" + *bad +
                                           "' (stages: " + stage_list() + ")");
                // An empty --stage leaves the selection unset (every stage; an
                // empty list would run none). An empty --skip does not clear
                // names already skipped. Repeating either flag appends names
                // and keeps every name already chosen.
                if (a == "--stage") {
                    if (!names.empty()) {
                        if (!cfg.stages) cfg.stages.emplace();
                        cfg.stages->insert(cfg.stages->end(), names.begin(), names.end());
                    }
                } else if (!names.empty()) {
                    cfg.skip.insert(cfg.skip.end(), names.begin(), names.end());
                }
            }
        } else if (a == "--store-buffer") {
            auto v = in.take_int(1);
            if ((ok = v.has_value())) cfg.conc_store_buffer = *v;
        } else if (a == "--memory-model") {
            auto v = in.take();
            ok = v.has_value() && (*v == "sc" || *v == "tso");
            if (ok) cfg.conc_tso = *v == "tso";
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
        } else if (a == "--complexity") {
            // Z3 rlimit units per verification-condition query (0: off, the
            // wall clock alone); process-wide (include/prism/complexity.hpp)
            auto v = in.take();
            unsigned long long n = 0;
            if (v) {
                auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
                ok = !v->empty() && ec == std::errc() && p == v->data() + v->size();
                if (!ok) in.error = "argument --complexity: invalid int value: '" + *v + "'";
            } else {
                ok = false;
            }
            if (ok) complexity::set_scale(n);
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
                auto trim = [](std::string_view sv) {
                    while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
                    while (!sv.empty() && sv.back() == ' ') sv.remove_suffix(1);
                    return std::string(sv);
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
    Args in{args, 0, {}, {}, {}};
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
        "prism PATH [--no-llm] [--jobs N] [--tool NAME=PATH] [--pbsd PATH]\n"
        "           [--stage a,b] [--skip a,b] [--preset prove] [--out DIR] [--resume] [--unwind N]\n"
        "           [--fuzz-budget S] [--fuzz-iters N] [--repair-rounds N]\n"
        "           [--list-stages] [--version] [--fail-on never|defect|gap] [--allow-exec]\n"
        "           [--requirements PATH] [--contracts-approved PATH]\n"
        "           [--strict-aliasing] [--pir-drafts] [--fp-checks]\n"
        "           [--certified] [--solver-cache DIR] [--timeout S] [--complexity N]\n"
        "           [--compile-commands FILE] [--kernel-api linux|freebsd]\n"
        "           [--assume-unknown-calls] [--ops-table FILE]\n"
        "prism prove FILE.lean THEOREM [--write] [--allow-exec] (Lean proof search; prove --help)\n"
        "prism library import|vet|stats|lookup ... (proof library of earlier verdicts; library --help)\n"
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
        "  prints the names; an empty --stage runs every stage; repeating --stage\n"
        "  adds names and keeps earlier ones; repeating --skip adds names and\n"
        "  keeps earlier ones).\n"
        "--preset prove runs only the proving stages (inventory, classify, pir) with\n"
        "  --no-llm; pir checks every function of every unit as its own task.\n"
        "Adapter search: --tool, then ~/.prism/tools/<name>/<commit>/bin (prism-deps tool), then PATH.\n"
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
        "--compile-commands FILE: the pir stage compiles each unit with its entry's flags\n"
        "  from this compile_commands.json (language level such as gnu11, defines, include\n"
        "  path, target, short-wchar, ...); warning flags are never taken, so the UB-folding\n"
        "  warnings stay on. A Linux kernel unit needs it (docs/PIR.md).\n"
        "--kernel-api linux|freebsd: the pir stage links the kernel API specs (allocation,\n"
        "  locking, user access, MMIO, delays, logging, BUG/WARN, ...) and models inline asm,\n"
        "  asm goto, atomics and barriers conservatively; every spec and model used is a\n"
        "  named assumption in the verdict (PROVED-ASSUMING). Off by default (docs/KERNEL_MODELS.md).\n"
        "--pir-discharge: with --pir-harness, the harness of a static, non-address-taken, non-recursive\n"
        "  function is a proof obligation checked at every call site of the unit; discharged, the\n"
        "  invented assumption is replaced by the call sites (docs/HARNESS_GENERATOR.md).\n"
        "--pir-container-of: with --pir-harness, a parameter used at a negative constant offset gets its\n"
        "  containing object (container_of). --pir-vacuity: a harnessed function whose exit is\n"
        "  unreachable under its assumptions is NEEDS-HARNESS (vacuous), never a proof.\n"
        "--pir-input-hash: check nothing; each translated function's row (NOTRUN) carries input_key\n"
        "  and input_key_loc_free: equal keys mean the same check (docs/PIR.md \"Input key\").\n"
        "--callee-contracts FILE: assume-guarantee stubs for callees without a body (TSV: function,\n"
        "  signature, requires, assigns, ensures, status, source). The requires are checked at the call\n"
        "  site, only the assigns are havocked, the ensures are assumed; a callee without a usable\n"
        "  contract is treated as before. Every stub used is a named assumption with its status\n"
        "  (docs/HARNESS_PHASE2.md). --pir-ops-auto: ops tables derived from the unit's own function-pointer\n"
        "  stores; a harness function-pointer field is bound to one of its targets (named assumption).\n"
        "  --pir-addr-values: ptrtoint / inttoptr constant expressions. --pir-harness-deep: depth 4, 64 objects.\n"
        "  --pir-builtin-contracts: with --kernel-api, contracts for llvm.read_register / prefetch / objectsize.\n"
        "  --pir-build-semantics: with --compile-commands, a unit built with -fwrapv or -fno-strict-overflow\n"
        "  is checked with that semantics: signed + - * and << wrap (not UB); every other check stays (named).\n"
        "  --pir-kernel-stops: with --kernel-api, Linux BUG() and panic() / kassert_panic() end the path\n"
        "  (a named assumption: the verdict covers the paths that do not stop there).\n"
        "  --pir-kernel-current: with --kernel-api, current (Linux) and curthread (FreeBSD) point to a valid object.\n"
        "  --pir-global-harness: with --kernel-api, mutable pointer globals (MMIO bases, bus handles) point to a valid object.\n"
        "  --pir-lock-wrappers: with --kernel-api, an acquire-only or release-only function is a lock wrapper (named).\n"
        "  --pir-icall-havoc: with --kernel-api, an indirect call with more than 16 targets or to a target outside\n"
        "  the address-taken functions is a havoc of all memory with an arbitrary result (named).\n"
        "  --pir-field-invariants FILE: struct field invariants (TSV: struct, offset, width, kind valid|nonnull|range,\n"
        "  a, b, status, source) applied to automatic-harness objects, one named assumption per row used.\n"
        "--assume-unknown-calls: a call without a spec havocs its result and the objects its\n"
        "  pointer arguments point to (assumption listed). Opt-in; the default stays UNENCODED.\n"
        "--ops-table FILE: an indirect call through a field of a known ops table (TSV: struct\n"
        "  type, field, targets, role, optional field index) is checked against each target,\n"
        "  at most 8 (more stays UNENCODED). Off by default (docs/PIR.md).\n"
        "--lock-contracts FILE: TSV of lock contracts for unmodified kernel code (function, lock\n"
        "  global:NAME|arg:N|field:TYPE.INDEX, acquires|releases|requires-held, optional mutex|spin); needs --kernel-api.\n"
        "--complexity N: Z3 resource-limit (rlimit) units per pir query of a function of\n"
        "  reference size (default: --timeout x 1000000, else PRISM_PIR_RLIMIT; 0 = off);\n"
        "  the same limit on every machine, so \"not decided\" is reproducible; the\n"
        "  --timeout clock stays as a backstop (3 x).\n"
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
