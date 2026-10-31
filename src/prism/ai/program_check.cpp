// ai::check_program on pir. The hook statements of an AI program are lowered
// to C and the result is checked by stages_detail::pir_one (the function's own
// unit, so types, globals and callees are the file's):
//
//   __prism_assume(e);       -> __VERIFIER_assume(e);
//   __prism_havoc(x);        -> __prism_havoc_bytes(&(x), sizeof(x));
//   __prism_step (c) { B }   -> if (c) { B; __VERIFIER_assume(0); }
//                               (B runs under c and its path ends; the code
//                               after continues only where c is false: a cut)
//   __prism_assert(tag, e);  -> assert(e) on line kTagLine + tag
//
// pir reports one violation per run, at its line, so a FAILED `assert` on line
// kTagLine + t refutes tag t (with the counterexample as model); the tag is
// then disabled and the program re-checked until pir proves the rest (unsat),
// bounds them (unsat, unwind_ok = false: a loop was not closed) or stops
// (unknown). Only PROVED, PROVED-UNBOUNDED or BOUNDED make a tag unsat.
// only_invariants checks just the `assert` properties (CheckOptions::
// only_props); otherwise the first UB violation is reported as its own
// property (sat) and the open tags stay unknown.
#include "ai_internal.hpp"
#include "../stages/common.hpp"
#include "prism/laws.hpp"

#include <cctype>
#include <set>
#include <string_view>

namespace prism::ai {

namespace {

constexpr int kTagLine = 900000;

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// End (exclusive) of the balanced group opening at s[i] (one of "({"),
// skipping literals and comments; npos when unbalanced.
std::size_t group_end(const std::string& s, std::size_t i) {
    int depth = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            for (++i; i < s.size() && s[i] != c; ++i)
                if (s[i] == '\\') ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            auto e = s.find("*/", i + 2);
            if (e == std::string::npos) return std::string::npos;
            i = e + 1;
            continue;
        }
        if (c == '(' || c == '{' || c == '[') ++depth;
        if (c == ')' || c == '}' || c == ']')
            if (--depth == 0) return i + 1;
    }
    return std::string::npos;
}

std::size_t skip_ws(const std::string& s, std::size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

struct Lowering {
    const std::set<int>& disabled;
    int base_line;  // line of body[0] in pir_one's unit
    std::string err;
    std::set<int> tags;

    int line_at(const std::string& whole, std::size_t pos) const {
        int n = base_line;
        for (std::size_t i = 0; i < pos && i < whole.size(); ++i) n += whole[i] == '\n';
        return n;
    }

    // Lowers whole[from, to); positions are of `whole` for the line numbers.
    std::string lower(const std::string& whole, std::size_t from, std::size_t to) {
        std::string out;
        std::size_t i = from;
        while (i < to) {
            char c = whole[i];
            if (c == '"' || c == '\'' || (c == '/' && i + 1 < to && (whole[i + 1] == '/' || whole[i + 1] == '*'))) {
                std::size_t j = i;
                if (c == '/' && whole[i + 1] == '/') {
                    while (j < to && whole[j] != '\n') ++j;
                } else if (c == '/') {
                    auto e = whole.find("*/", i + 2);
                    j = e == std::string::npos ? to : e + 2;
                } else {
                    for (++j; j < to && whole[j] != c; ++j)
                        if (whole[j] == '\\') ++j;
                    ++j;
                }
                out.append(whole, i, std::min(j, to) - i);
                i = j;
                continue;
            }
            if (!ident_char(c) || (i > from && ident_char(whole[i - 1]))) {
                out += c;
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < to && ident_char(whole[j])) ++j;
            std::string_view id(whole.data() + i, j - i);
            if (id != "__prism_assume" && id != "__prism_havoc" && id != "__prism_assert" && id != "__prism_step") {
                out.append(id);
                i = j;
                continue;
            }
            std::size_t p = skip_ws(whole, j);
            std::size_t pe = p < to && whole[p] == '(' ? group_end(whole, p) : std::string::npos;
            if (pe == std::string::npos || pe > to) {
                err = std::string(id) + ": malformed hook";
                return out;
            }
            std::string inner = lower(whole, p + 1, pe - 1);
            if (id == "__prism_assume") {
                out += "__VERIFIER_assume(" + inner + ")";
                i = pe;
            } else if (id == "__prism_havoc") {
                out += "__prism_havoc_bytes((void*)&(" + inner + "), sizeof(" + inner + "))";
                i = pe;
            } else if (id == "__prism_assert") {
                auto comma = inner.find(',');
                int tag = -1;
                try {
                    tag = std::stoi(inner.substr(0, comma));
                } catch (const std::exception&) {
                }
                if (comma == std::string::npos || tag < 0 || tag >= 99999) {
                    err = "__prism_assert: tag is not a small integer literal";
                    return out;
                }
                tags.insert(tag);
                std::size_t semi = skip_ws(whole, pe);
                if (semi < to && whole[semi] == ';') pe = semi + 1;
                if (disabled.count(tag)) {
                    out += "((void)0);";
                } else {
                    out += "\n#line " + std::to_string(kTagLine + tag) + "\nassert(" + inner.substr(comma + 1) +
                           ");\n#line " + std::to_string(line_at(whole, pe)) + "\n";
                }
                i = pe;
            } else {  // __prism_step (c) { B }
                std::size_t b = skip_ws(whole, pe);
                std::size_t be = b < to && whole[b] == '{' ? group_end(whole, b) : std::string::npos;
                if (be == std::string::npos || be > to) {
                    err = "__prism_step: no block";
                    return out;
                }
                out += "if (" + inner + ") {" + lower(whole, b + 1, be - 1) + "\n__VERIFIER_assume(0); }";
                i = be;
            }
            if (!err.empty()) return out;
        }
        return out;
    }
};

}  // namespace

