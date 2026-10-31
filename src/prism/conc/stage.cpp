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
#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace prism::conc {

namespace {

constexpr const char* kInstall = "install the pinned LLVM (python3 scripts/fetch_llvm.py; see third_party/llvm/PIN.toml) or set PRISM_LLVM_BIN";

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

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The LP64 <stdatomic.h> shim used when the host has no C11 atomics drops the
// memory order and lowers the operation to a plain load or store. That plain
// access can come back BOUNDED (or a data race). The order is still in the
// source, and it is not sequentially consistent.
std::optional<std::string> weak_memory_order(const std::string& text) {
    struct Row {
        const char* token;
        const char* shown;
    };
    static constexpr Row rows[] = {
        {"memory_order_relaxed", "memory_order_relaxed"},
        {"__ATOMIC_RELAXED", "memory_order_relaxed"},
        {"memory_order_consume", "memory_order_acquire (or consume)"},
        {"__ATOMIC_CONSUME", "memory_order_acquire (or consume)"},
        {"memory_order_acquire", "memory_order_acquire (or consume)"},
        {"__ATOMIC_ACQUIRE", "memory_order_acquire (or consume)"},
        {"memory_order_release", "memory_order_release"},
        {"__ATOMIC_RELEASE", "memory_order_release"},
        {"memory_order_acq_rel", "memory_order_acq_rel"},
        {"__ATOMIC_ACQ_REL", "memory_order_acq_rel"},
    };
    std::optional<std::string> shown;
    std::size_t at = std::string::npos;
    for (const auto& row : rows) {
        const std::size_t n = std::char_traits<char>::length(row.token);
        std::size_t p = 0;
        while ((p = text.find(row.token, p)) != std::string::npos) {
            const bool left = p > 0 && ident_char(text[p - 1]);
            const auto end = p + n;
            const bool right = end < text.size() && ident_char(text[end]);
            if (!left && !right && (at == std::string::npos || p < at)) {
                at = p;
                shown = row.shown;
                break;
            }
            p = end;
        }
    }
    return shown;
}

std::string weak_order_message(const std::string& name) {
    return "UNENCODED: " + name + " (SC only: relaxed/acquire/release atomics are not encoded yet)";
}

bool only_data_races(const Result& r) {
    if (r.violations.empty()) return false;
    for (const auto& v : r.violations)
        if (v.cls != "CONC-DATA-RACE") return false;
    return true;
}

// A clean result, or a data race found only after the order was erased, is not
// a verdict on a weak atomic. A deadlock or a failed assertion is kept.
void refuse_weak_order(Result& r, const std::string& name) {
    if (r.status == laws::NEEDS_HARNESS && r.message.find("UNENCODED:") != std::string::npos) return;
    const bool clean = r.status.empty() || r.status == laws::CLEAN || laws::is_formal(r.status);
    const bool stripped_race = r.status == laws::FAILED && only_data_races(r);
    if (!clean && !stripped_race) return;
    r.status = std::string(laws::NEEDS_HARNESS);
    r.violations.clear();
    r.message = weak_order_message(name);
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
    const auto weak = weak_memory_order(read_text(src));
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
    if (weak) {
        if (out.empty()) {
            Result r;
            r.status = std::string(laws::NEEDS_HARNESS);
            r.message = weak_order_message(*weak);
            out[""] = std::move(r);
        } else {
            for (auto& [_, r] : out) refuse_weak_order(r, *weak);
        }
    }
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
            irs.at(i) = pir::lower_to_ir(fe, units.at(i), std::max(10.0, cfg.timeout), errs.at(i));
        } catch (const std::exception& ex) {
            irs.at(i).reset();
            errs.at(i) = std::string("internal error: ") + ex.what();
        }
    }
    // phase 2: extract + solve (Z3 only)
    std::vector<Finding> out;
    for (std::size_t i = 0; i < units.size(); ++i) {
        auto rel = rel_of(units.at(i), cfg.root);
        if (!irs.at(i)) {
            auto f = base(rel);
            f.status = std::string(laws::ERROR);
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = "clang front end failed: " + errs.at(i);
            f.extra["frontend"] = fe.version;
            out.push_back(std::move(f));
            continue;
        }
        std::vector<Harnessed> hs;
        try {
            hs = analyse_ir(*irs.at(i), opt);
        } catch (const std::exception& ex) {
            auto f = base(rel);
            f.status = std::string(laws::ERROR);
            f.strength = std::string(laws::STRENGTH_SOME);
            f.message = std::string("conc internal error: ") + ex.what();
            out.push_back(std::move(f));
            continue;
        }
        if (const auto weak = weak_memory_order(read_text(units.at(i)))) {
            if (hs.empty()) {
                auto f = base(rel);
                f.status = std::string(laws::NEEDS_HARNESS);
                f.strength = std::string(laws::STRENGTH_SOME);
                f.message = weak_order_message(*weak);
                f.extra["frontend"] = fe.version;
                out.push_back(std::move(f));
                continue;
            }
            for (auto& h : hs) refuse_weak_order(h.res, *weak);
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
