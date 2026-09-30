// Stage execute: concrete replay of FAILED/CRASH counterexamples and the
// LLM interpreter loop (execute_cex).
#include "agent.hpp"
#include "interp.hpp"
#include "llm.hpp"

#include <climits>

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
const char* SYSTEM_HARNESS =
    "Write a complete C file that includes the target as a string in comments "
    "and a main() that tests the stated property. No markdown.";

// A counterexample value the way the reference engine reads it (Python's
// int(v, 0)): optional sign, 0x / 0o / 0b prefixes, single underscores
// between digits; a decimal with a leading zero ("010") is refused, not read
// as octal. nullopt when refused or beyond 64 bits.
std::optional<long long> parse_cex_int(std::string v) {
    v = strip(v);
    if (v.empty()) return std::nullopt;
    bool neg = false;
    std::size_t i = 0;
    if (v[i] == '+' || v[i] == '-') neg = v[i++] == '-';
    int base = 10;
    if (i + 1 < v.size() && v[i] == '0') {
        const char p = static_cast<char>(std::tolower(static_cast<unsigned char>(v[i + 1])));
        if (p == 'x') base = 16;
        else if (p == 'o') base = 8;
        else if (p == 'b') base = 2;
        if (base != 10) {
            i += 2;
            if (i < v.size() && v[i] == '_') ++i;  // 0x_1f
        }
    }
    const std::size_t start = i;
    if (start >= v.size()) return std::nullopt;
    unsigned long long acc = 0;
    bool any = false, prev_us = false, nonzero = false;
    for (; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '_') {
            if (!any || prev_us) return std::nullopt;
            prev_us = true;
            continue;
        }
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        if (d >= base) return std::nullopt;
        if (acc > (ULLONG_MAX - static_cast<unsigned>(d)) / static_cast<unsigned>(base)) return std::nullopt;
        acc = acc * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        nonzero = nonzero || d != 0;
        any = true;
        prev_us = false;
    }
    if (!any || prev_us) return std::nullopt;
    if (base == 10 && v[start] == '0' && nonzero) return std::nullopt;  // "010"
    if (neg) {
        if (acc > static_cast<unsigned long long>(LLONG_MAX) + 1) return std::nullopt;
        return acc == static_cast<unsigned long long>(LLONG_MAX) + 1 ? LLONG_MIN : -static_cast<long long>(acc);
    }
    if (acc > static_cast<unsigned long long>(LLONG_MAX)) return std::nullopt;
    return static_cast<long long>(acc);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string t; in >> t;) out.push_back(t);
    return out;
}

}  // namespace

