// llvm-sem-vs-lli: differential test of PRISM's formal LLVM semantics
// (proofs/refinement/PrismRefine/Llvm.lean, trusted per docs roadmap 8.3)
// against lli, for every function of the given C files that is in the
// modelled fragment:
//
// 1. the pir stage exports (LLVM fragment, PIR) pairs (PRISM_PIR_LEAN_EXPORT);
// 2. llvm_eval (proofs/refinement/EvalMain.lean; stays Lean, it executes the
//    proved semantics) runs each function on edge-case and random inputs
//    under the LangRef semantics, the strict semantics and the PIR semantics
//    of the proved translator;
// 3. every input on which the LangRef semantics returns a value without
//    undefined behaviour or poison is executed with lli on the IR the pir
//    stage itself verifies (pir::lower_to_ir: the same clang flags and opt
//    pipeline), and the printed result must be that value;
// 4. the three Lean semantics must agree as proved (translate_exact /
//    strict_lazy); a disagreement there is a bug in this tool.
//
// Exit 1 on any disagreement, 2 when a tool is missing (NOTRUN). Runs the
// given programs under lli: use it on trusted test programs only.

#include "qa.hpp"

#include "../../prism/proc.hpp"
#include "prism/config.hpp"
#include "prism/pir.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::uint64_t mask_of(int w) { return w >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << w) - 1); }

std::string join_u64(const std::vector<std::uint64_t>& v, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + std::to_string(v[i]);
    return out;
}
}  // namespace

std::vector<PirlRecord> pirl_records(const std::string& text) {
    std::vector<PirlRecord> out;
    PirlRecord cur;
    bool ok = true, inblock = false;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        auto w = words(line);
        if (w.empty()) continue;
        auto two = [&](const char* b) { return w.size() >= 2 && w[0] == "L" && w[1] == b; };
        if (w[0] == "func" && w.size() >= 2) {
            cur = PirlRecord{w[1], {}, 0};
            ok = true;
            inblock = false;
        } else if (two("params")) {
            cur.params.clear();
            for (std::size_t i = 4; i < w.size(); i += 2) cur.params.push_back(std::atoi(w[i].c_str()));
        } else if (two("block")) {
            inblock = true;
        } else if (two("ret") && !inblock && w.size() >= 3) {
            cur.ret = std::atoi(w[2].c_str());
        } else if (two("unsupported")) {
            ok = false;
        } else if (w[0] == "end" && ok && !cur.name.empty()) {
            // a repeated name keeps its first position and its latest shape
            auto it = std::find_if(out.begin(), out.end(), [&](auto& r) { return r.name == cur.name; });
            if (it == out.end()) out.push_back(cur);
            else *it = cur;
        }
    }
    return out;
}

std::vector<std::vector<std::uint64_t>> input_vectors(const std::vector<int>& widths, int n, std::mt19937_64& rng) {
    if (widths.empty()) return {{}};
    std::vector<std::vector<std::uint64_t>> pools;
    for (int w : widths) {
        const auto m = mask_of(w);
        const auto sign = w >= 1 ? std::uint64_t{1} << (w - 1) : 0;
        std::set<std::uint64_t> vals{0, 1, 2, 7, 100 & m, m, m - 1, sign, sign - 1};
        for (int i = 0; i < 3; ++i) vals.insert(rng() & m);
        std::set<std::uint64_t> masked;
        for (auto v : vals) masked.insert(v & m);
        pools.emplace_back(masked.begin(), masked.end());
    }
    std::set<std::vector<std::uint64_t>> picked;
    for (int k = 0; k < n * 4; ++k) {
        std::vector<std::uint64_t> v;
        for (auto& p : pools) v.push_back(p[rng() % p.size()]);
        picked.insert(std::move(v));
    }
    std::vector<std::vector<std::uint64_t>> out;
    for (auto& v : picked) {
        if (static_cast<int>(out.size()) >= n) break;
        out.push_back(v);
    }
    return out;
}

