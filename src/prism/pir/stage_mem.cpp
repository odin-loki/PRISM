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
    if (covered() || unit.cxx) return o;
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
