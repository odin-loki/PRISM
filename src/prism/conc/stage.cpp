// The conc stage (roadmap 2.6): bounded context-switch checking of the
// threads each harness creates. See include/prism/conc.hpp and
// docs/CONCURRENCY.md.
//
// Units whose source mentions no thread API are skipped without a row (a
// function without threads has nothing to check here). Phase 1 lowers the
// candidate units with clang/opt (processes only); phase 2 extracts and
// solves (Z3 only), the same split the pir stage keeps between process
// spawns and solver threads.

#include "prism/conc.hpp"
#include "prism/laws.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace prism::conc {

namespace {

constexpr const char* kInstall = "install clang and llvm 18 (apt install clang-18 llvm-18)";

bool is_cxx(const fs::path& p) {
    auto e = p.extension().string();
    return e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".C" || e == ".c++";
}

bool is_unit(const fs::path& p) { return p.extension() == ".c" || is_cxx(p); }

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool mentions_threads(const std::string& text) {
    for (auto* k : {"pthread_create", "thrd_create", "std::thread", "std::jthread"})
        if (text.find(k) != std::string::npos) return true;
    return false;
}

std::string rel_of(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        auto r = fs::relative(p, root, ec);
        if (!ec) return r.generic_string();
    }
    return p.filename().string();
}

std::string source_name(const pir::ir::Module& m, const std::string& ir_name) {
    if (const auto* f = m.find(ir_name)) {
        auto it = m.subprograms.find(f->dbg);
        if (it != m.subprograms.end() && !it->second.name.empty()) return it->second.name;
    }
    return ir_name;
}

std::optional<int> def_line(const pir::ir::Module& m, const std::string& ir_name) {
    if (const auto* f = m.find(ir_name)) {
        auto it = m.subprograms.find(f->dbg);
        if (it != m.subprograms.end() && it->second.line > 0) return it->second.line;
    }
    return std::nullopt;
}

Finding base(const std::string& rel) {
    Finding f;
    f.stage = "conc";
    f.file = rel;
    f.strength = std::string(laws::STRENGTH_FINDS);
    return f;
}

Options options_of(const Config& cfg) {
    Options o;
    o.unwind = std::max(1, cfg.unwind);
    o.timeout_s = cfg.timeout;
    return o;
}

struct Harnessed {
    std::string name;       // source name
    std::optional<int> line;
    Result res;
};

std::vector<Harnessed> analyse_ir(const std::string& ir, const Options& opt) {
    std::vector<Harnessed> out;
    auto m = pir::ir::parse_module(ir);
    for (auto& [fn, why] : unsupported_threading(m)) {
        Harnessed h;
        h.name = source_name(m, fn);
        h.line = def_line(m, fn);
        h.res.status = std::string(laws::NEEDS_HARNESS);
        h.res.message = why;
        out.push_back(std::move(h));
    }
    for (auto& hn : harnesses(m)) {
        Harnessed h;
        h.name = source_name(m, hn);
        h.line = def_line(m, hn);
        auto b = build_program(m, ir, hn, opt);
        if (!b.prog) {
            h.res.status = b.status.empty() ? std::string(laws::NEEDS_HARNESS) : b.status;
            h.res.message = b.reason;
        } else {
            h.res = check(*b.prog, opt);
        }
        out.push_back(std::move(h));
    }
    return out;
}

}  // namespace

std::map<std::string, Result> check_file(const fs::path& src, const Config& cfg, const Options& opt) {
    std::map<std::string, Result> out;
    auto fe = pir::find_frontend(cfg);
    std::string err;
    auto ir = pir::lower_to_ir(fe, src, std::max(10.0, cfg.timeout), err);
    if (!ir) {
        Result r;
        r.status = std::string(laws::ERROR);
        r.message = "clang front end failed: " + err;
        out[""] = r;
        return out;
    }
    for (auto& h : analyse_ir(*ir, opt)) out[h.name] = std::move(h.res);
    return out;
}

std::vector<Finding> run_conc(const std::vector<fs::path>& sources, const Config& cfg) {
    std::vector<fs::path> units;
    for (auto& p : sources)
        if (is_unit(p) && mentions_threads(read_text(p))) units.push_back(p);
    if (units.empty()) return {};  // nothing creates threads: no findings
    if (!pir::z3_available()) {
        Finding f = base("");
        f.status = std::string(laws::NOTRUN);
        f.message = "z3 not built: the concurrency stage cannot run";
        f.extra["install"] = "rebuild with -DPRISM_Z3=ON (vendored third_party/z3)";
        return {f};
    }
    auto fe = pir::find_frontend(cfg);
    bool need_cxx = std::any_of(units.begin(), units.end(), is_cxx);
    if (!fe.clang || !fe.opt || (need_cxx && !fe.clangxx)) {
        Finding f = base("");
        f.status = std::string(laws::NOTRUN);
        f.message = std::string(!fe.clang ? "clang" : !fe.opt ? "opt" : "clang++") +
                    " not found: the Clang/LLVM front end of the concurrency stage cannot run";
        f.extra["install"] = kInstall;
        return {f};
    }
    auto opt = options_of(cfg);
    // phase 1: lower (processes only)
    std::vector<std::optional<std::string>> irs(units.size());
    std::vector<std::string> errs(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        try {
            irs[i] = pir::lower_to_ir(fe, units[i], std::max(10.0, cfg.timeout), errs[i]);
        } catch (const std::exception& ex) {
            irs[i].reset();
            errs[i] = std::string("internal error: ") + ex.what();
        }
    }
    // phase 2: extract + solve (Z3 only)
    std::vector<Finding> out;
    for (std::size_t i = 0; i < units.size(); ++i) {
        auto rel = rel_of(units[i], cfg.root);
        if (!irs[i]) {
            auto f = base(rel);
            f.status = std::string(laws::ERROR);
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = "clang front end failed: " + errs[i];
            f.extra["frontend"] = fe.version;
            out.push_back(std::move(f));
            continue;
        }
        std::vector<Harnessed> hs;
        try {
            hs = analyse_ir(*irs[i], opt);
        } catch (const std::exception& ex) {
            auto f = base(rel);
            f.status = std::string(laws::ERROR);
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = std::string("conc internal error: ") + ex.what();
            out.push_back(std::move(f));
            continue;
        }
        for (auto& h : hs) {
            auto f = base(rel);
            f.function = h.name;
            f.line = h.line;
            f.extra["frontend"] = fe.version;
            for (auto& [k, v] : h.res.extra) f.extra[k] = v;
            if (h.res.status == laws::FAILED) {
                for (auto& v : h.res.violations) {
                    auto g = f;
                    g.status = std::string(laws::FAILED);
                    g.cls = v.cls;
                    g.message = v.msg;
                    if (v.line) g.line = v.line;
                    g.counterexample = v.schedule;
                    g.extra["prop"] = v.kind;
                    g.extra["schedule"] = v.schedule;
                    std::string tr;
                    for (auto& e : v.trace) tr += (tr.empty() ? "" : "\n") + e.text;
                    g.evidence = tr;
                    out.push_back(std::move(g));
                }
                continue;
            }
            f.status = h.res.status;
            f.message = h.res.message;
            if (h.res.status != laws::BOUNDED) f.strength = std::string(laws::STRENGTH_SOME);
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace prism::conc