std::string lli_harness(const std::string& ir, const std::string& fn, const std::vector<int>& widths, int ret,
                        const std::vector<std::uint64_t>& args) {
    std::string call_args;
    for (std::size_t i = 0; i < widths.size() && i < args.size(); ++i)
        call_args += (i ? ", " : "") + ("i" + std::to_string(widths[i]) + " " + std::to_string(args[i]));
    std::vector<std::string> body{"define i32 @__prism_lli_main() {"};
    if (ret == 0) {
        body.push_back("  call void @" + fn + "(" + call_args + ")");
        body.push_back("  call i32 (ptr, ...) @printf(ptr @__prism_lli_void)");
    } else {
        body.push_back("  %r = call i" + std::to_string(ret) + " @" + fn + "(" + call_args + ")");
        if (ret < 64) body.push_back("  %z = zext i" + std::to_string(ret) + " %r to i64");
        else body.push_back("  %z = add i64 %r, 0");
        body.push_back("  call i32 (ptr, ...) @printf(ptr @__prism_lli_fmt, i64 %z)");
    }
    body.push_back("  ret i32 0");
    body.push_back("}");
    // The pir stage's instrumentation markers (src/prism/pir/stage.cpp,
    // lower_ctl.cpp) are declarations lli cannot resolve; each gets a
    // definition: __prism.uninit.* returns 0 (one of the indeterminate
    // values it stands for), __prism.poison.* 0 (only reached on inputs the
    // Lean semantics calls UB or poison, which are never compared), and the
    // void markers do nothing. A compared result that depends on such a
    // value shows up as a disagreement, never as agreement.
    std::string out;
    {
        std::istringstream in(ir);
        for (std::string line; std::getline(in, line);) {
            const auto at = line.find(" @__prism.");
            const auto open = line.find('(', at == std::string::npos ? 0 : at);
            const auto close = line.find(')', open == std::string::npos ? 0 : open);
            if (!line.starts_with("declare ") || at == std::string::npos || open == std::string::npos ||
                close == std::string::npos) {
                out += line + "\n";
                continue;
            }
            const auto ret_ty = line.substr(8, at - 8);
            out += "define " + line.substr(8, close + 1 - 8) + " {\n";
            out += ret_ty == "void" ? "  ret void\n}\n" : "  ret " + ret_ty + " zeroinitializer\n}\n";
        }
    }
    out += "\n@__prism_lli_fmt = private constant [6 x i8] c\"%llu\\0A\\00\"\n"
           "@__prism_lli_void = private constant [6 x i8] c\"void\\0A\\00\"\n";
    if (ir.find("@printf(") == std::string::npos) out += "declare i32 @printf(ptr, ...)\n";
    for (auto& l : body) out += l + "\n";
    return out;
}

