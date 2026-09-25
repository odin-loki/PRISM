// Stage fuse: concrete and binary fuzzing (fuzz_function), AFL++ and
// libFuzzer engines, Fuzz4All / ChatFuzz LLM seeds and mutants (run_fuse).
#include "interp.hpp"
#include "llm.hpp"
#include "prism/simd.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifdef min
#    undef min
#  endif
#  ifdef max
#    undef max
#  endif
#  ifdef ERROR
#    undef ERROR
#  endif
#  ifdef OPTIONAL
#    undef OPTIONAL
#  endif
#  ifdef CONST
#    undef CONST
#  endif
#endif

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
#include "../bmc_unenc.inc"

std::optional<std::vector<uint8_t>> bytes_from_cex(const std::string& cex, int nbytes) {
    if (cex.empty()) return std::nullopt;
    std::vector<int> vals;
    std::string tmp = cex;
    std::istringstream ss(tmp);
    std::string part;
    while (std::getline(ss, part, ',')) {
        auto eq = part.find('=');
        if (eq == std::string::npos) continue;
        auto v = strip(part.substr(eq + 1));
        try {
            vals.push_back(std::stoi(v, nullptr, 0));
        } catch (...) {
            return std::nullopt;
        }
    }
    std::vector<uint8_t> raw;
    for (int v : vals) {
        uint32_t u = static_cast<uint32_t>(v);
        raw.push_back(static_cast<uint8_t>(u));
        raw.push_back(static_cast<uint8_t>(u >> 8));
        raw.push_back(static_cast<uint8_t>(u >> 16));
        raw.push_back(static_cast<uint8_t>(u >> 24));
    }
    if (static_cast<int>(raw.size()) > nbytes) raw.resize(static_cast<std::size_t>(nbytes));
    while (static_cast<int>(raw.size()) < nbytes) raw.push_back(0);
    return raw;
}

std::string afl_harness_source(const FunctionInfo& fn, std::string src_rel);

std::pair<bool, std::string> compile_afl_harness(const fs::path& harness, const fs::path& exe);

