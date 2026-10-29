// Counterexample replay (`prism svcomp`): the task is compiled with the
// property's sanitizer and run on the refutation's nondet values; the
// violation has to show. Running task code needs --allow-exec (Law 9), and
// the run is sandboxed (bwrap when it works here, rlimits always).
#include "internal.hpp"

#include "../stages/common.hpp"
#include "prism/abi_model.hpp"
#include "prism/config.hpp"
#include "prism/sandbox.hpp"
#include "prism/wide_int.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

namespace {

const Regex& nondet_call() {
    static const Regex re(R"(\b__VERIFIER_nondet_(\w+)\s*\()");
    return re;
}

bool defines(const std::string& source, const std::string& name) {
    Regex re("\\b" + re_escape(name) + R"(\s*\([^;{)]*\)\s*\{)");
    return re.search(source);
}

const std::map<std::string, std::string>& nondet_types() {
    static const std::map<std::string, std::string> k = {
        {"int", "int"},
        {"uint", "unsigned int"},
        {"unsigned_int", "unsigned int"},
        {"u32", "unsigned int"},
        {"long", "long"},
        {"ulong", "unsigned long"},
        {"longlong", "long long"},
        {"ulonglong", "unsigned long long"},
        {"short", "short"},
        {"ushort", "unsigned short"},
        {"char", "char"},
        {"uchar", "unsigned char"},
        {"bool", "_Bool"},
        {"_Bool", "_Bool"},
        {"float", "float"},
        {"double", "double"},
        {"size_t", "unsigned long"},
        {"loff_t", "long long"},
        {"u8", "unsigned char"},
        {"u16", "unsigned short"},
        {"sector_t", "unsigned long"},
    };
    return k;
}

// float(text): decimal floats, inf/infinity, nan; no hex floats.
std::optional<double> parse_float(const std::string& t) {
    if (t.empty()) return std::nullopt;
    std::string c;
    for (std::size_t k = 0; k < t.size(); ++k) {
        char ch = t.at(k);
        if (ch == 'x' || ch == 'X' || ch == '(') return std::nullopt;
        if (ch == '_') {
            bool ok = k > 0 && k + 1 < t.size() && std::isdigit(static_cast<unsigned char>(t.at(k - 1))) &&
                      std::isdigit(static_cast<unsigned char>(t.at(k + 1)));
            if (!ok) return std::nullopt;
            continue;
        }
        c += ch;
    }
    char* end = nullptr;
    double d = std::strtod(c.c_str(), &end);
    if (end == c.c_str() || *end != '\0') return std::nullopt;
    return d;
}

// The run shows the property's violation (UBSan overflow report, reach_error
// abort, ASan invalid deref/free): fills rec and returns true.
bool classify_run(const std::string& prop, const std::string& err, int rc, json& rec) {
    if (prop == "no-overflow") {
        static const Regex ubsan(
            R"(^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): runtime error: (?P<msg>.*)$)", true);
        static const Regex overflow_msg(
            R"(^(signed integer overflow|negation of .* cannot be represented|division of .* cannot be represented)"
            R"(|left shift of \d+ by \d+ places cannot be represented))");
        for (const auto& m : ubsan.finditer(err)) {
            if (overflow_msg.match_prefix(m.named("msg"))) {
                rec["replay"] = "replayed";
                rec["detail"] = "UBSan: " + m.named("msg");
                rec["line"] = parse_ll(m.named("line")).value_or(0);
                rec["column"] = parse_ll(m.named("col")).value_or(0);
                return true;
            }
        }
    } else if (prop == "unreach-call") {
        if (err.find("reach_error") != std::string::npos && rc != 0) {
            // The assertion message names reach_error's own line, not the call
            // site; the witness target comes from reach_error_call_site().
            rec["replay"] = "replayed";
            rec["detail"] = "reach_error() was called and the program aborted";
            return true;
        }
    } else if (prop == "valid-memsafety") {
        static const Regex asan(R"(ERROR: AddressSanitizer: (?P<kind>[\w-]+))");
        static const Regex frame(R"(#\d+ 0x[0-9a-f]+ in \S+ (?P<file>[^\s:]+):(?P<line>\d+):(?P<col>\d+))");
        static const std::set<std::string> deref = {"heap-buffer-overflow",  "stack-buffer-overflow",
                                                    "global-buffer-overflow", "SEGV",
                                                    "heap-use-after-free",   "stack-use-after-return",
                                                    "stack-use-after-scope", "stack-buffer-underflow"};
        static const std::set<std::string> frees = {"attempting", "bad-free", "double-free"};
        if (auto am = asan.search_match(err)) {
            std::string kind = am->named("kind");
            std::string sub = deref.contains(kind) ? "valid-deref" : frees.contains(kind) ? "valid-free" : "";
            if (!sub.empty()) {
                rec["replay"] = "replayed";
                rec["detail"] = "ASan: " + kind;
                rec["subproperty"] = sub;
                if (auto fm = frame.search_match(err)) {
                    rec["line"] = parse_ll(fm->named("line")).value_or(0);
                    rec["column"] = parse_ll(fm->named("col")).value_or(0);
                }
                return true;
            }
        }
    }
    return false;
}

}  // namespace