ProgramCheck check_program(const FunctionInfo& fn, const std::string& body, int unwind, unsigned timeout_ms,
                           bool only_invariants) {
    (void)timeout_ms;  // pir's own per-query limits apply
    ProgramCheck out;
    std::set<int> disabled;
    std::map<int, ProgramProp> refuted;
    std::set<int> all_tags;
    int sig_lines = 0;
    for (char ch : fn.signature) sig_lines += ch == '\n';
    const int base = std::max(1, fn.span.first) + sig_lines + 1;
    std::string rest = "unknown";
    for (int round = 0; round < 64; ++round) {
        Lowering lw{disabled, base, "", {}};
        auto text = lw.lower(body, 0, body.size());
        if (!lw.err.empty()) {
            out.error = lw.err;
            return out;
        }
        if (round == 0) all_tags = lw.tags;
        FunctionInfo g = fn;
        g.body = text;
        auto f = stages_detail::pir_one(g, unwind, only_invariants ? std::vector<std::string>{"assert"}
                                                                  : std::vector<std::string>{},
                                        stages_detail::kPirHookPrelude);
        const std::string& st = f.status;
        if (round == 0 && (st == laws::NOTRUN || st == laws::NEEDS_HARNESS || st == laws::ERROR || st == laws::NOFUNC)) {
            out.error = st + ": " + f.message;
            return out;
        }
        out.encoded = true;
        if (st == laws::PROVED || st == laws::PROVED_UNBOUNDED || st == laws::PROVED_CERTIFIED) {
            rest = "unsat";
            break;
        }
        if (st == laws::BOUNDED) {
            rest = "unsat";
            out.unwind_ok = false;
            break;
        }
        if (st != laws::FAILED) break;
        auto prop = f.extra.count("prop") ? f.extra.at("prop") : std::string();
        const int line = f.line.value_or(0);
        const int tag = line - kTagLine;
        if (prop == "assert" && all_tags.count(tag) && !disabled.count(tag)) {
            refuted[tag] = ProgramProp{"ai-inv#" + std::to_string(tag), f.cls, "sat", f.counterexample};
            disabled.insert(tag);
            continue;
        }
        if (!only_invariants)  // a UB (or a program assert) of the program itself
            out.props.push_back(ProgramProp{prop.empty() ? std::string("ub") : prop, f.cls, "sat", f.counterexample});
        break;
    }
    for (int t : all_tags) {
        if (auto it = refuted.find(t); it != refuted.end()) out.props.push_back(it->second);
        else out.props.push_back(ProgramProp{"ai-inv#" + std::to_string(t), "FUNC-CONTRACT", rest, ""});
    }
    return out;
}

}  // namespace prism::ai