Finding fuzz_function(const FunctionInfo& fn, const fs::path& src, double budget, int iters,
                      const std::vector<std::vector<uint8_t>>* seeds) {
    if (fn.kind == "POINTER")
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "",
                         "POINTER: no honest fuzzer harness (would invent a buffer or pass NULL)",
                         laws::STRENGTH_FINDS);
    if (fn.kind == "OTHER")
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "", "OTHER signature, not harnessed",
                         laws::STRENGTH_FINDS);
    if (auto syn = unencoded_syntax_reason(fn, "fuzzer"))
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "", *syn, laws::STRENGTH_FINDS);
    if (body_needs_pointer_harness(fn.body))
        return make_find("fuzz", laws::NEEDS_HARNESS, fn, "",
                         "local pointer or heap object: fuzzer would invent a buffer", laws::STRENGTH_FINDS);
    int nbytes = param_nbytes(fn.params);
    std::vector<std::vector<uint8_t>> corpus;
    for (auto& s : interesting_seeds(fn)) {
        auto b = s;
        if (static_cast<int>(b.size()) > nbytes) b.resize(static_cast<std::size_t>(nbytes));
        while (static_cast<int>(b.size()) < nbytes) b.push_back(0);
        corpus.push_back(std::move(b));
    }
    if (seeds)
        for (auto s : *seeds) {
            if (static_cast<int>(s.size()) > nbytes) s.resize(static_cast<std::size_t>(nbytes));
            while (static_cast<int>(s.size()) < nbytes) s.push_back(0);
            corpus.push_back(std::move(s));
        }
    if (corpus.empty()) {
        std::vector<uint8_t> rnd(static_cast<std::size_t>(nbytes));
        std::mt19937 rng{std::random_device{}()};
        for (auto& b : rnd) b = static_cast<uint8_t>(rng());
        corpus.push_back(std::move(rnd));
    }
    bool noseed = !seeds;
    auto t0 = std::chrono::steady_clock::now();
    std::set<uint64_t> seen;
    int new_cov = 0, stall = 0, i = 0;
    auto note_cov = [&](const std::vector<uint8_t>& child) {
        auto h = coverage_hash(child.data(), child.size());
        if (!seen.contains(h)) {
            seen.insert(h);
            corpus.push_back(child);
            ++new_cov;
            stall = 0;
        } else {
            ++stall;
        }
    };
    auto crash_from = [&](const std::vector<uint8_t>& child, const std::map<std::string, int>& args,
                          const std::string& cls, int ii) {
        std::string argstr;
        for (auto& [k, v] : args) {
            if (!argstr.empty()) argstr += ", ";
            argstr += k + "=" + std::to_string(v);
        }
        std::string hex;
        for (std::size_t k = 0; k < child.size() && k < 16; ++k) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "%02x", child[k]);
            hex += buf;
        }
        auto f = make_find("fuzz", laws::CRASH, fn, cls, cls + " on " + hex + " " + argstr, laws::STRENGTH_FINDS);
        f.counterexample = hex + " " + argstr;
        f.extra["iters"] = std::to_string(ii);
        f.extra["corpus"] = std::to_string(corpus.size());
        f.extra["new_cov"] = std::to_string(new_cov);
        f.extra["noseed"] = noseed ? "true" : "false";
        f.extra["oracle"] = "concrete";
        return f;
    };
    std::deque<std::vector<uint8_t>> queue(corpus.begin(), corpus.end());
    while (!queue.empty()) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= std::max(budget, 1.0)) break;
        auto child = queue.front();
        queue.pop_front();
        ++i;
        auto args = decode_args(fn, child);
        auto rec = execute(fn, args);
        if (!rec.ub.empty()) return crash_from(child, args, rec.ub, i);
        note_cov(child);
    }
    uint64_t hseed = 1;
    while (i < iters) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= budget) break;
        auto parent = corpus[static_cast<std::size_t>(i) % corpus.size()];
        auto child = parent;
        havoc(child.data(), child.size(), ++hseed);
        ++i;
        auto args = decode_args(fn, child);
        auto rec = execute(fn, args);
        if (!rec.ub.empty()) return crash_from(child, args, rec.ub, i);
        note_cov(child);
    }
    auto extra_iters = std::to_string(i);
    auto extra_corpus = std::to_string(corpus.size());
    auto extra_cov = std::to_string(new_cov);
    auto extra_noseed = noseed ? "true" : "false";
    auto extra_stall = std::to_string(stall);
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double remain = std::max(0.05, budget - elapsed);
    int bin_iters = std::min(32, std::max(1, iters));
    // Law 9: the compiled harness runs the scanned function; without
    // --allow-exec only the concrete oracle above ran.
    const bool exec_ok = sandbox::allowed();
    auto cc = exec_ok ? Config{}.which({"gcc", "clang"}) : std::nullopt;
    if (cc && fs::exists(src)) {
        auto td = fs::temp_directory_path() / ("prism_fuzzbin_" + std::to_string(std::random_device{}()));
        fs::create_directories(td);
        struct Guard {
            fs::path p;
            ~Guard() {
                std::error_code ec;
                fs::remove_all(p, ec);
            }
        } guard{td};
        auto src_copy = td / src.filename();
        try {
            std::ofstream out(src_copy);
            out << read_text_file(src);
        } catch (...) {
            cc.reset();
        }
        if (cc) {
            auto hpath = td / ("harness_" + fn.name + ".c");
            {
                std::ofstream out(hpath);
                out << afl_harness_source(fn, src.filename().string());
            }
            auto exe = td / ("harness_" + fn.name + ".exe");
            auto [ok, err] = compile_afl_harness(hpath, exe);
            if (!ok) {
                (void)err;
                auto f = make_find("fuzz", laws::CLEAN, fn, "",
                                   "no crash in " + extra_iters + " iters / " +
                                       std::format("{:.1f}", budget) + "s (not a proof)",
                                   laws::STRENGTH_FINDS);
                f.extra["iters"] = extra_iters;
                f.extra["corpus"] = extra_corpus;
                f.extra["new_cov"] = extra_cov;
                f.extra["noseed"] = extra_noseed;
                f.extra["stall"] = extra_stall;
                f.extra["oracle"] = "concrete";
                f.extra["binary"] = "compile-failed";
                return f;
            }
            auto bin_crash = [&](const std::vector<uint8_t>& child, const std::string& detail, int n) {
                auto args = decode_args(fn, child);
                std::string hex;
                for (std::size_t k = 0; k < child.size() && k < 16; ++k) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%02x", child[k]);
                    hex += buf;
                }
                auto f = make_find("fuzz", laws::CRASH, fn, "FUZZ-CRASH", "crash on " + hex + "…",
                                   laws::STRENGTH_FINDS);
                f.extra["sandbox"] = sandbox::kind();
                std::string full;
                for (auto b : child) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%02x", b);
                    full += buf;
                }
                f.counterexample = full;
                f.evidence = detail.substr(0, 800);
                f.extra["iters"] = extra_iters;
                f.extra["corpus"] = extra_corpus;
                f.extra["new_cov"] = extra_cov;
                f.extra["noseed"] = extra_noseed;
                f.extra["stall"] = extra_stall;
                f.extra["oracle"] = "binary";
                f.extra["binary_iters"] = std::to_string(n);
                std::string argstr;
                for (auto& [k, v] : args) {
                    if (!argstr.empty()) argstr += ", ";
                    argstr += k + "=" + std::to_string(v);
                }
                f.extra["args"] = argstr;
                return f;
            };
            auto is_bin_crash = [&](const ProcRun& rr) {
                if (rr.timeout) return false;
                if (rr.crashed || rr.rc < 0) return true;
                auto low = lower_copy(rr.err);
                return low.find("runtime error") != std::string::npos ||
                       low.find("undefinedbehaviorsanitizer") != std::string::npos ||
                       low.find("addresssanitizer") != std::string::npos ||
                       low.find("heap-buffer-overflow") != std::string::npos ||
                       low.find("heap-use-after-free") != std::string::npos;
            };
            auto tbin = std::chrono::steady_clock::now();
            int n = 0;
            int take = std::max(1, std::min(bin_iters, static_cast<int>(corpus.size())));
            for (int k = 0; k < take; ++k) {
                auto elapsed_bin =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tbin).count();
                if (elapsed_bin > remain) break;
                ++n;
                std::string in(corpus[static_cast<std::size_t>(k)].begin(),
                               corpus[static_cast<std::size_t>(k)].end());
                auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), in, 1.0,
                                   sandbox::limits_for(1.0, /*limit_as=*/false));
                if (is_bin_crash(rr))
                    return bin_crash(corpus[static_cast<std::size_t>(k)], rr.err, n);
            }
            uint64_t hbin = 91;
            while (n < bin_iters) {
                auto elapsed_bin =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tbin).count();
                if (elapsed_bin >= remain) break;
                auto child = corpus[static_cast<std::size_t>(n) % corpus.size()];
                havoc(child.data(), child.size(), ++hbin);
                ++n;
                std::string in(child.begin(), child.end());
                auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), in, 1.0,
                                   sandbox::limits_for(1.0, /*limit_as=*/false));
                if (is_bin_crash(rr)) return bin_crash(child, rr.err, n);
            }
            auto f = make_find("fuzz", laws::CLEAN, fn, "",
                               "no crash in " + extra_iters + " iters / " +
                                   std::format("{:.1f}", budget) + "s (not a proof)",
                               laws::STRENGTH_FINDS);
            f.extra["iters"] = extra_iters;
            f.extra["corpus"] = extra_corpus;
            f.extra["new_cov"] = extra_cov;
            f.extra["noseed"] = extra_noseed;
            f.extra["stall"] = extra_stall;
            f.extra["oracle"] = "concrete";
            f.extra["binary_iters"] = std::to_string(n);
            f.extra["sandbox"] = sandbox::kind();
            return f;
        }
    }
    auto f = make_find("fuzz", laws::CLEAN, fn, "",
                       "no crash in " + extra_iters + " iters / " +
                           std::format("{:.1f}", budget) + "s (not a proof)",
                       laws::STRENGTH_FINDS);
    f.extra["iters"] = extra_iters;
    f.extra["corpus"] = extra_corpus;
    f.extra["new_cov"] = extra_cov;
    f.extra["noseed"] = extra_noseed;
    f.extra["stall"] = extra_stall;
    f.extra["oracle"] = "concrete";
    if (!exec_ok) {
        f.extra["binary"] = std::string(laws::NOTRUN);
        f.extra["exec"] = std::string(laws::NOTRUN);
    }
    return f;
}

std::string norm_ws(std::string_view s) {
    std::string n;
    bool sp = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!sp && !n.empty()) {
                n.push_back(' ');
                sp = true;
            }
        } else {
            n.push_back(c);
            sp = false;
        }
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    return n;
}

void add_goal(std::vector<std::string>& seen, std::string cond) {
    cond = norm_ws(cond);
    if (!cond.empty() && std::find(seen.begin(), seen.end(), cond) == seen.end())
        seen.push_back(std::move(cond));
}

std::vector<std::string> branch_goals(const FunctionInfo& fn) {
    // FuSeBMC MyVisitor::check / checkStmt: then, implicit else, loop-exit, switch cases.
    static Regex if_re("\\bif\\s*\\(([^)]+)\\)");
    static Regex while_re("\\bwhile\\s*\\(([^)]+)\\)");
    static Regex for_re("\\bfor\\s*\\(([^)]*)\\)");
    static Regex switch_re("\\bswitch\\s*\\(([^)]+)\\)");
    static Regex case_re("\\bcase\\s+([^:]+):");
    std::vector<std::string> seen;
    for (auto& m : if_re.finditer(fn.body)) {
        auto cond = norm_ws(m.group(1));
        if (cond.empty()) continue;
        add_goal(seen, cond);
        add_goal(seen, "!(" + cond + ")");
    }
    for (auto& m : while_re.finditer(fn.body)) {
        auto cond = norm_ws(m.group(1));
        if (cond.empty()) continue;
        add_goal(seen, cond);
        add_goal(seen, "!(" + cond + ")");
    }
    for (auto& m : for_re.finditer(fn.body)) {
        auto inner = m.group(1);
        auto semi = inner.find(';');
        if (semi == std::string::npos) continue;
        auto semi2 = inner.find(';', semi + 1);
        auto mid = inner.substr(semi + 1, semi2 == std::string::npos ? std::string::npos : semi2 - semi - 1);
        auto cond = norm_ws(mid);
        if (cond.empty()) continue;
        add_goal(seen, cond);
        add_goal(seen, "!(" + cond + ")");
    }
    auto switches = switch_re.finditer(fn.body);
    for (std::size_t i = 0; i < switches.size(); ++i) {
        auto expr = norm_ws(switches[i].group(1));
        if (expr.empty()) continue;
        std::size_t from = switches[i].spans.empty() ? 0 : static_cast<std::size_t>(std::max(0, switches[i].spans[0].second));
        std::size_t to = fn.body.size();
        if (i + 1 < switches.size() && !switches[i + 1].spans.empty() && switches[i + 1].spans[0].first >= 0)
            to = static_cast<std::size_t>(switches[i + 1].spans[0].first);
        if (from > to) from = to;
        auto rest = fn.body.substr(from, to - from);
        for (auto& cm : case_re.finditer(rest)) {
            auto lab = norm_ws(cm.group(1));
            if (!lab.empty()) add_goal(seen, "(" + expr + ") == (" + lab + ")");
        }
    }
    return seen;
}