namespace stages_detail {

std::string sandbox_verdict(const nlohmann::json& result) {
    auto err = sandbox_err(result);
    if (err == "no compiler" || err == "exec-disabled") return std::string(laws::NOTRUN);
    if (result.value("ok", false)) return std::string(laws::CLEAN);
    if (err == "crash") return std::string(laws::CRASH);
    return std::string(laws::FAILED);
}

std::vector<Finding> interpreter_loop(LlamaEngine& engine, const std::string& prompt, int rounds,
                                      bool allow_exec) {
    if (!engine.available()) {
        auto f = nr("execute", LLM_UNAVAILABLE_MSG);
        f.extra["install"] = LLM_INSTALL;
        return {f};
    }
    if (!which_cc()) {
        auto f = nr("execute", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
        f.extra["install"] = "install gcc or clang";
        return {f};
    }
    if (!allow_exec)
        return {sandbox::exec_notrun("execute", "execute (LLM-written C)", {{"half", "llm"}})};
    std::vector<std::pair<std::string, std::string>> messages{{"system", SYSTEM_HARNESS}, {"user", prompt}};
    std::string last_src;
    bool ran = false;
    for (int i = 0; i < rounds; ++i) {
        auto r = engine.complete(messages, 120);
        if (!r.error.empty()) {
            if (llm_httpish(r.error)) {
                auto f = make_find("execute", laws::NOTRUN, FunctionInfo{}, "", r.error,
                                   laws::STRENGTH_READS);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                return {f};
            }
            return {make_find("execute", laws::ERROR, FunctionInfo{}, "", r.error, laws::STRENGTH_READS)};
        }
        auto src = c_from_llm(r.text);
        if (src.empty()) {
            messages.push_back({"assistant", r.text});
            messages.push_back({"user", "Reply with a complete C file only."});
            continue;
        }
        last_src = src;
        auto result = sandbox_run(src, 8.0, /*allow_exec=*/true);
        ran = true;
        auto verdict = sandbox_verdict(result);
        if (verdict == laws::NOTRUN) {
            auto f = nr("execute", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
            f.extra["install"] = "install gcc or clang";
            return {f};
        }
        if (verdict == laws::CLEAN) {
            Finding f;
            f.stage = "execute";
            f.status = std::string(laws::CLEAN);
            f.message = "interpreter harness passed on round " + std::to_string(i + 1) + " (not a proof)";
            f.strength = std::string(laws::STRENGTH_FINDS);
            f.extra["stdout"] = tail(result.value("stdout", std::string{}), 400);
            f.extra["rounds"] = std::to_string(i + 1);
            f.extra["sandbox"] = result.value("sandbox", std::string{});
            return {f};
        }
        if (verdict == laws::CRASH) {
            Finding f;
            f.stage = "execute";
            f.status = std::string(laws::CRASH);
            f.message = "sandbox crash on round " + std::to_string(i + 1) + " (not a proof)";
            f.strength = std::string(laws::STRENGTH_FINDS);
            f.extra["stderr"] = tail(result.value("stderr", std::string{}), 400);
            f.extra["rounds"] = std::to_string(i + 1);
            f.extra["code"] = result.contains("code") ? result["code"].dump() : "null";
            f.extra["sandbox"] = result.value("sandbox", std::string{});
            return {f};
        }
        messages.push_back({"assistant", r.text});
        messages.push_back({"user", "execution failed: " + result.dump().substr(0, 1500) + ". Fix the C file."});
    }
    if (!ran) {
        Finding f;
        f.stage = "execute";
        f.status = std::string(laws::HYPOTHESIS);
        f.message = "LLM produced no C to run";
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    Finding f;
    f.stage = "execute";
    f.status = std::string(laws::FAILED);
    f.message = "interpreter loop exhausted (" + std::to_string(rounds) + " rounds)";
    f.strength = std::string(laws::STRENGTH_FINDS);
    f.extra["last_src"] = last_src.substr(0, 500);
    return {f};
}

}  // namespace stages_detail

std::vector<Finding> execute_cex(const std::vector<Finding>& fails, const std::vector<FunctionInfo>& functions,
                                 const Config& cfg) {
    std::vector<Finding> ordered = fails;
    std::sort(ordered.begin(), ordered.end(), [](const Finding& a, const Finding& b) {
        int ka = a.status == laws::CRASH ? 0 : 1;
        int kb = b.status == laws::CRASH ? 0 : 1;
        if (ka != kb) return ka < kb;
        return a.stage < b.stage;
    });
    std::vector<Finding> out;
    int n = 0;
    for (auto& f0 : ordered) {
        if (n >= 16) break;
        const FunctionInfo* fn = nullptr;
        for (auto& x : functions) {
            if (!f0.function) break;
            if (x.name == *f0.function &&
                (x.file == f0.file || fs::path(x.file).filename() == fs::path(f0.file).filename())) {
                fn = &x;
                break;
            }
        }
        if (!fn && f0.function) {
            for (auto& x : functions)
                if (x.name == *f0.function) {
                    fn = &x;
                    break;
                }
        }
        if (!fn) continue;
        if (fn->kind == "POINTER" || fn->kind == "OTHER") {
            auto f = make_find("execute", laws::NEEDS_HARNESS, *fn, "",
                               fn->kind + ": cex replay would invent a buffer or object",
                               laws::STRENGTH_FINDS);
            f.extra["oracle"] = "concrete-replay";
            out.push_back(std::move(f));
            continue;
        }
        if (float_unencoded(*fn)) {
            auto f = make_find("execute", laws::NEEDS_HARNESS, *fn, "",
                               "float/double unencoded: cex replay oracle is not an IEEE model",
                               laws::STRENGTH_FINDS);
            f.extra["oracle"] = "concrete-replay";
            out.push_back(std::move(f));
            continue;
        }
        Args args;
        auto blob = f0.counterexample;
        for (char& c : blob)
            if (c == ';') c = ',';
        std::istringstream ss(blob);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto eq = part.find('=');
            if (eq == std::string::npos) continue;
            const auto kt = split_ws(part.substr(0, eq)), vt = split_ws(part.substr(eq + 1));
            if (kt.empty() || vt.empty()) continue;
            // The interpreter's arguments are 32-bit: a value that is no
            // 32-bit bit pattern (signed or unsigned) is not replayed.
            auto v = parse_cex_int(vt[0]);
            if (!v || *v < INT_MIN || *v > static_cast<long long>(UINT_MAX)) continue;
            args[kt.back()] = static_cast<int>(static_cast<std::uint32_t>(*v));
        }
        if (args.empty()) continue;
        ++n;
        auto rec = execute(*fn, args);
        Finding f;
        f.stage = "execute";
        f.file = fn->file;
        f.function = fn->name;
        f.line = fn->line;
        f.strength = std::string(laws::STRENGTH_FINDS);
        f.extra["oracle"] = "concrete-replay";
        if (!rec.ub.empty()) {
            f.status = std::string(laws::CRASH);
            f.cls = rec.ub;
            f.message = "cex replay trapped " + rec.ub;
            f.counterexample = f0.counterexample.substr(0, 200);
        } else {
            f.status = std::string(laws::CLEAN);
            f.message = "cex did not trap in concrete replay (not a proof)";
        }
        out.push_back(std::move(f));
    }
    if (cfg.llm && !fails.empty()) {
        LlamaEngine engine(cfg);
        // Python engine execute_cex: LLM half down is NOTRUN with extra.half=llm, never CLEAN.
        if (!engine.available()) {
            auto f = nr("execute", LLM_UNAVAILABLE_MSG);
            f.extra["install"] = LLM_INSTALL;
            f.extra["half"] = "llm";
            out.push_back(std::move(f));
        } else {
            auto& f0 = fails[0];
            std::string prompt = "Write a C main() that demonstrates this finding is real or not.\n" + f0.file +
                                 ":" + (f0.line ? std::to_string(*f0.line) : "0") + " " +
                                 (f0.function ? *f0.function : "") + " " + f0.cls + ": " + f0.message +
                                 "\ncounterexample: " + f0.counterexample;
            auto extra = interpreter_loop(engine, prompt, 2, cfg.allow_exec);
            out.insert(out.end(), extra.begin(), extra.end());
        }
    }
    if (out.empty()) return {nr("execute", "no FAILED/CRASH cex to replay")};
    return out;
}

}  // namespace prism