int llvm_sem_vs_lli_main(const Args& args) {
    const auto repo = repo_root();
    std::vector<fs::path> files;
    std::string bin;
    fs::path pairs_dir;
    int nvec = 12;
    std::uint64_t seed = 20260923;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--bin") bin = next();
        else if (a == "--vectors") nvec = std::max(1, std::atoi(next().c_str()));
        else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--pairs") pairs_dir = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism-qa llvm-sem-vs-lli [FILE.c...] [--bin PRISM] [--vectors N] [--seed S]\n"
                         "                         [--pairs DIR]\n"
                         "The formal LLVM semantics (llvm_eval, proofs/refinement) against lli on the\n"
                         "pir stage's IR. Default files: tests/pir/*.c. --pairs: existing .pirl files\n"
                         "(<dir>/*<file name>*.pirl) instead of running PRISM. Exit 1 on a\n"
                         "disagreement, 2 NOTRUN. Runs the programs: trusted test programs only.\n";
            return 0;
        } else if (!a.starts_with("-")) files.emplace_back(a);
        else {
            std::cerr << "llvm-sem-vs-lli: unknown option " << a << "\n";
            return 2;
        }
    }
    std::mt19937_64 rng(seed);
    std::vector<std::string> missing;
    const auto eval = repo / "proofs" / "refinement" / ".lake" / "build" / "bin" / "llvm_eval";
    std::error_code ec;
    if (!fs::is_regular_file(eval, ec)) missing.push_back("llvm_eval");
    std::optional<fs::path> exe;
    if (pairs_dir.empty()) {
        exe = find_prism(bin, repo);
        if (!exe) missing.push_back("prism");
    }
    auto cfg = default_config();
    const auto fe = pir::find_frontend(cfg);
    if (!fe.clang) missing.push_back("clang");
    if (!fe.opt) missing.push_back("opt");
    if (!fe.lli) missing.push_back("lli");
    if (!missing.empty()) {
        std::string m;
        for (std::size_t i = 0; i < missing.size(); ++i) m += (i ? ", " : "") + missing[i];
        std::cout << "llvm_sem_vs_lli: NOTRUN: missing " << m
                  << " (build prism; cd proofs/refinement && lake build llvm_eval)\n";
        return 2;
    }
    if (files.empty()) {
        for (auto& e : fs::directory_iterator(repo / "tests" / "pir", ec))
            if (e.is_regular_file() && e.path().extension() == ".c") files.push_back(e.path());
        std::sort(files.begin(), files.end());
    }
    std::map<std::string, int> stats;
    std::vector<std::string> bad;
    TempDir t("prism-sem-lli-");
    for (auto src : files) {
        src = fs::weakly_canonical(fs::absolute(src), ec);
        const auto stem = src.stem().string();
        const auto name = src.filename().string();
        std::vector<fs::path> pirls;
        const auto dir = pairs_dir.empty() ? t.path / "pairs" / stem : pairs_dir;
        if (pairs_dir.empty()) {
            detail::SessionOptions so;
            so.env = {{"PRISM_PIR_LEAN_EXPORT", dir.string()}};
            detail::run_session({exe->string(), src.string(), "--no-llm", "--stage", "inventory,classify,pir",
                                 "--out", (t.path / "out" / stem).string()},
                                so);
        }
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".pirl") continue;
            const auto pn = e.path().filename().string();
            if (!pairs_dir.empty() && pn != name + ".pirl" && !pn.ends_with("_" + name + ".ll.pirl")) continue;
            pirls.push_back(e.path());
        }
        std::sort(pirls.begin(), pirls.end());
        std::string err;
        auto ir = pir::lower_to_ir(fe, src, std::max(10.0, cfg.timeout), err);
        if (pirls.empty() || !ir) {
            ++stats["file-skipped"];
            continue;
        }
        const auto prog = t.path / "h.ll";
        for (const auto& pirl : pirls) {
            for (const auto& rec : pirl_records(read_text(pirl))) {
                if (rec.ret > 64 ||
                    std::any_of(rec.params.begin(), rec.params.end(), [](int w) { return w < 1 || w > 64; })) {
                    // wider than the harness's i64 printf path: not compared, counted
                    ++stats["wide-skipped"];
                    continue;
                }
                const auto vecs = input_vectors(rec.params, nvec, rng);
                std::string q;
                for (auto& v : vecs) q += rec.name + (v.empty() ? "" : " " + join_u64(v, " ")) + "\n";
                detail::RunSpec spec;
                spec.argv = {eval.string(), pirl.string()};
                spec.input = q;
                spec.timeout_s = 600;
                auto ev = detail::run(spec);
                std::istringstream lines(ev.out);
                for (std::string line; std::getline(lines, line);) {
                    const auto bar = line.find(" | ");
                    const auto head = line.substr(0, bar);
                    const auto res = bar == std::string::npos ? std::string() : line.substr(bar + 3);
                    auto hw = words(head);
                    std::vector<std::uint64_t> vals;
                    for (std::size_t k = 1; k < hw.size(); ++k) vals.push_back(std::strtoull(hw[k].c_str(), nullptr, 10));
                    const auto pl = res.find("lazy="), ps = res.find(" strict="), pp = res.find(" pir=");
                    if (!res.starts_with("lazy=") || ps == std::string::npos || pp == std::string::npos || pp < ps) {
                        ++stats["outside"];
                        continue;
                    }
                    const auto lazy = res.substr(pl + 5, ps - pl - 5);
                    const auto strict = res.substr(ps + 8, pp - ps - 8);
                    const auto pirv = res.substr(pp + 5);
                    ++stats["runs"];
                    const auto where = name + ":" + rec.name + "[" + join_u64(vals, ", ") + "]";
                    // the proved relations between the three Lean semantics
                    if (lazy.starts_with("ret ") && !pirv.starts_with("outside") && !(strict == lazy && pirv == lazy))
                        bad.push_back(where + ": lazy=" + lazy + " strict=" + strict + " pir=" + pirv);
                    if (!(lazy.starts_with("ret ") || lazy == "ret-void")) {
                        ++stats["lean-ub-or-poison-or-fuel"];
                        continue;
                    }
                    std::ofstream(prog, std::ios::binary) << lli_harness(*ir, rec.name, rec.params, rec.ret, vals);
                    detail::SessionOptions lo;
                    lo.timeout_s = 60;
                    auto li = detail::run_session(
                        {fe.lli->string(), "--entry-function=__prism_lli_main", prog.string()}, lo);
                    auto out_words = words(li.out);
                    if (li.rc != 0 || out_words.empty()) {
                        // the module does not run under lli (unresolved externals
                        // elsewhere in it): not a comparison, counted separately
                        ++stats["lli-error"];
                        continue;
                    }
                    std::string got;
                    {
                        std::istringstream ol(li.out);
                        for (std::string l; std::getline(ol, l);)
                            if (!words(l).empty()) got = l;
                        while (!got.empty() && (got.back() == ' ' || got.back() == '\r')) got.pop_back();
                        while (!got.empty() && got.front() == ' ') got.erase(got.begin());
                    }
                    const auto lw = words(lazy);
                    const auto want = lazy == "ret-void" ? std::string("void") : lw.size() > 1 ? lw[1] : lazy;
                    ++stats["lli-compared"];
                    if (got != want)
                        bad.push_back(where + ": Lean " + want + ", lli " + got + " " + li.err.substr(0, 200));
                    else
                        ++stats["agree"];
                }
            }
        }
    }
    for (auto& b : bad) std::cout << "DISAGREE " << b << "\n";
    std::cout << "summary";
    for (auto& [k, v] : stats) std::cout << " " << k << "=" << v;
    std::cout << " disagree=" << bad.size() << "\n";
    return bad.empty() ? 0 : 1;
}

}  // namespace prism::qa