std::vector<std::pair<std::string, std::string>> numbered_goals(const FunctionInfo& fn) {
    // FuSeBMC GoalCounter::GetNewGoalForFunc: ++counter, "GOAL_" + counter.
    auto conds = branch_goals(fn);
    std::vector<std::pair<std::string, std::string>> out;
    unsigned long long counter = 0;
    for (auto& c : conds) {
        ++counter;
        out.emplace_back("GOAL_" + std::to_string(counter), c);
    }
    return out;
}

std::vector<std::vector<uint8_t>> seeds_from_bmc(const FunctionInfo& fn, const std::vector<Finding>& bmc_findings) {
    int n = param_nbytes(fn.params);
    std::vector<std::vector<uint8_t>> out;
    for (auto& f : bmc_findings) {
        if (!f.function || *f.function != fn.name) continue;
        if (f.status != laws::FAILED) continue;
        auto b = bytes_from_cex(f.counterexample, n);
        if (b) out.push_back(*b);
    }
    return out;
}

bool llama_engine_up(LlamaEngine* e);

std::vector<std::vector<uint8_t>> fuzz4all_seeds(LlamaEngine& engine, const FunctionInfo& fn, int nbytes);

std::vector<std::vector<uint8_t>> chatfuzz_mutants(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex);

std::vector<std::vector<uint8_t>> fuzz4all_mutate_interesting(LlamaEngine& engine, const FunctionInfo& fn,
                                                              const std::string& seed_hex,
                                                              const std::string& prev_hex = {},
                                                              int strategy = 1);

std::vector<std::vector<uint8_t>> fuzz4all_combine(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex, const std::string& prev_hex);

void pad_seed_nbytes(std::vector<uint8_t>& b, int n) {
    if (n < 0) n = 0;
    if (static_cast<int>(b.size()) > n) b.resize(static_cast<std::size_t>(n));
    while (static_cast<int>(b.size()) < n) b.push_back(0);
}

std::string bytes_to_hex(const std::vector<uint8_t>& b) {
    std::string hex;
    for (auto byte : b) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02x", byte);
        hex += buf;
    }
    return hex;
}

bool env_flag_is_one(const char* key) {
    const char* v = std::getenv(key);
    return v != nullptr && std::string_view(v) == "1";
}

void env_setdefault(const char* key, const char* val) {
    const char* cur = std::getenv(key);
    if (cur && *cur) return;
#ifdef _WIN32
    SetEnvironmentVariableA(key, val);
#else
    ::setenv(key, val, 0);
#endif
}

std::optional<fs::path> afl_fuzz_which() {
    return Config{}.which({"afl-fuzz", "afl-fuzz.exe"});
}

std::string afl_harness_source(const FunctionInfo& fn, std::string src_rel) {
    for (char& c : src_rel)
        if (c == '\\') c = '/';
    int nbytes = param_nbytes(fn.params);
    std::string dlines, reads, args;
    int off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::string decl = strip(typ);
        if (decl.empty()) decl = "int";
        dlines += "    " + decl + " " + name + ";\n";
        reads += "    memcpy(&" + name + ", buf + " + std::to_string(off) + ", " + std::to_string(sz) + ");\n";
        if (!args.empty()) args += ", ";
        args += name;
        off += sz;
    }
    return "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n#include \"" + src_rel +
           "\"\n\nint main(void) {\n    unsigned char buf[" + std::to_string(nbytes) +
           "];\n    if (fread(buf, 1, " + std::to_string(nbytes) +
           ", stdin) != " + std::to_string(nbytes) + ") return 0;\n" + dlines + reads + "    (void)" +
           fn.name + "(" + args + ");\n    return 0;\n}\n";
}

std::pair<bool, std::string> compile_afl_harness(const fs::path& harness, const fs::path& exe) {
    // Python engine prism/afl.py compile_afl_harness sanitizer fallback:
    //   1. -fsanitize=address,undefined  -fno-sanitize-recover=address,undefined
    //   2. -fsanitize=undefined          -fno-sanitize-recover=undefined
    //   3. -fsanitize=address            -fno-sanitize-recover=address
    //   4. bare (no sanitizer)
    // Prefer ASan+UBSan together; fall back to one sanitizer, then bare.
    // TSan cannot combine with ASan. Missing sanitizer runtime is not a fake
    // CLEAN. Missing gcc/clang is mapped by the caller to NOTRUN, never ERROR.
    auto cc = Config{}.which({"gcc", "clang"});
    if (!cc) return {false, "no C compiler on PATH"};
    std::vector<std::string> cmd{cc->string(), "-O0", "-g", "-std=c11", harness.string(), "-o",
                                 exe.string()};
    const char* san_tries[][2] = {
        {"-fsanitize=address,undefined", "-fno-sanitize-recover=address,undefined"},
        {"-fsanitize=undefined", "-fno-sanitize-recover=undefined"},
        {"-fsanitize=address", "-fno-sanitize-recover=address"},
    };
    ProcRun p;
    bool compiled = false;
    for (auto& flags : san_tries) {
        std::vector<std::string> san = cmd;
        san.insert(san.begin() + 1, flags[0]);
        san.insert(san.begin() + 2, flags[1]);
        p = run_argv(san, {}, 30.0);
        if (p.timeout) return {false, "compile timeout"};
        if (p.rc == 0) {
            compiled = true;
            break;
        }
    }
    if (!compiled) {
        p = run_argv(cmd, {}, 30.0);
        if (p.timeout) return {false, "compile timeout"};
        if (p.rc != 0) {
            std::string err = p.err.empty() ? p.out : p.err;
            if (err.size() > 200) err.resize(200);
            return {false, err};
        }
    }
    return {true, {}};
}