std::optional<std::vector<std::pair<std::string, Num>>> nondet_trace(const json& f) {
    const json& extra = extra_of(f);
    if (!(has(f, "function") && f.at("function").is_string() && f.at("function").get<std::string>() == "main") ||
        !has(extra, "nondet"))
        return std::nullopt;
    std::vector<std::pair<std::string, Num>> out;
    std::string raw = pystr(extra.at("nondet"));
    std::size_t start = 0;
    for (;;) {
        auto comma = raw.find(',', start);
        std::string part = strip(std::string_view(raw).substr(start, comma == std::string::npos ? std::string::npos
                                                                                               : comma - start));
        if (!part.empty()) {
            auto eq = part.find('=');
            std::string name = strip(part.substr(0, eq));
            std::string val = eq == std::string::npos ? "" : strip(part.substr(eq + 1));
            if (auto iv = parse_i128(val)) out.emplace_back(name, Num::of_int(*iv));
            else if (auto fv = parse_float(val)) out.emplace_back(name, Num::of_float(*fv));
            else return std::nullopt;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string stub_source(const std::string& source, const std::vector<Num>& nondet_values) {
    std::string vals;
    for (std::size_t i = 0; i < nondet_values.size(); ++i) {
        if (i) vals += ", ";
        vals += float_repr(nondet_values.at(i).as_double());
    }
    if (vals.empty()) vals = "0.0";
    std::vector<std::string> out = {
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <unistd.h>",
        "static const double prism_nondet[] = {" + vals + "};",
        "static unsigned prism_next, prism_count = " + std::to_string(nondet_values.size()) + ";",
        "static double prism_take(void) { if (prism_next < prism_count) return prism_nondet[prism_next++];",
        "  fprintf(stderr, \"PRISM-NONDET-EXHAUSTED\\n\"); return 0; }",
    };
    std::set<std::string> suffixes;
    for (const auto& m : nondet_call().finditer(source)) suffixes.insert(m.group(1));
    for (const auto& suffix : suffixes) {
        std::string name = "__VERIFIER_nondet_" + suffix;
        if (defines(source, name)) continue;
        auto it = nondet_types().find(suffix);
        if (it == nondet_types().end()) throw std::invalid_argument("unsupported nondet type " + name);
        const auto& ctype = it->second;
        out.push_back(ctype + " " + name + "(void) { return (" + ctype + ")prism_take(); }");
    }
    if (source.find("__VERIFIER_assume") != std::string::npos && !defines(source, "__VERIFIER_assume"))
        out.emplace_back(
            "void __VERIFIER_assume(int c) { if (!c) { fprintf(stderr, \"PRISM-ASSUME-FAILED\\n\"); _exit(0); } }");
    if (source.find("reach_error") != std::string::npos && !defines(source, "reach_error"))
        out.emplace_back("void reach_error(void) { fprintf(stderr, \"PRISM-REACH-ERROR reach_error\\n\"); abort(); }");
    std::string text;
    for (std::size_t i = 0; i < out.size(); ++i) text += (i ? "\n" : "") + out.at(i);
    return text + "\n";
}

json replay(const fs::path& src, const std::string& prop, bool allow_exec,
            const std::optional<std::vector<Num>>& nondet_values, const fs::path& work_in,
            std::string_view data_model, double timeout_s) {
    const std::string text = read_text(src);
    if (nondet_call().search(text) && !nondet_values)
        return json{{"replay", "unsupported"},
                    {"why", "program reads __VERIFIER_nondet_* inputs and the engine reported no nondet values"}};
    if (!allow_exec)
        return json{{"replay", "notrun"}, {"why", "replay executes task code: re-run with --allow-exec (Law 9)"}};
    auto cc = Config{}.which({"clang", "gcc", "cc"});
    if (!cc) return json{{"replay", "notrun"}, {"why", "no C compiler (clang/gcc) on PATH"}};
    // shift-base: `x << n` whose result is not representable, and a negative
    // x (a report the overflow message filter does not accept: not an overflow).
    std::string san;
    if (prop == "no-overflow") san = "signed-integer-overflow,shift-base";
    else if (prop == "valid-memsafety") san = "address";
    std::error_code ec;
    fs::path work = fs::weakly_canonical(fs::absolute(work_in), ec);
    if (ec) work = fs::absolute(work_in);
    fs::create_directories(work, ec);
    const fs::path stubs = work / "prism_stubs.c";
    try {
        write_text(stubs, stub_source(text, nondet_values.value_or(std::vector<Num>{})));
    } catch (const std::invalid_argument& e) {
        return json{{"replay", "unsupported"}, {"why", e.what()}};
    }
    const fs::path csrc = work / (src.stem().string() + ".c");
    fs::copy_file(src, csrc, fs::copy_options::overwrite_existing, ec);
    const fs::path exe = work / "replay.bin";
    std::vector<std::string> argv = {cc->string(), "-g", "-O0", "-w", "-std=gnu11"};
    if (is_ilp32(data_model)) {
#ifndef _WIN32
        argv.push_back("-m32");
#else
        return json{{"replay", "notrun"},
                    {"why", "ILP32 replay needs a 32-bit C toolchain (-m32); not supported on this host"}};
#endif
    }
    if (!san.empty()) {
        argv.push_back("-fsanitize=" + san);
        argv.emplace_back("-fno-sanitize-recover=all");
    }
    argv.insert(argv.end(), {csrc.string(), stubs.string(), "-o", exe.string()});
    auto build = stages_detail::run_argv(argv, "", 120.0);
    if (build.rc != 0 || build.timeout) {
        std::string err = build.timeout ? "timed out after 120s" : build.err;
        if (err.size() > 300) err = err.substr(err.size() - 300);
        return json{{"replay", "not-replayed"}, {"why", "replay build failed: " + err}};
    }
    // The run's environment: sanitizers halt on the first report.
    detail::set_env("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1");
    detail::set_env("UBSAN_OPTIONS", "print_stacktrace=0:halt_on_error=1");
    const bool jailed = sandbox::kind() == "bwrap";
    auto run_argv = sandbox::wrap_argv({exe.string()}, work);
    // ASan reserves terabytes of shadow address space: no RLIMIT_AS for it
    auto p = stages_detail::run_argv(run_argv, "", timeout_s, sandbox::limits_for(timeout_s, san != "address"));
    if (p.timeout) return json{{"replay", "not-replayed"}, {"why", "program did not finish in " + float_repr(timeout_s) + "s"}};
    const std::string& err = p.err;
    json rec = {{"exit", p.rc}, {"sandbox", jailed ? "bwrap+rlimits" : "rlimits only (no bwrap)"}};
    if (classify_run(prop, err, p.rc, rec)) return rec;
    std::string tail = err.size() > 300 ? err.substr(err.size() - 300) : err;
    rec["replay"] = "not-replayed";
    rec["why"] = "the program ran without the violation: " + strip(tail);
    return rec;
}

namespace {

// Seeded input generator for fuzz_replay: every nondet call draws from a
// xorshift stream seeded by PRISM_FUZZ_SEED. Odd seeds draw mostly small
// values (loop counters), even seeds mix in type extremes.
std::string fuzz_stub_source(const std::string& source) {
    std::vector<std::string> out = {
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <unistd.h>",
        "static unsigned long long prism_s = 88172645463325252ULL; static unsigned long prism_n; static int prism_mode;",
        "__attribute__((constructor)) static void prism_init(void) { const char* e = getenv(\"PRISM_FUZZ_SEED\");",
        "  unsigned long long k = e ? strtoull(e, 0, 10) : 1; prism_mode = (int)(k % 2);",
        "  prism_s ^= (k + 1) * 0x9E3779B97F4A7C15ULL; if (!prism_s) prism_s = 1; }",
        "static unsigned long long prism_rnd(void) { prism_s ^= prism_s << 13; prism_s ^= prism_s >> 7; prism_s ^= prism_s << 17; return prism_s; }",
        "static long long prism_take(void) {",
        "  if (++prism_n > 1000000UL) { fprintf(stderr, \"PRISM-NONDET-EXHAUSTED\\n\"); _exit(0); }",
        "  unsigned long long r = prism_rnd(); unsigned c = (unsigned)(r % 16); long long v = (long long)(r >> 8);",
        "  if (prism_mode == 1 || c < 6) return (long long)(v % 21) - 4;",
        "  switch (c) { case 6: return 2147483647LL; case 7: return -2147483647LL - 1; case 8: return 4294967295LL;",
        "    case 9: return 9223372036854775807LL; case 10: return -9223372036854775807LL - 1; case 11: return 2147483646LL;",
        "    case 12: return (long long)(v % 1001); default: return v; } }",
    };
    std::set<std::string> suffixes;
    for (const auto& m : nondet_call().finditer(source)) suffixes.insert(m.group(1));
    for (const auto& suffix : suffixes) {
        std::string name = "__VERIFIER_nondet_" + suffix;
        if (defines(source, name)) continue;
        auto it = nondet_types().find(suffix);
        if (it == nondet_types().end()) throw std::invalid_argument("unsupported nondet type " + name);
        const auto& ctype = it->second;
        // integer conversion from long long is implementation-defined (modular), not UB.
        // Each value is logged (first 1000) for the witness's nondet waypoints.
        std::string fmt, arg;
        if (ctype == "float" || ctype == "double") fmt = "%.17g", arg = "(double)v";
        else if (ctype.starts_with("unsigned") || ctype == "_Bool") fmt = "%llu", arg = "(unsigned long long)v";
        else fmt = "%lld", arg = "(long long)v";
        out.push_back(ctype + " " + name + "(void) { " + ctype + " v = (" + ctype +
                      ")prism_take(); if (prism_n <= 1000) fprintf(stderr, \"PRISM-FUZZ-NONDET " + name + " " + fmt +
                      " %p\\n\", " + arg + ", __builtin_return_address(0)); return v; }");
    }
    if (source.find("__VERIFIER_assume") != std::string::npos && !defines(source, "__VERIFIER_assume"))
        out.emplace_back("void __VERIFIER_assume(int c) { if (!c) _exit(0); }");
    if (source.find("reach_error") != std::string::npos && !defines(source, "reach_error"))
        out.emplace_back("void reach_error(void) { fprintf(stderr, \"PRISM-REACH-ERROR reach_error\\n\"); abort(); }");
    std::string text;
    for (const auto& l : out) text += l + "\n";
    return text;
}

}  // namespace

json fuzz_replay(const fs::path& src, const std::string& prop, bool allow_exec, const fs::path& work_in,
                 std::string_view data_model, double budget_s, int max_seeds) {
    if (!allow_exec) return json{{"replay", "notrun"}, {"why", "fuzzing executes task code (Law 9)"}};
    if (prop != "no-overflow" && prop != "unreach-call")
        return json{{"replay", "unsupported"}, {"why", "fuzzing covers no-overflow and unreach-call"}};
    const std::string text = read_text(src);
    auto cc = Config{}.which({"clang", "gcc", "cc"});
    if (!cc) return json{{"replay", "notrun"}, {"why", "no C compiler (clang/gcc) on PATH"}};
    std::error_code ec;
    fs::path work = fs::absolute(work_in);
    fs::create_directories(work, ec);
    const fs::path stubs = work / "prism_fuzz_stubs.c";
    try {
        write_text(stubs, fuzz_stub_source(text));
    } catch (const std::invalid_argument& e) {
        return json{{"replay", "unsupported"}, {"why", e.what()}};
    }
    const fs::path csrc = work / (src.stem().string() + ".c");
    fs::copy_file(src, csrc, fs::copy_options::overwrite_existing, ec);
    const fs::path exe = work / "fuzz.bin";
    std::vector<std::string> argv = {cc->string(), "-g", "-O0", "-w", "-std=gnu11"};
    if (is_ilp32(data_model)) {
#ifndef _WIN32
        argv.push_back("-m32");
#else
        return json{{"replay", "notrun"}, {"why", "ILP32 fuzzing needs -m32"}};
#endif
    }
    argv.emplace_back("-no-pie");  // fixed code addresses: call sites are symbolized after the run
    if (prop == "no-overflow") {
        argv.emplace_back("-fsanitize=signed-integer-overflow,shift-base");
        argv.emplace_back("-fno-sanitize-recover=all");
    }
    argv.insert(argv.end(), {csrc.string(), stubs.string(), "-o", exe.string()});
    auto build = stages_detail::run_argv(argv, "", 120.0);
    if (build.rc != 0 || build.timeout)
        return json{{"replay", "not-replayed"}, {"why", "fuzz build failed"}};
    detail::set_env("UBSAN_OPTIONS", "print_stacktrace=0:halt_on_error=1");
    const auto t0 = std::chrono::steady_clock::now();
    int runs = 0, timeouts = 0;
    // Every run timing out at 2 s (a long deterministic loop such as
    // nested_1-2, 2.7e9 iterations at -O0) gets one long run of seed 1 within
    // the remaining budget. Same sanitizer check, so never weaker evidence.
    bool long_run = false;
    for (int seed = 1; seed <= max_seeds; ++seed) {
        double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (used > budget_s) break;
        if (runs >= 3 && timeouts == runs && !long_run) {
            long_run = true;
            seed = 1;
        }
        const double lim = long_run && seed == 1 ? std::max(2.0, std::min(30.0, budget_s - used)) : 2.0;
        detail::set_env("PRISM_FUZZ_SEED", std::to_string(seed).c_str());
        auto run_argv = sandbox::wrap_argv({exe.string()}, work);
        const auto r0 = std::chrono::steady_clock::now();
        auto p = stages_detail::run_argv(run_argv, "", lim, sandbox::limits_for(lim, true));
        ++runs;
        // a CPU-limit kill (SIGXCPU) can beat the wall timeout: count it too
        const bool hit_limit = std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count() >= 0.9 * lim;
        if (p.timeout || hit_limit) {
            ++timeouts;
            continue;
        }
        json rec = {{"exit", p.rc}, {"fuzz_seed", seed}};
        if (classify_run(prop, p.err, p.rc, rec)) {
            static const Regex nd(R"(^PRISM-FUZZ-NONDET (\S+) (\S+) (0x[0-9a-fA-F]+)$)", true);
            std::string trace, addrs;
            std::size_t n = 0;
            for (const auto& m : nd.finditer(p.err)) {
                trace += (trace.empty() ? "" : ", ") + m.group(1) + "=" + m.group(2);
                // the return address is just past the call: symbolize the byte before it
                unsigned long long a = std::strtoull(m.group(3).c_str(), nullptr, 16);
                char buf[32];
                std::snprintf(buf, sizeof buf, "0x%llx\n", a ? a - 1 : 0);
                addrs += buf;
                ++n;
            }
            rec["nondet"] = trace;
            // call sites (line:col) of each value; dropped unless every one resolves in the task file
            if (auto sym = Config{}.which({"llvm-symbolizer", "llvm-symbolizer-18"}); sym && n) {
                auto q = stages_detail::run_argv({sym->string(), "--obj=" + exe.string(), "--no-inlines"}, addrs, 30.0);
                static const Regex loc(R"(^(\S+):(\d+):(\d+)$)", true);
                std::string locs;
                std::size_t k = 0;
                const std::string base = csrc.filename().string();
                for (const auto& m : loc.finditer(q.out)) {
                    if (fs::path(m.group(1)).filename().string() != base) break;
                    locs += (locs.empty() ? "" : ", ") + m.group(2) + ":" + m.group(3);
                    ++k;
                }
                if (q.rc == 0 && k == n) rec["nondet_loc"] = locs;
            }
            rec["detail"] = get_str(rec, "detail") + " (fuzz seed " + std::to_string(seed) + ")";
            return rec;
        }
    }
    return json{{"replay", "not-replayed"}, {"why", "no violation in " + std::to_string(runs) + " fuzz runs"}};
}

}  // namespace prism::svcomp
