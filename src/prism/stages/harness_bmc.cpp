// Stage harness: pointer-parameter functions materialized from an honest
// requires and model-checked (run_harness_bmc).
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
// Element type of a one-level pointer/array parameter, if the BMC encoder
// models it (an integer type); nullopt keeps NEEDS-HARNESS. Mirrors
// prism/harness.py _pointee_type.
std::optional<std::string> pointee_type(const std::string& typ) {
    int stars = 0;
    for (char c : typ)
        if (c == '*' || c == '[') ++stars;
    if (stars != 1) return std::nullopt;
    // The base type is what precedes the declarator (`char *dst`, `int a[]`).
    std::string t = typ.substr(0, typ.find_first_of("*["));
    std::istringstream ss(t);
    std::string w, out;
    while (ss >> w) {
        if (w == "const" || w == "volatile" || w == "restrict" || w == "__restrict" || w == "__restrict__")
            continue;
        if (!out.empty()) out += ' ';
        out += w;
    }
    static Regex elem(
        "^(?:(?:unsigned|signed)\\s+(?:long\\s+long|long|short|char|int)(?:\\s+int)?|"
        "long\\s+long(?:\\s+int)?|long(?:\\s+int)?|short(?:\\s+int)?|"
        "unsigned|signed|int|char|_Bool|bool|u?int(?:8|16|32|64)_t|"
        "size_t|ssize_t|ptrdiff_t|u?intptr_t|u?intmax_t)$");
    auto m = elem.search_match(out, 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0 ||
        static_cast<std::size_t>(m->spans[0].second) != out.size())
        return std::nullopt;
    return out;
}

std::vector<std::pair<std::string, std::string>> ptr_params(const FunctionInfo& fn) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [t, n] : fn.params)
        if (!n.empty() && (t.find('*') != std::string::npos || t.find('[') != std::string::npos))
            out.push_back({t, n});
    return out;
}

std::vector<std::pair<std::string, std::string>> scalar_params(const FunctionInfo& fn) {
    auto ptrs = ptr_params(fn);
    std::unordered_set<std::string> pn;
    for (auto& [t, n] : ptrs) pn.insert(n);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [t, n] : fn.params)
        if (!n.empty() && !pn.contains(n)) out.push_back({t, n});
    return out;
}

std::optional<int> honest_size(const std::vector<std::tuple<std::string, std::string, int, std::string>>& atoms,
                               const std::set<std::string>& ptr_names, const std::set<std::string>& scalar_names) {
    std::vector<int> sizes;
    for (auto& [name, op, val, req] : atoms) {
        if (ptr_names.contains(name)) continue;
        if (!scalar_names.contains(name) && !scalar_names.empty()) continue;
        if (op == "<" && val > 0) sizes.push_back(val);
        else if (op == "<=" && val >= 0) sizes.push_back(val + 1);
    }
    if (!sizes.empty()) {
        int k = *std::min_element(sizes.begin(), sizes.end());
        return k > 0 ? std::optional<int>(k) : std::nullopt;
    }
    std::set<std::string> null_ok;
    for (auto& [name, op, val, req] : atoms)
        if (ptr_names.contains(name) && op == "!=" && val == 0) null_ok.insert(name);
    if (!ptr_names.empty()) {
        bool all = true;
        for (auto& n : ptr_names)
            if (!null_ok.contains(n)) all = false;
        if (all) return 1;
    }
    return std::nullopt;
}