// Bounded AFL++ campaign on a SCALAR stdin harness. CLEAN is not a proof.
std::optional<Finding> run_afl_fuzz(const FunctionInfo& fn, const fs::path& src, double timeout = 2.0) {
    auto afl = afl_fuzz_which();
    if (!afl || fn.kind != "SCALAR") return std::nullopt;
    auto afl_base = [&](std::string_view st, std::string cls, std::string msg) {
        auto f = make_find("fuse", st, fn, std::move(cls), std::move(msg), laws::STRENGTH_FINDS);
        if (st != laws::NOTRUN) f.extra["engine"] = "afl";
        return f;
    };
    if (!sandbox::allowed()) {
        // Law 9: the harness runs the scanned function.
        auto f = sandbox::exec_notrun("fuse", "AFL++ harness", {{"exec", std::string(laws::NOTRUN)}});
        f.file = fn.file;
        f.function = fn.name;
        f.line = fn.line;
        return f;
    }
    auto cc = Config{}.which({"gcc", "clang"});
    if (!cc) {
        auto f = afl_base(laws::NOTRUN, "", "AFL: no C compiler on PATH");
        f.extra["install"] = "install gcc or clang";
        return f;
    }
    auto td = fs::temp_directory_path() / ("prism_afl_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    struct Guard {
        fs::path p;
        ~Guard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{td};
    auto src_copy = td / src.filename();
    if (!fs::exists(src_copy)) {
        std::ofstream out(src_copy);
        out << read_text_file(src);
    }
    auto hpath = td / ("harness_" + fn.name + ".c");
    {
        std::ofstream out(hpath);
        out << afl_harness_source(fn, src.filename().string());
    }
    auto exe = td / ("harness_" + fn.name + ".exe");
    auto [ok, err] = compile_afl_harness(hpath, exe);
    if (!ok) {
        auto low = lower_copy(err);
        if (low.find("no c compiler") != std::string::npos ||
            low.find("gcc/clang") != std::string::npos ||
            (low.find("not on path") != std::string::npos &&
             (low.find("gcc") != std::string::npos || low.find("clang") != std::string::npos))) {
            auto f = afl_base(laws::NOTRUN, "", "AFL: no C compiler on PATH");
            f.extra["install"] = "install gcc or clang";
            return f;
        }
        return afl_base(laws::ERROR, "", "AFL: compile failed: " + err);
    }
    auto in_dir = td / "in";
    auto out_dir = td / "out";
    fs::create_directories(in_dir);
    fs::create_directories(out_dir);
    int nbytes = param_nbytes(fn.params);
    {
        std::ofstream seed(in_dir / "seed", std::ios::binary);
        std::vector<char> zeros(static_cast<std::size_t>(std::max(1, nbytes)), 0);
        seed.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    env_setdefault("AFL_NO_UI", "1");
    env_setdefault("AFL_SKIP_CPUFREQ", "1");
    env_setdefault("AFL_NO_AFFINITY", "1");
    int vsec = std::max(1, static_cast<int>(timeout));
    run_argv(sandbox::wrap_argv({afl->string(), "-i", in_dir.string(), "-o", out_dir.string(), "-V",
                                 std::to_string(vsec), "--", exe.string()},
                                td),
             {}, timeout + 10.0, sandbox::limits_for(timeout + 10.0, /*limit_as=*/false));
    std::vector<fs::path> crash_dirs{out_dir / "crashes", out_dir / "default" / "crashes"};
    for (auto& cdir : crash_dirs) {
        std::error_code ec;
        if (!fs::is_directory(cdir, ec)) continue;
        std::vector<fs::path> crashes;
        for (auto& ent : fs::directory_iterator(cdir, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            if (ent.path().filename() == "README.txt") continue;
            crashes.push_back(ent.path());
        }
        if (crashes.empty()) continue;
        std::string data = read_text_file(crashes[0]);
        std::vector<uint8_t> raw(data.begin(), data.end());
        if (raw.size() > 16) raw.resize(16);
        auto f = afl_base(laws::CRASH, "AFL-CRASH", "AFL crash on " + bytes_to_hex(raw));
        f.counterexample = bytes_to_hex(std::vector<uint8_t>(data.begin(), data.end()));
        f.extra["afl_crashes"] = std::to_string(crashes.size());
        return f;
    }
    return afl_base(laws::CLEAN, "", "no AFL crash in " + std::to_string(vsec) + "s (not a proof)");
}

const char* kLibfuzzerInstall = "clang -fsanitize=fuzzer is a system tool: apt install clang-18 (see third_party/MANIFEST.toml)";

std::string libfuzzer_harness_source(const FunctionInfo& fn, std::string src_rel) {
    for (char& c : src_rel)
        if (c == '\\') c = '/';
    int nbytes = param_nbytes(fn.params);
    std::string dlines, reads, args;
    int off = 0;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        std::string key;
        for (char c : typ)
            if (c != '*') key.push_back(c);
        key = strip(key);
        int sz = kCTypeSize.contains(key) ? kCTypeSize.at(key) : 4;
        std::string decl = strip(typ);
        if (decl.empty()) decl = "int";
        dlines += "    " + decl + " " + name + ";\n";
        reads += "    memcpy(&" + name + ", Data + " + std::to_string(off) + ", " + std::to_string(sz) + ");\n";
        if (!args.empty()) args += ", ";
        args += name;
        off += sz;
    }
    return "#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n#include \"" + src_rel +
           "\"\n\nint LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {\n    if (Size < " +
           std::to_string(nbytes) + ") return 0;\n" + dlines + reads + "    (void)" + fn.name + "(" + args +
           ");\n    return 0;\n}\n";
}

bool libfuzzer_flag_rejected(const std::string& text) {
    auto low = lower_copy(text);
    return low.find("unsupported") != std::string::npos || low.find("unknown") != std::string::npos ||
           low.find("unrecognized") != std::string::npos;
}

std::pair<std::string, std::string> compile_libfuzzer(const fs::path& clang, const fs::path& src,
                                                      const fs::path& exe) {
    const char* san_tries[][2] = {
        {"-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=address,undefined"},
        {"-fsanitize=fuzzer,undefined", "-fno-sanitize-recover=undefined"},
        {"-fsanitize=fuzzer,address", "-fno-sanitize-recover=address"},
        {"-fsanitize=fuzzer", nullptr},
    };
    std::string last_text;
    bool rejected = false;
    for (auto& flags : san_tries) {
        std::vector<std::string> cmd{clang.string(), flags[0]};
        if (flags[1]) cmd.push_back(flags[1]);
        cmd.insert(cmd.end(), {"-O0", "-g", "-std=c11", src.string(), "-o", exe.string()});
        auto p = run_argv(cmd, {}, 30.0);
        last_text = p.err.empty() ? p.out : p.err + p.out;
        if (p.timeout) return {"timeout", last_text};
        if (p.rc == 0) return {"ok", {}};
        if (libfuzzer_flag_rejected(last_text)) rejected = true;
    }
    if (rejected) return {"notrun", last_text};
    return {"error", last_text};
}

Finding run_libfuzzer(const FunctionInfo& fn, const fs::path& src, double timeout = 2.0) {
    auto lf_base = [&](std::string_view st, std::string cls, std::string msg) {
        auto f = make_find("libfuzzer", st, fn, std::move(cls), std::move(msg), laws::STRENGTH_FINDS);
        if (st != laws::NOTRUN && st != laws::NEEDS_HARNESS) f.extra["engine"] = "libfuzzer";
        return f;
    };
    if (fn.kind == "POINTER")
        return lf_base(laws::NEEDS_HARNESS, "",
                       "POINTER: libFuzzer harness would invent a buffer or pass NULL");
    if (fn.kind == "OTHER")
        return lf_base(laws::NEEDS_HARNESS, "", "OTHER signature, not harnessed");
    if (auto syn = unencoded_syntax_reason(fn, "libFuzzer"))
        return lf_base(laws::NEEDS_HARNESS, "", *syn);
    if (body_needs_pointer_harness(fn.body))
        return lf_base(laws::NEEDS_HARNESS, "",
                       "local pointer or heap object: libFuzzer harness would invent a buffer");
    if (!sandbox::allowed()) {
        // Law 9: the libFuzzer binary runs the scanned function.
        auto f = sandbox::exec_notrun("libfuzzer", "libFuzzer harness",
                                      {{"exec", std::string(laws::NOTRUN)}});
        f.file = fn.file;
        f.function = fn.name;
        f.line = fn.line;
        return f;
    }
    auto clang = Config{}.which({"clang", "clang.exe"});
    if (!clang) {
        auto f = lf_base(laws::NOTRUN, "", "clang not on PATH");
        f.extra["install"] = kLibfuzzerInstall;
        return f;
    }
    auto td = fs::temp_directory_path() / ("prism_libfuzzer_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    struct Guard {
        fs::path p;
        ~Guard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{td};
    auto src_copy = td / src.filename();
    if (!fs::exists(src_copy)) {
        std::ofstream out(src_copy);
        out << read_text_file(src);
    }
    auto hpath = td / ("lfuzzer_" + fn.name + ".c");
    {
        std::ofstream out(hpath);
        out << libfuzzer_harness_source(fn, src.filename().string());
    }
    auto exe = td / ("lfuzzer_" + fn.name + ".exe");
    auto [st, err] = compile_libfuzzer(*clang, hpath, exe);
    if (st == "timeout") {
        auto f = lf_base(laws::TIMEOUT, "", "libFuzzer compile timeout");
        f.extra["install"] = kLibfuzzerInstall;
        f.extra["exe"] = clang->string();
        return f;
    }
    if (st == "notrun") {
        auto f = lf_base(laws::NOTRUN, "", "clang has no libFuzzer (-fsanitize=fuzzer)");
        f.extra["install"] = kLibfuzzerInstall;
        f.extra["exe"] = clang->string();
        return f;
    }
    if (st != "ok") {
        std::string msg = err;
        if (msg.size() > 200) msg.resize(200);
        auto f = lf_base(laws::ERROR, "", "libFuzzer compile failed: " + msg);
        f.extra["exe"] = clang->string();
        return f;
    }
    auto corpus = td / "corpus";
    fs::create_directories(corpus);
    int nbytes = param_nbytes(fn.params);
    {
        std::ofstream seed(corpus / "seed", std::ios::binary);
        std::string zeros(static_cast<std::size_t>(std::max(0, nbytes)), '\0');
        seed.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    int vsec = std::max(1, static_cast<int>(timeout));
    // crash-* artifacts land in the scratch dir (the jail's only writable dir).
    auto r = run_argv(sandbox::wrap_argv({exe.string(), corpus.string(),
                                          "-max_total_time=" + std::to_string(vsec), "-timeout=1",
                                          "-artifact_prefix=" + (td / "").string()},
                                         td),
                      {}, timeout + 10.0, sandbox::limits_for(timeout + 10.0, /*limit_as=*/false));
    std::vector<fs::path> crashes;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(td, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        auto name = it->path().filename().string();
        if (name.rfind("crash-", 0) == 0) crashes.push_back(it->path());
    }
    if (!crashes.empty()) {
        std::string data = read_text_file(crashes[0]);
        std::vector<uint8_t> raw(data.begin(), data.end());
        if (raw.size() > 16) raw.resize(16);
        auto f = lf_base(laws::CRASH, "LIBFUZZER-CRASH", "libFuzzer crash on " + bytes_to_hex(raw));
        f.counterexample = bytes_to_hex(std::vector<uint8_t>(data.begin(), data.end()));
        f.extra["exe"] = clang->string();
        f.extra["libfuzzer_crashes"] = std::to_string(crashes.size());
        return f;
    }
    std::string text = lower_copy(r.err + r.out);
    if (text.find("addresssanitizer") != std::string::npos ||
        text.find("undefinedbehaviorsanitizer") != std::string::npos) {
        auto f = lf_base(laws::CRASH, "LIBFUZZER-CRASH", "libFuzzer sanitizer crash");
        f.extra["exe"] = clang->string();
        return f;
    }
    auto f = lf_base(laws::CLEAN, "", "no libFuzzer crash in " + std::to_string(vsec) + "s (not a proof)");
    f.extra["exe"] = clang->string();
    return f;
}

Finding fuse_one(const FunctionInfo& fn, const std::vector<Finding>& bmc_findings, const fs::path& root,
                 double budget, int iters, int rounds, LlamaEngine* engine) {
    auto nh = [&](std::string msg) {
        return make_find("fuse", laws::NEEDS_HARNESS, fn, "", std::move(msg), laws::STRENGTH_FINDS);
    };
    if (fn.kind == "POINTER")
        return nh("POINTER: FuSeBMC harness would invent a buffer or pass NULL");
    if (fn.kind == "OTHER") return nh("OTHER signature, not harnessed");
    if (float_unencoded(fn)) return nh("float/double unencoded: FuSeBMC concrete oracle is not an IEEE model");
    if (auto syn = unencoded_syntax_reason(fn, "FuSeBMC")) return nh(*syn);
    if (body_needs_pointer_harness(fn.body))
        return nh("local pointer or heap object: FuSeBMC harness would invent a buffer");
    bool afl_on_path = afl_fuzz_which().has_value();
    bool opted_afl = env_flag_is_one("PRISM_AFL");
    bool use_afl = opted_afl && afl_on_path;
    bool opted_libfuzzer = env_flag_is_one("PRISM_LIBFUZZER");
    fs::path src = fn.file;
    if (!src.is_absolute()) src = root / fn.file;
    if (!fs::exists(src) && fs::is_regular_file(root)) src = root;
    if (!fs::exists(src) && fs::exists(root / fs::path(fn.file).filename()))
        src = root / fs::path(fn.file).filename();
    auto seeds = seeds_from_bmc(fn, bmc_findings);
    int from_bmc = static_cast<int>(seeds.size());
    auto labeled = numbered_goals(fn);
    std::set<std::string> covered;
    Finding last;
    std::map<std::string, std::string> extra{
        {"from_bmc", std::to_string(from_bmc)},
        {"rounds", "0"},
        {"new_bmc_seeds", "0"},
        {"noseed", from_bmc == 0 ? "true" : "false"},
    };
    {
        nlohmann::json ids = nlohmann::json::array();
        nlohmann::json gmap = nlohmann::json::object();
        for (auto& [lab, cond] : labeled) {
            ids.push_back(lab);
            gmap[lab] = cond;
        }
        extra["goals"] = ids.dump();
        extra["goal_ids"] = ids.dump();
        extra["goal_map"] = gmap.dump();
        extra["new_goals"] = nlohmann::json::array().dump();
    }
    extra["llm"] = engine ? "true" : "false";
    bool chatfuzz_done = false;
    bool mutate_done = false;
    bool combine_done = false;
    std::vector<uint8_t> prev_interesting;
    bool afl_tried = false;
    bool libfuzzer_tried = false;
    if (opted_afl && !afl_on_path) {
        extra["afl"] = "NOTRUN";
        extra["install"] = adapter_install("afl-fuzz");
    } else if (afl_on_path && !use_afl) {
        extra["afl_available"] = "true";
    }
    bool llm_up = llama_engine_up(engine);
    if (engine && !llm_up) {
        extra["autoprompt"] = "NOTRUN";
        extra["chatfuzz"] = "NOTRUN";
        extra["install"] = "ollama serve  (qwen3.5:9b) or build prism with -DPRISM_LLAMA=ON";
    }
    if (llm_up) {
        try {
            int n = param_nbytes(fn.params);
            int added = 0;
            for (auto b : fuzz4all_seeds(*engine, fn, n)) {
                pad_seed_nbytes(b, n);
                if (!b.empty() && std::find(seeds.begin(), seeds.end(), b) == seeds.end()) {
                    seeds.push_back(std::move(b));
                    ++added;
                }
            }
            extra["fuzz4all"] = std::to_string(added);
            extra["autoprompt"] = "ok";
        } catch (const std::exception& ex) {
            extra["fuzz4all_error"] = std::string(ex.what()).substr(0, 200);
        }
    }
    for (int r = 0; r < rounds; ++r) {
        extra["rounds"] = std::to_string(r + 1);
        try {
            last = fuzz_function(fn, src, budget, iters, seeds.empty() ? nullptr : &seeds);
        } catch (const std::exception& ex) {
            auto f = make_find("fuse", laws::ERROR, fn, "", ex.what(), laws::STRENGTH_FINDS);
            f.extra = extra;
            return f;
        }
        last.stage = "fuse";
        if (last.extra.contains("iters")) extra["fuzz_iters"] = last.extra["iters"];
        if (last.extra.contains("corpus")) extra["corpus"] = last.extra["corpus"];
        if (last.extra.contains("new_cov")) extra["new_cov"] = last.extra["new_cov"];
        if (last.extra.contains("sandbox")) extra["sandbox"] = last.extra["sandbox"];
        if (auto ex = last.extra.find("exec"); ex != last.extra.end() && ex->second == laws::NOTRUN) {
            // Law 9: the compiled-harness half was held back (no --allow-exec).
            extra["binary"] = std::string(laws::NOTRUN);
            extra["exec"] = std::string(laws::NOTRUN);
        }
        if (last.status == laws::CRASH || last.status == laws::ERROR || last.status == laws::NEEDS_HARNESS) {
            for (auto& [k, v] : extra) last.extra[k] = v;
            return last;
        }
        if (use_afl && fn.kind == "SCALAR" && !afl_tried) {
            afl_tried = true;
            auto afl_last = run_afl_fuzz(fn, src, 2.0);
            if (afl_last) {
                if (afl_last->status == laws::NOTRUN) {
                    extra["afl"] = "NOTRUN";
                    extra.erase("engine");
                    if (afl_last->extra.contains("install")) extra["install"] = afl_last->extra["install"];
                    if (afl_last->extra.contains("exec")) extra["exec"] = afl_last->extra["exec"];
                } else if (afl_last->status == laws::CRASH || afl_last->status == laws::ERROR) {
                    extra["engine"] = "afl";
                    for (auto& [k, v] : extra) afl_last->extra[k] = v;
                    return *afl_last;
                } else {
                    extra["engine"] = "afl";
                }
            }
        }
        if (opted_libfuzzer && fn.kind == "SCALAR" && !libfuzzer_tried) {
            libfuzzer_tried = true;
            auto lf_last = run_libfuzzer(fn, src, 2.0);
            if (lf_last.status == laws::NOTRUN) {
                extra["libfuzzer"] = "NOTRUN";
                auto eng = extra.find("engine");
                if (eng != extra.end() && eng->second == "libfuzzer") extra.erase(eng);
                if (lf_last.extra.contains("install")) extra["install"] = lf_last.extra["install"];
                else extra["install"] = kLibfuzzerInstall;
                if (lf_last.extra.contains("exec")) extra["exec"] = lf_last.extra["exec"];
            } else if (lf_last.status == laws::NEEDS_HARNESS) {
                for (auto& [k, v] : extra) lf_last.extra[k] = v;
                return lf_last;
            } else if (lf_last.status == laws::CRASH || lf_last.status == laws::ERROR) {
                extra["engine"] = "libfuzzer";
                for (auto& [k, v] : extra) lf_last.extra[k] = v;
                return lf_last;
            } else {
                extra["engine"] = "libfuzzer";
                extra["libfuzzer"] = "ok";
            }
        }
        int new_cov = 0;
        if (last.extra.contains("new_cov")) {
            try {
                new_cov = std::stoi(last.extra["new_cov"]);
            } catch (...) {
            }
        }
        nlohmann::json newly = nlohmann::json::array();
        for (auto& s : seeds) {
            auto args = decode_args(fn, s);
            for (auto& [lab, cond] : labeled) {
                if (covered.contains(lab)) continue;
                auto v = eval_cond(fn, args, cond);
                if (v && *v) {
                    covered.insert(lab);
                    newly.push_back(lab);
                }
            }
        }
        if (!newly.empty()) {
            nlohmann::json ng = nlohmann::json::array();
            if (extra.contains("new_goals")) {
                try {
                    ng = nlohmann::json::parse(extra["new_goals"]);
                } catch (...) {
                }
            }
            for (auto& x : newly) ng.push_back(x);
            extra["new_goals"] = ng.dump();
        }
        if (llm_up && !chatfuzz_done && new_cov == 0) {
            chatfuzz_done = true;
            try {
                int n = param_nbytes(fn.params);
                std::string seed_hex = seeds.empty() ? std::string(static_cast<std::size_t>(std::max(0, n)) * 2, '0')
                                                     : bytes_to_hex(seeds[0]);
                extra["chatfuzz"] = "true";
                for (auto m : chatfuzz_mutants(*engine, fn, seed_hex)) {
                    pad_seed_nbytes(m, n);
                    if (!m.empty() && std::find(seeds.begin(), seeds.end(), m) == seeds.end())
                        seeds.push_back(std::move(m));
                }
            } catch (const std::exception& ex) {
                extra["chatfuzz_error"] = std::string(ex.what()).substr(0, 200);
            }
        }
        if (llm_up && new_cov > 0) {
            try {
                int n = param_nbytes(fn.params);
                std::vector<uint8_t> parent = seeds.empty() ? std::vector<uint8_t>(static_cast<std::size_t>(std::max(0, n)), 0)
                                                           : seeds.back();
                pad_seed_nbytes(parent, n);
                std::string parent_hex = bytes_to_hex(parent);
                if (!mutate_done) {
                    mutate_done = true;
                    extra["fuzz4all_mutate"] = "true";
                    for (auto m : fuzz4all_mutate_interesting(*engine, fn, parent_hex)) {
                        pad_seed_nbytes(m, n);
                        if (!m.empty() && std::find(seeds.begin(), seeds.end(), m) == seeds.end())
                            seeds.push_back(std::move(m));
                    }
                }
                std::string prev_hex;
                if (!prev_interesting.empty())
                    prev_hex = bytes_to_hex(prev_interesting);
                else if (seeds.size() >= 2)
                    prev_hex = bytes_to_hex(seeds[seeds.size() - 2]);
                if (!prev_hex.empty() && !combine_done) {
                    combine_done = true;
                    extra["fuzz4all_combine"] = "true";
                    for (auto m : fuzz4all_combine(*engine, fn, parent_hex, prev_hex)) {
                        pad_seed_nbytes(m, n);
                        if (!m.empty() && std::find(seeds.begin(), seeds.end(), m) == seeds.end())
                            seeds.push_back(std::move(m));
                    }
                }
                prev_interesting = parent;
            } catch (const std::exception& ex) {
                extra["fuzz4all_mutate_error"] = std::string(ex.what()).substr(0, 200);
            }
        }
#ifdef PRISM_HAS_Z3
        for (auto& [lab, cond] : labeled) {
            if (covered.contains(lab)) continue;
            auto cloned = fn;
            cloned.body = "if (!(" + cond + ")) return 0;\n" + fn.body;
            auto g = bmc_one(cloned, 8);
            extra["rounds"] = std::to_string(r + 1);
            if (g.status == laws::FAILED && !g.counterexample.empty()) {
                covered.insert(lab);
                nlohmann::json ids = nlohmann::json::array();
                if (extra.contains("bmc_goals")) {
                    try {
                        ids = nlohmann::json::parse(extra["bmc_goals"]);
                    } catch (...) {
                    }
                }
                ids.push_back(lab);
                extra["bmc_goals"] = ids.dump();
                int n = param_nbytes(fn.params);
                auto b = bytes_from_cex(g.counterexample, n);
                if (b && std::find(seeds.begin(), seeds.end(), *b) == seeds.end()) {
                    seeds.push_back(*b);
                    extra["new_bmc_seeds"] = std::to_string(std::stoi(extra["new_bmc_seeds"]) + 1);
                }
            } else if (g.status == laws::PROVED || g.status == laws::PROVED_UNBOUNDED ||
                       g.status == laws::BOUNDED) {
                covered.insert(lab);
            }
        }
#endif
    }
    extra["covered_goals"] = nlohmann::json(std::vector<std::string>(covered.begin(), covered.end())).dump();
    extra["seeds"] = std::to_string(seeds.size());
    auto eng = extra.find("engine");
    auto afl_flag = extra.find("afl");
    if (afl_flag != extra.end() && afl_flag->second == "NOTRUN" && eng != extra.end() &&
        eng->second == "afl")
        extra.erase("engine");
    auto lf_flag = extra.find("libfuzzer");
    eng = extra.find("engine");
    if (lf_flag != extra.end() && lf_flag->second == "NOTRUN" && eng != extra.end() &&
        eng->second == "libfuzzer")
        extra.erase("engine");
    if (use_afl && afl_tried) {
        auto it = extra.find("afl");
        if (it == extra.end() || it->second != "NOTRUN") extra["engine"] = "afl";
    }
    auto f = make_find("fuse", laws::CLEAN, fn, "",
                       "no crash in FuSeBMC loop (" + extra["rounds"] + " rounds; not a proof)",
                       laws::STRENGTH_FINDS);
    f.extra = extra;
    return f;
}

const char* SYSTEM_FUZZ4ALL =
    "You distill a fuzzing prompt. Given C source, reply JSON: "
    "{\"prompt\":\"...\", \"seeds\":[\"hexbytes\", \"...\"]}. Seeds are little-endian "
    "argument encodings as lowercase hex. No prose.";

const char* AP_SYSTEM_MESSAGE = "You are an auto-prompting tool";

const char* AP_INSTRUCTION =
    "Please summarize the above documentation in a concise manner to describe the usage and "
    "functionality of the target ";

const char* SYSTEM_FUZZ4ALL_MUTATE =
    "The previous generation was interesting. Mutate it into a new valid encoding. "
    "Given the function and a hex seed, reply JSON: "
    "{\"mutants\":[\"hex\", \"...\"]}. No prose.";

const char* SYSTEM_FUZZ4ALL_COMBINE =
    "Combine the two previous interesting encodings into one valid encoding. "
    "Reply JSON: "
    "{\"mutants\":[\"hex\", \"...\"]}. No prose.";

const char* SYSTEM_CHATFUZZ =
    "Coverage has stalled. Given the function and a hex seed, reply JSON: "
    "{\"mutants\":[\"hex\", \"...\"]} semantically valid argument encodings. No prose.";

const char* LLM_SKIP_FUSE_MSG = "llama.cpp/Ollama not reachable; stall mutants / autoprompt skipped";

bool llama_engine_up(LlamaEngine* e) { return e && e->available(); }

std::optional<std::vector<uint8_t>> parse_hex_bytes(std::string h) {
    h = strip(h);
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i < h.size(); i += 2) {
        int a = nibble(h[i]), b = nibble(h[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((a << 4) | b));
    }
    return out;
}

int score_prompt_seeds(const std::vector<std::vector<uint8_t>>& seeds, int nbytes) {
    int n = nbytes > 0 ? nbytes : 1;
    std::set<std::vector<uint8_t>> uniq;
    for (auto s : seeds) {
        if (s.empty()) continue;
        if (static_cast<int>(s.size()) > n) s.resize(static_cast<std::size_t>(n));
        while (static_cast<int>(s.size()) < n) s.push_back(0);
        uniq.insert(std::move(s));
    }
    return static_cast<int>(uniq.size());
}

std::vector<std::vector<uint8_t>> json_hex_list(const nlohmann::json& data, const char* key) {
    std::vector<std::vector<uint8_t>> out;
    if (!data.is_object() || !data.contains(key) || !data[key].is_array()) return out;
    for (auto& h : data[key]) {
        auto raw = parse_hex_bytes(h.is_string() ? h.get<std::string>() : std::string{});
        if (raw && !raw->empty()) out.push_back(std::move(*raw));
    }
    return out;
}

std::string documentation_from_comments(const std::string& source) {
    std::vector<std::string> parts;
    auto is_contract = [](const std::string& s) {
        auto l = lower_copy(s);
        return l.find("requires:") != std::string::npos || l.find("ensures:") != std::string::npos ||
               l.find("invariant:") != std::string::npos || l.find("decreases:") != std::string::npos;
    };
    for (std::size_t i = 0; i < source.size();) {
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*') {
            if (i + 2 < source.size() && source[i + 2] == '@') {
                auto end = source.find("*/", i + 2);
                i = end == std::string::npos ? source.size() : end + 2;
                continue;
            }
            auto end = source.find("*/", i + 2);
            if (end == std::string::npos) break;
            auto inner = source.substr(i + 2, end - (i + 2));
            i = end + 2;
            std::string blob;
            std::istringstream ss(inner);
            std::string ln;
            while (std::getline(ss, ln)) {
                ln = strip(ln);
                while (!ln.empty() && ln.front() == '*') ln = strip(ln.substr(1));
                if (!blob.empty() && !ln.empty()) blob.push_back(' ');
                blob += ln;
            }
            blob = strip(blob);
            if (!blob.empty() && !is_contract(blob)) parts.push_back(blob);
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/') {
            auto eol = source.find('\n', i);
            auto line = strip(source.substr(i + 2, (eol == std::string::npos ? source.size() : eol) - (i + 2)));
            i = eol == std::string::npos ? source.size() : eol + 1;
            if (!line.empty() && !is_contract(line)) parts.push_back(line);
            continue;
        }
        ++i;
    }
    std::string out;
    for (std::size_t k = 0; k < parts.size() && k < 8; ++k) {
        if (!out.empty()) out.push_back('\n');
        out += parts[k];
    }
    return out;
}

std::string fuzz4all_autoprompt_text(LlamaEngine& engine, const FunctionInfo& fn) {
    if (!engine.available()) return {};
    auto src_file = read_fn_source(fn);
    auto docs = documentation_from_comments(src_file);
    auto src = docs.empty() ? (fn.signature + "\n{" + fn.body + "\n}") : docs;
    if (src.size() > 6000) src.resize(6000);
    auto r = engine.complete({{"system", AP_SYSTEM_MESSAGE}, {"user", src + "\n" + AP_INSTRUCTION}}, 90.0);
    if (!r.error.empty()) return {};
    auto t = strip(r.text);
    if (t.size() > 2000) t.resize(2000);
    return t;
}

std::string fuzz4all_update_strategy(const std::string& new_hex, const std::string& prev_hex, int strategy) {
    if (strategy == 0) return "seed=" + new_hex + "\ngenerate a new encoding";
    if (strategy == 1) return "seed=" + new_hex + "\nmutate the previous generation";
    if (strategy == 2) return "seed=" + new_hex + "\nsemantically equivalent encoding";
    if (!prev_hex.empty())
        return "prev=" + prev_hex + "\nseed=" + new_hex + "\ncombine the two previous encodings";
    return "seed=" + new_hex + "\nmutate the previous generation";
}

std::vector<std::vector<uint8_t>> fuzz4all_seeds(LlamaEngine& engine, const FunctionInfo& fn, int nbytes) {
    if (!engine.available()) return {};
    auto user = "nbytes=" + std::to_string(nbytes) + "\n" + fn.signature + "\n{" + fn.body + "\n}";
    auto r = engine.complete({{"system", SYSTEM_FUZZ4ALL}, {"user", user}}, 90.0);
    auto data = r.error.empty() ? extract_json(r.text) : nlohmann::json(nullptr);
    auto seeds_a = json_hex_list(data, "seeds");
    int best_sc = score_prompt_seeds(seeds_a, nbytes);
    auto best = seeds_a;
    auto distilled = fuzz4all_autoprompt_text(engine, fn);
    if (!distilled.empty()) {
        auto r2 = engine.complete(
            {{"system", std::string(SYSTEM_FUZZ4ALL) + "\n" + distilled}, {"user", user}}, 90.0);
        auto data2 = r2.error.empty() ? extract_json(r2.text) : nlohmann::json(nullptr);
        auto seeds_b = json_hex_list(data2, "seeds");
        if (score_prompt_seeds(seeds_b, nbytes) > best_sc) best = std::move(seeds_b);
    }
    return best;
}

std::vector<std::vector<uint8_t>> fuzz4all_mutate_interesting(LlamaEngine& engine, const FunctionInfo& fn,
                                                              const std::string& seed_hex,
                                                              const std::string& prev_hex, int strategy) {
    if (!engine.available()) return {};
    auto user = fuzz4all_update_strategy(seed_hex, prev_hex, strategy);
    user += "\n" + fn.signature + "\n{" + fn.body + "\n}";
    const char* sys = (strategy == 3 && !prev_hex.empty()) ? SYSTEM_FUZZ4ALL_COMBINE : SYSTEM_FUZZ4ALL_MUTATE;
    auto r = engine.complete({{"system", sys}, {"user", user}}, 90.0);
    if (!r.error.empty()) return {};
    return json_hex_list(extract_json(r.text), "mutants");
}

std::vector<std::vector<uint8_t>> fuzz4all_combine(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex, const std::string& prev_hex) {
    return fuzz4all_mutate_interesting(engine, fn, seed_hex, prev_hex, 3);
}

std::vector<std::vector<uint8_t>> chatfuzz_mutants(LlamaEngine& engine, const FunctionInfo& fn,
                                                   const std::string& seed_hex) {
    if (!engine.available()) return {};
    auto r = engine.complete({{"system", SYSTEM_CHATFUZZ},
                              {"user", "seed=" + seed_hex + "\n" + fn.signature + "\n{" + fn.body + "\n}"}},
                             90.0);
    if (!r.error.empty()) return {};
    auto data = extract_json(r.text);
    std::vector<std::vector<uint8_t>> out;
    if (!data.is_object() || !data.contains("mutants") || !data["mutants"].is_array()) return out;
    for (auto& h : data["mutants"]) {
        auto raw = parse_hex_bytes(h.is_string() ? h.get<std::string>() : std::string{});
        if (raw) out.push_back(std::move(*raw));
    }
    return out;
}

}  // namespace

std::vector<Finding> run_fuse(const std::vector<FunctionInfo>& functions, const std::vector<Finding>& bmc_findings,
                              const fs::path& src_root, double budget, int iters, bool llm) {
    std::vector<Finding> out;
    int rounds = 2;
    double slice_budget = std::max(0.05, budget / rounds);
    int slice_iters = std::max(1, iters / rounds);
    std::optional<LlamaEngine> eng;
    if (llm) eng.emplace(Config{});
    LlamaEngine* ep = eng ? &*eng : nullptr;
    for (auto& fn : functions)
        out.push_back(fuse_one(fn, bmc_findings, src_root, slice_budget, slice_iters, rounds, ep));
    if (ep && !ep->available() && !functions.empty()) {
        Finding f;
        f.stage = "fuse";
        f.status = std::string(laws::NOTRUN);
        f.file = functions[0].file;
        f.function = functions[0].name;
        f.line = functions[0].line;
        f.message = LLM_SKIP_FUSE_MSG;
        f.strength = std::string(laws::STRENGTH_READS);
        f.extra["install"] = LLM_INSTALL;
        f.extra["autoprompt"] = "NOTRUN";
        f.extra["chatfuzz"] = "NOTRUN";
        f.extra["docstring"] = documentation_from_comments(read_fn_source(functions[0]));
        out.push_back(std::move(f));
    } else if (ep && ep->available()) {
        for (auto& fn : functions) {
            auto docs = documentation_from_comments(read_fn_source(fn));
            std::string distilled = docs;
            try {
                auto t = fuzz4all_autoprompt_text(*ep, fn);
                if (!t.empty()) distilled = t;
            } catch (...) {
            }
            Finding f;
            f.stage = "fuse";
            f.status = std::string(laws::HYPOTHESIS);
            f.file = fn.file;
            f.function = fn.name;
            f.line = fn.line;
            f.message = "Fuzz4All distilled prompt (hypothesis, not a proof)";
            f.strength = std::string(laws::STRENGTH_READS);
            if (distilled.size() > 800) distilled.resize(800);
            f.extra["autoprompt"] = distilled;
            auto ds = docs;
            if (ds.size() > 400) ds.resize(400);
            f.extra["docstring"] = ds;
            f.extra["target_api"] = fn.name;
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace prism
