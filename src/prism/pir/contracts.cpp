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
    while (i >= 0 && is_comment_or_blank(lines.at(static_cast<std::size_t>(i)))) {
        cand.push_back(lines.at(static_cast<std::size_t>(i)));
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
    for (auto& l : cand) {
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
        } else {
            return fail("lock must be global:NAME or arg:N");
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
