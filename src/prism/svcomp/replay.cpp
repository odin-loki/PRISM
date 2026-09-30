// Counterexample replay (`prism svcomp`): the task is compiled with the
// property's sanitizer and run on the refutation's nondet values; the
// violation has to show. Running task code needs --allow-exec (Law 9), and
// the run is sandboxed (bwrap when it works here, rlimits always).
#include "internal.hpp"

#include "../stages/common.hpp"
#include "prism/config.hpp"
#include "prism/sandbox.hpp"

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

// int(text, 0): an optional sign, then a 0x/0o/0b prefix or a decimal
// without leading zeros; single underscores between digits.
std::optional<__int128> parse_int0(const std::string& t) {
    std::size_t i = 0;
    bool neg = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    int base = 10;
    if (i + 1 < t.size() && t[i] == '0') {
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(t[i + 1])));
        if (p == 'x') base = 16;
        else if (p == 'o') base = 8;
        else if (p == 'b') base = 2;
        if (base != 10) {
            i += 2;
            if (i < t.size() && t[i] == '_') ++i;  // 0x_ff
        }
    }
    if (i >= t.size()) return std::nullopt;
    const std::size_t first = i;
    unsigned __int128 v = 0;
    const unsigned __int128 limit = (static_cast<unsigned __int128>(1) << 127);
    bool nonzero_lead = false;
    for (; i < t.size(); ++i) {
        char c = t[i];
        if (c == '_') {
            if (i == first || i + 1 >= t.size() || t[i + 1] == '_') return std::nullopt;
            continue;
        }
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        if (d >= base) return std::nullopt;
        if (base == 10 && i == first && d != 0) nonzero_lead = true;
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        if (v > limit) return std::nullopt;
    }
    if (base == 10 && !nonzero_lead) {
        // "0", "00", "0_0" are 0; "010" is not an int(text, 0) literal
        for (std::size_t k = first; k < t.size(); ++k)
            if (t[k] != '0' && t[k] != '_') return std::nullopt;
    }
    if (!neg && v == limit) return std::nullopt;
    __int128 s = neg ? -static_cast<__int128>(v - 1) - 1 : static_cast<__int128>(v);
    if (neg && v == 0) s = 0;
    return s;
}

// float(text): decimal floats, inf/infinity, nan; no hex floats.
std::optional<double> parse_float(const std::string& t) {
    if (t.empty()) return std::nullopt;
    std::string c;
    for (std::size_t k = 0; k < t.size(); ++k) {
        char ch = t[k];
        if (ch == 'x' || ch == 'X' || ch == '(') return std::nullopt;
        if (ch == '_') {
            bool ok = k > 0 && k + 1 < t.size() && std::isdigit(static_cast<unsigned char>(t[k - 1])) &&
                      std::isdigit(static_cast<unsigned char>(t[k + 1]));
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
            if (auto iv = parse_int0(val)) out.emplace_back(name, Num::of_int(*iv));
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
        vals += float_repr(nondet_values[i].as_double());
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
    for (std::size_t i = 0; i < out.size(); ++i) text += (i ? "\n" : "") + out[i];
    return text + "\n";
}

json replay(const fs::path& src, const std::string& prop, bool allow_exec,
            const std::optional<std::vector<Num>>& nondet_values, const fs::path& work_in, double timeout_s) {
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
    ::setenv("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1", 1);
    ::setenv("UBSAN_OPTIONS", "print_stacktrace=0:halt_on_error=1", 1);
    const bool jailed = sandbox::kind() == "bwrap";
    auto run_argv = sandbox::wrap_argv({exe.string()}, work);
    // ASan reserves terabytes of shadow address space: no RLIMIT_AS for it
    auto p = stages_detail::run_argv(run_argv, "", timeout_s, sandbox::limits_for(timeout_s, san != "address"));
    if (p.timeout) return json{{"replay", "not-replayed"}, {"why", "program did not finish in " + float_repr(timeout_s) + "s"}};
    const std::string& err = p.err;
    json rec = {{"exit", p.rc}, {"sandbox", jailed ? "bwrap+rlimits" : "rlimits only (no bwrap)"}};
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
                return rec;
            }
        }
    } else if (prop == "unreach-call") {
        if (err.find("reach_error") != std::string::npos && p.rc != 0) {
            // The assertion message names reach_error's own line, not the call
            // site; the witness target comes from reach_error_call_site().
            rec["replay"] = "replayed";
            rec["detail"] = "reach_error() was called and the program aborted";
            return rec;
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
                return rec;
            }
        }
    }
    std::string tail = err.size() > 300 ? err.substr(err.size() - 300) : err;
    rec["replay"] = "not-replayed";
    rec["why"] = "the program ran without the violation: " + strip(tail);
    return rec;
}

}  // namespace prism::svcomp
