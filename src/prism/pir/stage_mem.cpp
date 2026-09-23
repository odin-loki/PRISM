// Memory-model policy of the pir stage (docs/PIR.md "Memory model",
// "Pointer parameters", "Global state").

#include "stage_mem.hpp"

#include "prism/ai.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"

#include <algorithm>
#include <regex>

namespace prism::pir::pirmem {

std::vector<PtrContract> contracts_from_draft(const std::vector<std::string>& assumptions, const ir::Function& irf) {
    std::vector<PtrContract> out;
    static const std::regex size(R"(^([A-Za-z_]\w*) points to (at least |exactly )?([A-Za-z_]\w*|\d+) int element)");
    static const std::regex range(R"(^(-?\d+) <= ([A-Za-z_]\w*) <= (\d+))");
    auto param = [&](const std::string& n, bool ptr) {
        for (auto& p : irf.params)
            if (p.name == n) return ptr ? p.ty.kind == ir::Type::Ptr : p.ty.kind == ir::Type::Int;
        return false;
    };
    std::map<std::string, std::pair<int64_t, int64_t>> ranges;
    for (auto& a : assumptions) {
        std::smatch m;
        if (std::regex_search(a, m, range)) ranges[m[2].str()] = {std::stoll(m[1].str()), std::stoll(m[3].str())};
    }
    for (auto& a : assumptions) {
        std::smatch m;
        if (!std::regex_search(a, m, size) || !param(m[1].str(), true)) continue;
        // Only sizes with evidence independent of the accesses being checked:
        // a length parameter, or an index bounded by an early return. A size
        // taken from the largest literal index ("at least max+1") would make
        // those accesses in bounds by construction: not used.
        if (a.find("at least") != std::string::npos && a.find("by the early return") == std::string::npos) continue;
        PtrContract c;
        c.param = m[1].str();
        c.elem_bytes = 4;
        c.source = "harness (template draft)";
        c.text = a;
        auto n = m[3].str();
        if (std::isdigit(static_cast<unsigned char>(n[0]))) {
            c.count = std::stoll(n);
        } else if (param(n, false)) {
            c.count_param = n;
            if (auto it = ranges.find(n); it != ranges.end()) {
                c.count_min = it->second.first;
                c.count_max = it->second.second;
            }
        } else {
            continue;
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool has_dynamic_init(const ir::Module& m) {
    for (auto& g : m.globals)
        if (g.name == "llvm.global_ctors" || g.name == "@llvm.global_ctors") return true;
    for (auto& f : m.functions)
        if (f.name.starts_with("_GLOBAL__sub_I") || f.name.starts_with("__cxx_global_var_init")) return true;
    return false;
}

std::vector<std::string> static_init_functions(const ir::Module& m) {
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    };
    // @llvm.global_ctors = appending global [N x { i32, ptr, ptr }] [{ i32 65535, ptr @f, ptr null }, ...]
    static const std::regex ref(R"(\{\s*i32\s+\d+\s*,\s*ptr\s+@([\w.$"]+))");
    for (auto& g : m.globals) {
        if (g.name != "llvm.global_ctors" && g.name != "@llvm.global_ctors") continue;
        for (std::sregex_iterator it(g.text.begin(), g.text.end(), ref), end; it != end; ++it) {
            auto n = (*it)[1].str();
            if (n.size() > 1 && n.front() == '"') n = n.substr(1, n.size() - 2);
            add(n);
        }
    }
    for (auto& f : m.functions)
        if (f.name.starts_with("_GLOBAL__sub_I")) add(f.name);
    // no list and no _GLOBAL__sub_I: the per-variable initialisers
    if (out.empty())
        for (auto& f : m.functions)
            if (f.name.starts_with("__cxx_global_var_init")) add(f.name);
    return out;
}

TranslateOptions function_options(const TranslateOptions& base, const ir::Function& irf, UnitInfo& unit, int line,
                                  const Config& cfg, const std::string& source_name) {
    TranslateOptions o = base;
    o.strict_aliasing = cfg.strict_aliasing;
    o.globals_initial = source_name == "main";
    // C++ library code (constructors, accessors) nests deeper than C helpers
    if (unit.cxx) o.inline_depth = std::max(o.inline_depth, 12);
    o.contracts.clear();
    std::vector<std::string> ptrs;
    for (auto& p : irf.params)
        if (p.ty.kind == ir::Type::Ptr && p.attrs.find("byval(") == std::string::npos &&
            p.attrs.find("sret(") == std::string::npos)
            ptrs.push_back(p.name);
    if (ptrs.empty() || line <= 0) return o;
    o.contracts = parse_contracts(unit.src_lines, line, irf);
    auto covered = [&] {
        for (auto& p : ptrs)
            if (std::none_of(o.contracts.begin(), o.contracts.end(), [&](auto& c) { return c.param == p; }))
                return false;
        return true;
    };
    if (covered() || unit.cxx || !cfg.pir_drafts) return o;
    // no (complete) precondition: a deterministic template harness draft may
    // give the sizes (ai::draft_harness; never an LLM here, Law 4)
    if (!unit.functions) {
        try {
            unit.functions = extract_functions(unit.path, unit.rel);
        } catch (...) {
            unit.functions = std::vector<FunctionInfo>{};
        }
    }
    for (auto& fi : *unit.functions) {
        if (fi.name != source_name) continue;
        std::string why;
        auto drafts = ai::draft_harness(fi, &why);
        if (drafts.empty() || drafts.front().source != "template") break;
        auto dc = contracts_from_draft(drafts.front().assumptions, irf);
        for (auto& c : dc)
            if (std::none_of(o.contracts.begin(), o.contracts.end(), [&](auto& x) { return x.param == c.param; }))
                o.contracts.push_back(c);
        break;
    }
    return o;
}

void apply_memory_policy(Finding& f, Verdict& v, Function& fn, const ir::Module& mod, const ir::Function& irf,
                         const TranslateOptions& topt, const Config& cfg) {
    if (fn.uses_memory) {
        f.extra["strict_aliasing"] = cfg.strict_aliasing ? "checked" : "off (--strict-aliasing checks effective types)";
    }
    if (!fn.throws.empty()) {
        std::string s;
        for (auto& t : fn.throws) s += (s.empty() ? "" : ",") + t;
        f.extra["throws_not_followed"] = s;
    }
    // `main` starts from the globals' initializers only when the unit has no
    // dynamic initialisation: C++ constructors of globals (`B g;` with a
    // user constructor, `int x = f();`) and __attribute__((constructor))
    // run before main and are not modelled, so the IR initializer (often
    // zeroinitializer) is not the state main sees. Only a verdict that holds
    // for arbitrary global values stands; anything else (a proof that relied
    // on the initializers, a violation that the constructors may prevent)
    // is NEEDS-HARNESS. Found by the ESBMC C++ conformance tasks
    // (docs/CONFORMANCE.md "Known issues", S8).
    // The same code must itself be free of violations for main's proof to be
    // the program's: a constructor of a global that throws, or fails, ends
    // the program before main runs.
    if (topt.globals_initial && has_dynamic_init(mod) &&
        (v.status == laws::PROVED || v.status == laws::PROVED_UNBOUNDED || v.status == laws::BOUNDED)) {
        auto o = topt;
        o.globals_initial = true;  // static initialisation starts from the static initializers
        for (auto& name : static_init_functions(mod)) {
            auto* ctor = mod.find(name);
            if (!ctor) continue;
            auto tr = translate(mod, *ctor, o);
            std::string st = tr.fn ? check_function(*tr.fn, cfg.unwind, cfg.timeout).status
                                   : (tr.status.empty() ? std::string(laws::NEEDS_HARNESS) : tr.status);
            if (st == laws::PROVED || st == laws::PROVED_UNBOUNDED) continue;
            f.extra["verdict_before_static_init"] = v.status;
            v.extra["static_init"] = name + ": " + st + (tr.fn ? "" : " (" + tr.reason + ")");
            v.status = std::string(laws::NEEDS_HARNESS);
            v.message = "the unit's dynamic initialisation before main (" + v.extra["static_init"] +
                        ") is not proved, so main's verdict is not the program's";
            v.cex.clear();
            v.cex_args.clear();
            return;
        }
        v.extra["static_init"] = "proved";
    }
    if (topt.globals_initial && fn.mutable_globals && has_dynamic_init(mod) && v.status != laws::NEEDS_HARNESS &&
        v.status != laws::ERROR) {
        auto o = topt;
        o.globals_initial = false;
        auto tr = translate(mod, irf, o);
        std::optional<Verdict> v2;
        if (tr.fn) v2 = check_function(*tr.fn, cfg.unwind, cfg.timeout);
        if (v2 && (v2->status == laws::PROVED || v2->status == laws::PROVED_UNBOUNDED)) {
            v2->extra["globals"] = "arbitrary (dynamic initialisation not modelled): " + v2->status;
            v = std::move(*v2);
        } else {
            f.extra["verdict_before_globals"] = v.status;
            v.extra["globals"] = "initial values: " + v.status + "; arbitrary: " + (v2 ? v2->status : "UNENCODED");
            v.status = std::string(laws::NEEDS_HARNESS);
            v.message = "main runs after the unit's dynamic initialisation (constructors of globals), which is not "
                        "modelled; the verdict with the globals' static initializers (" + v.extra["globals"] +
                        ") is not the program's";
            v.cex.clear();
            v.cex_args.clear();
            return;
        }
    }
    // Global state: a violation that needs other values of mutable globals
    // than their initializers is a missing precondition, not a defect.
    if (v.status == laws::FAILED && fn.mutable_globals && !topt.globals_initial) {
        auto o = topt;
        o.globals_initial = true;
        auto tr = translate(mod, irf, o);
        if (tr.fn) {
            auto v2 = check_function(*tr.fn, cfg.unwind, cfg.timeout);
            if (v2.status != laws::FAILED) {
                f.extra["verdict_before_globals"] = v.status;
                v.status = std::string(laws::NEEDS_HARNESS);
                v.message = "violation needs values of mutable globals other than their initializers (" + v.message +
                            "); the global state the function is called in is an unstated precondition";
                v.extra["globals"] = "arbitrary: FAILED; initial values: " + v2.status;
                v.cex.clear();
                v.cex_args.clear();
                return;
            }
            v.extra["globals"] = "FAILED also with the globals' initial values";
        }
    }
    // A violation under a drafted (not user-stated) precondition is not a
    // defect report: the precondition was invented (Law 6).
    bool drafted = false;
    for (auto& a : fn.assumptions) drafted = drafted || a.find("harness (template draft)") != std::string::npos;
    if (drafted && v.status == laws::FAILED) {
        v.extra["verdict_under_draft"] = v.status + ": " + v.message;
        v.status = std::string(laws::NEEDS_HARNESS);
        v.message = "pointer parameter: violation only under a drafted harness (" + v.message +
                    "); state the precondition with // requires: (Law 6)";
        v.cex.clear();
        v.cex_args.clear();
    }
    if (!fn.assumptions.empty()) {
        std::string s;
        for (auto& a : fn.assumptions) s += (s.empty() ? "" : "; ") + a;
        v.extra["assumptions"] = s;
        if (v.status == laws::PROVED || v.status == laws::PROVED_UNBOUNDED) {
            v.extra["verdict_before_assumptions"] = v.status;
            v.status = std::string(laws::PROVED_ASSUMING);
            v.message += " -- assuming: " + s;
        }
    }
}

std::string tv_exclusion(const Function& fn) {
    if (!fn.ptr_params.empty()) return "pointer parameters bound to contract objects are not replayed";
    if (fn.returns_ptr) return "returns a pointer (object ids differ from addresses)";
    if (fn.mutable_globals) return "reads or writes mutable globals (state persists across replayed calls)";
    return {};
}

}  // namespace prism::pir::pirmem
