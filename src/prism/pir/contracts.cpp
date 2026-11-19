// Pointer-parameter preconditions for PIR (Law 6, docs/PIR.md "Pointer
// parameters"). A pointer parameter is encoded only when a precondition
// states the size of the object it points to:
//
//   // requires: \valid(p + (0..n-1))        n elements (n: an int parameter)
//   // requires: \valid(p + (0..7))          8 elements
//   // requires: \valid(p)                   one element
//   // requires: \valid_read(p + (0..n-1))   read-only (writes are MEM-WRITE-CONST)
//   /*@ requires \valid(p + (0..n-1)); */   ACSL block
//
// in the comment block right before the function or in the leading comment
// lines of its body. The element size is the size of the first access
// through p in the IR. The verdict of such a function is PROVED-ASSUMING
// with every assumption listed (translate.cpp bind_contract).

#include "prism/pir.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace prism::pir {

namespace {

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r");
    auto b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool is_comment_or_blank(const std::string& l) {
    auto t = trim(l);
    return t.empty() || t.starts_with("//") || t.starts_with("/*") || t.starts_with("*") || t.ends_with("*/");
}

}  // namespace

std::vector<PtrContract> parse_contracts(const std::vector<std::string>& lines, int fn_line, const ir::Function& f,
                                         std::vector<std::string>* unparsed) {
    std::vector<PtrContract> out;
    if (fn_line <= 0 || fn_line > static_cast<int>(lines.size())) return out;
    // candidate lines: the comment block above the signature, the signature
    // line(s) up to '{', and the leading comment lines of the body
    std::vector<std::string> cand;
    int i = fn_line - 2;
    // A multi-line ACSL block `/*@ requires A;\n    ensures B;\n*/` has
    // continuation lines that start with neither `*` nor `//`: from its
    // closing line up to the line that opens it, every line is comment.
    bool in_block = false;
    while (i >= 0) {
        const auto& l = lines.at(static_cast<std::size_t>(i));
        if (in_block) {
            cand.push_back(l);
            if (l.find("/*") != std::string::npos) in_block = false;
            --i;
            continue;
        }
        if (!is_comment_or_blank(l)) break;
        cand.push_back(l);
        auto t = trim(l);
        if (t.ends_with("*/") && t.find("/*") == std::string::npos) in_block = true;
        --i;
    }
    std::size_t j = static_cast<std::size_t>(fn_line - 1);
    while (j < lines.size() && lines.at(j).find('{') == std::string::npos) {
        cand.push_back(lines.at(j));
        ++j;
    }
    if (j < lines.size()) {
        auto l = lines[j];
        auto rest = l.substr(l.find('{') + 1);
        if (rest.find('}') == std::string::npos) {  // not a one-line body
            cand.push_back(rest);
            ++j;
            // leading comment lines of the body (a blank line or code ends them)
            while (j < lines.size() && !trim(lines.at(j)).empty() && is_comment_or_blank(lines.at(j)) &&
                   lines.at(j).find('}') == std::string::npos) {
                cand.push_back(lines.at(j));
                ++j;
            }
        }
    }
    std::string text;
    // The contracts stage checks the function with its requires assumed (an
    // early return, stages/contracts.cpp): conditions are not unbound there.
    bool conditions_assumed = false;
    for (auto& l : cand) {
        if (l.find("prism: requires assumed") != std::string::npos) conditions_assumed = true;
        if (l.find("requires") == std::string::npos) continue;
        text += l + "\n";
    }
    if (text.empty()) return out;
    static const std::regex valid(
        R"(\\valid(_read)?\s*\(\s*([A-Za-z_]\w*)\s*(?:\+\s*\(\s*0\s*\.\.\s*([^()]+?)\s*\))?\s*\))");
    static const std::regex upper(
        R"re(^\s*(?:([A-Za-z_]\w*)\s*(?:([+-])\s*(\d+))?|(\d+))\s*$)re");
    auto is_param = [&](const std::string& n, bool ptr) {
        for (auto& p : f.params)
            if (p.name == n) return ptr ? p.ty.kind == ir::Type::Ptr : p.ty.kind == ir::Type::Int;
        return false;
    };
    for (auto it = std::sregex_iterator(text.begin(), text.end(), valid); it != std::sregex_iterator(); ++it) {
        const auto& mt = *it;
        PtrContract c;
        c.read_only = mt[1].matched;
        c.param = mt[2].str();
        c.text = mt[0].str();
        c.source = "requires";
        if (!is_param(c.param, true)) {
            if (unparsed) unparsed->push_back(c.text + " (not a pointer parameter)");
            continue;
        }
        if (!mt[3].matched) {
            c.count = 1;
        } else {
            std::smatch um;
            auto ub = mt[3].str();
            if (!std::regex_match(ub, um, upper)) {
                if (unparsed) unparsed->push_back(c.text + " (range bound not recognised)");
                continue;
            }
            if (um[4].matched) {
                c.count = std::stoll(um[4].str()) + 1;
            } else {
                c.count_param = um[1].str();
                if (!is_param(c.count_param, false)) {
                    if (unparsed) unparsed->push_back(c.text + " (bound is not an integer parameter)");
                    continue;
                }
                int64_t k = um[3].matched ? std::stoll(um[3].str()) : 0;
                if (um[2].matched && um[2].str() == "-") k = -k;
                c.count_add = k + 1;  // 0..E has E+1 elements
            }
        }
        bool dup = false;
        for (auto& o : out)
            if (o.param == c.param) dup = true;
        if (!dup) out.push_back(std::move(c));
    }
    // A requires atom the binding above does not state -- a \valid of
    // something other than a pointer parameter (\valid(*pp),
    // \valid(head->next)) or any other condition (r->count <= CAP, n < 8;
    // pir binds objects, it does not assume conditions) -- is recorded with
    // an empty param (source "unbound"): a violation under such a
    // precondition is not reported as FAILED (stage_mem.cpp
    // apply_memory_policy; the contracts stage checks the function with its
    // requires assumed).
    std::istringstream ls(text);
    for (std::string l; std::getline(ls, l);) {
        for (std::size_t at = l.find("requires"); at != std::string::npos; at = l.find("requires", at + 8)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(l[at - 1])) || l[at - 1] == '_')) continue;
            auto b = at + 8;
            while (b < l.size() && (l[b] == ':' || l[b] == ' ' || l[b] == '\t')) ++b;
            auto e = std::min({l.find(';', b), l.find("*/", b), l.size()});
            auto clause = l.substr(b, e - b);
            // atoms: the clause split at top-level &&
            std::vector<std::string> atoms;
            std::string cur;
            int depth = 0;
            for (std::size_t i = 0; i < clause.size(); ++i) {
                char ch = clause[i];
                if (ch == '(') ++depth;
                if (ch == ')') --depth;
                if (depth == 0 && ch == '&' && i + 1 < clause.size() && clause[i + 1] == '&') {
                    atoms.push_back(cur);
                    cur.clear();
                    ++i;
                    continue;
                }
                cur.push_back(ch);
            }
            atoms.push_back(cur);
            for (auto& a0 : atoms) {
                auto a = trim(a0);
                if (a.empty() || a.starts_with("\\separated")) continue;  // separation is pir's own assumption
                // a \valid anywhere but as a whole atom (p == \null || \valid(p))
                // is never bound
                bool bound = conditions_assumed && a.find("\\valid") == std::string::npos;
                if (a.starts_with("\\valid")) {
                    auto open = a.find('(');
                    auto inner = open == std::string::npos ? std::string() : trim(a.substr(open + 1));
                    if (!inner.empty() && inner.back() == ')') inner = trim(inner.substr(0, inner.size() - 1));
                    std::size_t k = 0;
                    while (k < inner.size() && (std::isalnum(static_cast<unsigned char>(inner[k])) || inner[k] == '_'))
                        ++k;
                    auto rest = trim(inner.substr(k));
                    bound = a.ends_with(")") && k > 0 && is_param(inner.substr(0, k), true) &&
                            (rest.empty() || rest.front() == '+');
                }
                if (!bound) {
                    PtrContract u;
                    u.source = "unbound";
                    u.text = a;
                    out.push_back(std::move(u));
                }
            }
        }
    }
    return out;
}

}  // namespace prism::pir