std::optional<FunctionInfo> materialize(const FunctionInfo& fn) {
    if (fn.kind != "POINTER") return std::nullopt;
    auto spec = parse_comments(fn);
    std::vector<std::string> reqs;
    for (auto& r : spec.requires_) {
        auto s = strip(r);
        if (!s.empty()) reqs.push_back(s);
    }
    if (reqs.empty()) return std::nullopt;
    auto ptrs = ptr_params(fn);
    if (ptrs.empty()) return std::nullopt;
    auto scalars = scalar_params(fn);
    std::set<std::string> ptr_names, scalar_names;
    for (auto& [t, n] : ptrs) ptr_names.insert(n);
    for (auto& [t, n] : scalars) scalar_names.insert(n);
    static Regex atom("^([A-Za-z_]\\w*)\\s*(==|!=|<=|>=|<|>)\\s*(0|[1-9]\\d*)$");
    std::vector<std::tuple<std::string, std::string, int, std::string>> atoms;
    for (auto& req : reqs) {
        auto m = match_at(atom, req);
        if (m) atoms.emplace_back(m->group(1), m->group(2), std::stoi(m->group(3)), req);
    }
    auto k = honest_size(atoms, ptr_names, scalar_names);
    if (!k) return std::nullopt;
    std::vector<std::string> decls;
    for (auto& [t, name] : ptrs) {
        // The buffer has the pointee's own integer type: an `int` stand-in
        // for a `long *` or `char *` would change every value range.
        auto elem_t = pointee_type(t);
        if (!elem_t) return std::nullopt;
        decls.push_back(*elem_t + " _h_" + name + "[" + std::to_string(*k) + "];");
        decls.push_back(*elem_t + " *" + name + " = _h_" + name + ";");
    }
    std::string guard;
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        if (i) guard += " && ";
        guard += "(" + reqs[i] + ")";
    }
    auto parts = decls;
    if (!guard.empty()) parts.push_back("if (!(" + guard + ")) return 0;");
    parts.push_back(fn.body);
    auto out = fn;
    out.kind = scalars.empty() ? "VOID" : "SCALAR";
    out.params = scalars;
    std::string param_s;
    for (std::size_t i = 0; i < scalars.size(); ++i) {
        if (i) param_s += ", ";
        param_s += strip(scalars[i].first + " " + scalars[i].second);
    }
    if (param_s.empty()) param_s = "void";
    auto ret = fn.return_type.empty() ? "int" : fn.return_type;
    out.signature = ret + " " + fn.name + "(" + param_s + ")";
    out.body = join_sv(parts, "\n");
    return out;
}

}  // namespace

std::vector<Finding> run_harness_bmc(const std::vector<FunctionInfo>& functions, int unwind) {
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (fn.kind != "POINTER") continue;
        auto harnessed = materialize(fn);
        if (!harnessed) {
            // Roadmap 4.2: draft the missing preconditions (template, then the
            // model when one is bound). PROVED-ASSUMING at best, every
            // assumption listed; no draft keeps the NEEDS-HARNESS row below.
            std::string refused;
            if (auto drafted = ai::drafted_harness_bmc(fn, unwind, &refused)) {
                out.push_back(std::move(*drafted));
                continue;
            }
            auto f = make_find(
                "harness", laws::NEEDS_HARNESS, fn, "",
                "POINTER: no honest requires; unguarded BMC would invent "
                "a buffer or dress a NULL crash as a finding",
                laws::STRENGTH_SOME);
            f.extra["harness"] = "false";
            f.extra["assumed"] = "false";
            if (!refused.empty()) f.extra["harness_draft"] = "refused: " + refused;
            out.push_back(std::move(f));
            continue;
        }
        auto spec = parse_comments(fn);
        std::vector<std::string> reqs;
        for (auto& r : spec.requires_) {
            auto s = strip(r);
            if (!s.empty()) reqs.push_back(s);
        }
        auto recs = run_bmc({*harnessed}, unwind, true);
        auto r = recs.empty() ? bmc_one(*harnessed, unwind) : recs[0];
        r.extra["assumed"] = "true";
        r.extra["requires"] = nlohmann::json(reqs).dump();
        r.extra["original_status"] = r.status;
        r.extra["harness"] = "true";
        r.stage = "harness";
        if (r.status == laws::PROVED || r.status == laws::PROVED_UNBOUNDED) {
            r.status = std::string(laws::PROVED_ASSUMING);
            r.message = "encoded UB properties hold assuming (" + join_sv(reqs, " && ") +
                        "); never an unconditional PROVED";
        }
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace prism
