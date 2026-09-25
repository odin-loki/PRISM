// Stage execute: concrete replay of FAILED/CRASH counterexamples and the
// LLM interpreter loop (execute_cex).
#include "interp.hpp"
#include "llm.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
const char* SYSTEM_HARNESS =
    "Write a complete C file that includes the target as a string in comments "
    "and a main() that tests the stated property. No markdown.";

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
            f.extra["stdout"] = result.value("stdout", std::string{}).substr(0, 400);
            f.extra["rounds"] = std::to_string(i + 1);
            return {f};
        }
        if (verdict == laws::CRASH) {
            Finding f;
            f.stage = "execute";
            f.status = std::string(laws::CRASH);
            f.message = "sandbox crash on round " + std::to_string(i + 1) + " (not a proof)";
            f.strength = std::string(laws::STRENGTH_FINDS);
            f.extra["stderr"] = result.value("stderr", std::string{}).substr(0, 400);
            f.extra["rounds"] = std::to_string(i + 1);
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

}  // namespace

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
            auto k = strip(part.substr(0, eq));
            auto sp = k.find_last_of(" \t");
            if (sp != std::string::npos) k = k.substr(sp + 1);
            auto v = strip(part.substr(eq + 1));
            auto sp2 = v.find(' ');
            if (sp2 != std::string::npos) v = v.substr(0, sp2);
            try {
                args[k] = std::stoi(v, nullptr, 0);
            } catch (...) {
            }
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