namespace prism::pir {

namespace {

std::string_view trim_sv(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::vector<LockContract> parse_lock_contracts(std::string_view tsv, std::string* error) {
    std::vector<LockContract> out;
    int n = 0;
    auto fail = [&](const std::string& why) {
        if (error) *error = "line " + std::to_string(n) + ": " + why;
        return std::vector<LockContract>{};
    };
    while (!tsv.empty()) {
        auto nl = tsv.find('\n');
        std::string_view line = tsv.substr(0, nl);
        tsv = nl == std::string_view::npos ? std::string_view() : tsv.substr(nl + 1);
        ++n;
        auto t = trim_sv(line);
        if (t.empty() || t.front() == '#') continue;
        std::vector<std::string_view> f;
        std::string_view rest = line;
        for (;;) {
            auto tab = rest.find('\t');
            f.push_back(trim_sv(rest.substr(0, tab)));
            if (tab == std::string_view::npos) break;
            rest = rest.substr(tab + 1);
        }
        if (f[0] == "function") continue;
        if (f.size() < 3 || f.size() > 4) return fail("expected 3 or 4 tab-separated columns");
        if (f[0].empty()) return fail("empty function name");
        LockContract c;
        c.function = std::string(f[0]);
        c.lock = std::string(f[1]);
        c.action = std::string(f[2]);
        if (f[1].starts_with("global:")) {
            if (f[1].size() == 7) return fail("empty global name");
        } else if (f[1].starts_with("arg:")) {
            auto d = f[1].substr(4);
            if (d.empty() || d.size() > 9) return fail("bad argument index");
            for (char ch : d)
                if (ch < '0' || ch > '9') return fail("bad argument index");
        } else if (f[1].starts_with("field:")) {
            // field:TYPE.INDEX -> the lock-order class "TYPE.#INDEX" (lock_order.cpp field_class); field names
            // are not parsed (no debug info in the IR), so the index is required
            auto body = f[1].substr(6);
            auto dot = body.rfind('.');
            if (dot == std::string_view::npos || dot == 0 || dot + 1 == body.size())
                return fail("field must be field:TYPE.INDEX");
            for (char ch : body.substr(dot + 1))
                if (ch < '0' || ch > '9') return fail("field index must be a number (field names are not parsed)");
            c.lock = "field:" + std::string(body.substr(0, dot)) + ".#" + std::string(body.substr(dot + 1));
        } else {
            return fail("lock must be global:NAME, arg:N or field:TYPE.INDEX");
        }
        if (f[2] != "acquires" && f[2] != "releases" && f[2] != "requires-held")
            return fail("action must be acquires, releases or requires-held");
        if (f.size() == 4) {
            if (f[3] == "spin") c.spin = true;
            else if (f[3] != "mutex") return fail("kind must be mutex or spin");
        }
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace prism::pir

namespace prism::pir {

std::vector<LockContract> read_lock_contracts(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("--lock-contracts: cannot read " + path);
    std::stringstream text;
    text << in.rdbuf();
    std::string err;
    auto out = parse_lock_contracts(text.str(), &err);
    if (!err.empty()) throw std::runtime_error("--lock-contracts " + path + ": " + err);
    return out;
}

}  // namespace prism::pir
