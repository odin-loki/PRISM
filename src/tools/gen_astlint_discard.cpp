// Build-time generator of prism/astlint_discard.inc (roadmap 2.8).
//
// The regex lints have a family of "<fn>() return is discarded" rules
// (lint_discarded* calls in src/prism/checkers_*.cpp, each one line-regex
// `^\s*name(...);$` and one taxonomy class). The Clang-AST layer ports the
// whole family as one check on the AST (a call that is an expression
// statement, to the library function of that name, not the project's own
// function), so it needs the callee-name -> class table. CMake runs this tool
// on the checkers sources at build time, so the two cannot drift.
//
//     prism_gen_astlint_discard OUT.inc checkers_a.cpp checkers_b.cpp ...
//
// The callee part of each regex is expanded into its finite set of names. The
// expander accepts only the regex subset a plain discarded-call rule uses
// (literals, groups, alternation, literal character classes, `?`, and a
// trailing `\w+`/`[..]*` repeat, which becomes a '*' prefix wildcard);
// lookarounds' negative form and anchors are ignored. A rule outside that
// subset is not guessed at: it is printed as "skipped (not a plain
// discarded-call rule)" and left to the regex layer (nothing is dropped
// silently). Standard library only; no PRISM code is linked.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef PRISM_GEN_DISCARD_NO_MAIN
#define PRISM_GEN_DISCARD_INLINE
#else
#define PRISM_GEN_DISCARD_INLINE inline
#endif

namespace prism_gen_discard {

// ------------------------------------------------------------------ regex subset
// A parsed item, following the node kinds of a backtracking regex parser:
// Literal, In (character set: literal chars, or `other` for categories and
// ranges), Branch, Group (sub-sequence), Repeat, Skip (anchor / negative
// lookahead: matches no callee characters), Bad (anything else).
struct Item;
using Seq = std::vector<Item>;
struct Item {
    enum Kind { Literal, In, Branch, Group, Repeat, Skip, Bad } kind = Bad;
    char ch = 0;                    // Literal
    std::string chars;              // In: literal members in order
    bool in_other = false;          // In: holds a category or range
    std::vector<Seq> alts;          // Branch
    Seq sub;                        // Group / Repeat
    int lo = 0, hi = 0;             // Repeat; hi < 0 = unbounded
    std::string what;               // Bad: the construct
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    Seq parse() {
        auto r = alternation();
        if (i_ != s_.size()) throw std::runtime_error("unbalanced parenthesis");
        return r;
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;

    bool at_end() const { return i_ >= s_.size(); }
    char peek() const { return s_[i_]; }

    // A branch whose alternatives are all one literal or one literal set is
    // a character set (the parser this mirrors does the same rewrite; it
    // matters for "a single set under a repeat").
    static Seq make_branch(std::vector<Seq> alts) {
        if (alts.size() == 1) return std::move(alts[0]);
        bool set = true;
        for (auto& a : alts)
            if (a.size() != 1 || (a[0].kind != Item::Literal && !(a[0].kind == Item::In))) set = false;
        Item it;
        if (set) {
            it.kind = Item::In;
            for (auto& a : alts) {
                if (a[0].kind == Item::Literal) it.chars += a[0].ch;
                else {
                    it.chars += a[0].chars;
                    it.in_other = it.in_other || a[0].in_other;
                }
            }
        } else {
            it.kind = Item::Branch;
            it.alts = std::move(alts);
        }
        return Seq{it};
    }

    Seq alternation() {
        std::vector<Seq> alts{sequence()};
        while (!at_end() && peek() == '|') {
            ++i_;
            alts.push_back(sequence());
        }
        return make_branch(std::move(alts));
    }

    Seq sequence() {
        Seq out;
        while (!at_end() && peek() != '|' && peek() != ')') {
            Item a = atom();
            quantifier(out, std::move(a));
        }
        return out;
    }

    void quantifier(Seq& out, Item a) {
        if (at_end()) {
            out.push_back(std::move(a));
            return;
        }
        int lo = 0, hi = 0;
        char c = peek();
        if (c == '?') lo = 0, hi = 1;
        else if (c == '*') lo = 0, hi = -1;
        else if (c == '+') lo = 1, hi = -1;
        else if (c == '{') {
            auto e = s_.find('}', i_);
            auto body = e == std::string_view::npos ? std::string_view{} : s_.substr(i_ + 1, e - i_ - 1);
            auto comma = body.find(',');
            auto num = [](std::string_view v, int dflt) {
                if (v.empty()) return dflt;
                int n = 0;
                for (char d : v) {
                    if (d < '0' || d > '9') throw std::runtime_error("repeat");
                    n = n * 10 + (d - '0');
                }
                return n;
            };
            if (e == std::string_view::npos || body.empty()) {
                out.push_back(std::move(a));  // a literal '{'
                return;
            }
            if (comma == std::string_view::npos) lo = hi = num(body, 0);
            else {
                lo = num(body.substr(0, comma), 0);
                hi = num(body.substr(comma + 1), -1);
            }
            i_ = e;
        } else {
            out.push_back(std::move(a));
            return;
        }
        ++i_;
        if (!at_end() && peek() == '?') ++i_;               // lazy: the same names
        else if (!at_end() && peek() == '+') throw std::runtime_error("possessive repeat");
        Item r;
        r.kind = Item::Repeat;
        r.lo = lo;
        r.hi = hi;
        // A repeated non-capturing group repeats its contents.
        if (a.kind == Item::Group) r.sub = std::move(a.sub);
        else r.sub = Seq{std::move(a)};
        out.push_back(std::move(r));
    }

    Item escape_item(char e, bool in_class) {
        Item it;
        switch (e) {
            case 'w': case 'W': case 's': case 'S': case 'd': case 'D':
                it.kind = Item::In;
                it.in_other = true;
                return it;
            case 'b': case 'B': case 'A': case 'Z':
                if (in_class) throw std::runtime_error("escape in class");
                it.kind = Item::Skip;
                return it;
            case 'n': it.kind = Item::Literal; it.ch = '\n'; return it;
            case 't': it.kind = Item::Literal; it.ch = '\t'; return it;
            default:
                if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || (e >= '0' && e <= '9'))
                    throw std::runtime_error(std::string("escape \\") + e);
                it.kind = Item::Literal;
                it.ch = e;
                return it;
        }
    }

    Item atom() {
        char c = s_[i_++];
        Item it;
        if (c == '\\') {
            if (at_end()) throw std::runtime_error("trailing backslash");
            return escape_item(s_[i_++], false);
        }
        if (c == '^' || c == '$') {
            it.kind = Item::Skip;
            return it;
        }
        if (c == '.') throw std::runtime_error("ANY");
        if (c == '[') return char_class();
        if (c == '(') {
            enum { Capture, NonCapture, NegLook } mode = Capture;
            if (!at_end() && peek() == '?') {
                ++i_;
                if (s_.substr(i_, 1) == ":") ++i_, mode = NonCapture;
                else if (s_.substr(i_, 1) == "!") ++i_, mode = NegLook;
                else if (s_.substr(i_, 2) == "P<") {
                    auto e = s_.find('>', i_);
                    if (e == std::string_view::npos) throw std::runtime_error("group name");
                    i_ = e + 1;
                } else throw std::runtime_error("group flags");
            }
            Seq inner = alternation();
            if (at_end() || peek() != ')') throw std::runtime_error("missing )");
            ++i_;
            if (mode == NegLook) {
                it.kind = Item::Skip;
                return it;
            }
            it.kind = Item::Group;
            it.sub = std::move(inner);
            return it;
        }
        if (c == ')' || c == '*' || c == '+' || c == '?') throw std::runtime_error("nothing to repeat");
        it.kind = Item::Literal;
        it.ch = c;
        return it;
    }

    Item char_class() {
        Item it;
        it.kind = Item::In;
        if (!at_end() && peek() == '^') throw std::runtime_error("character class");
        bool first = true;
        while (true) {
            if (at_end()) throw std::runtime_error("unterminated class");
            char c = s_[i_++];
            if (c == ']' && !first) break;
            first = false;
            if (c == '\\') {
                if (at_end()) throw std::runtime_error("trailing backslash");
                auto e = escape_item(s_[i_++], true);
                if (e.kind == Item::In) it.in_other = true;
                else it.chars += e.ch;
                continue;
            }
            if (!at_end() && peek() == '-' && i_ + 1 < s_.size() && s_[i_ + 1] != ']') {
                it.in_other = true;  // a range
                i_ += 2;
                continue;
            }
            it.chars += c;
        }
        // One literal member is that literal.
        if (!it.in_other && it.chars.size() == 1) {
            Item l;
            l.kind = Item::Literal;
            l.ch = it.chars[0];
            return l;
        }
        return it;
    }
};

// Finite expansions of a parsed sequence; a trailing '*' marks a \w+ tail.
PRISM_GEN_DISCARD_INLINE std::vector<std::string> expand(const Seq& items) {
    std::vector<std::string> outs{""};
    auto cross = [&](const std::vector<std::string>& tails) {
        std::vector<std::string> n;
        for (auto& o : outs)
            for (auto& t : tails) n.push_back(o + t);
        outs = std::move(n);
    };
    for (const auto& it : items) {
        switch (it.kind) {
            case Item::Literal: cross({std::string(1, it.ch)}); break;
            case Item::Group: cross(expand(it.sub)); break;
            case Item::Branch: {
                std::vector<std::string> alts;
                for (auto& b : it.alts) {
                    auto e = expand(b);
                    alts.insert(alts.end(), e.begin(), e.end());
                }
                cross(alts);
                break;
            }
            case Item::In: {
                if (it.in_other) throw std::runtime_error("character class");
                std::vector<std::string> cs;
                for (char c : it.chars) cs.emplace_back(1, c);
                cross(cs);
                break;
            }
            case Item::Repeat:
                if (it.lo == 0 && it.hi == 1) {
                    auto e = expand(it.sub);
                    e.insert(e.begin(), "");
                    cross(e);
                } else if (it.hi < 0 && it.sub.size() == 1 && it.sub[0].kind == Item::In) {
                    cross({"*"});
                } else {
                    throw std::runtime_error("repeat");
                }
                break;
            case Item::Skip: break;
            case Item::Bad: throw std::runtime_error(it.what);
        }
    }
    return outs;
}

PRISM_GEN_DISCARD_INLINE std::vector<std::string> expand_regex(std::string_view pattern) {
    return expand(Parser(pattern).parse());
}

// ------------------------------------------------------------------ source scan
struct Row {
    std::string name, cls;
    bool zero = false;
};
struct Skipped {
    std::string var, cls, why;
};

namespace detail {
inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
inline bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
inline void skip_ws(std::string_view s, std::size_t& i) {
    while (i < s.size() && is_ws(s[i])) ++i;
}
inline bool lit(std::string_view s, std::size_t& i, std::string_view w) {
    if (s.substr(i, w.size()) != w) return false;
    i += w.size();
    return true;
}
inline std::string word(std::string_view s, std::size_t& i) {
    std::size_t b = i;
    while (i < s.size() && is_word(s[i])) ++i;
    return std::string(s.substr(b, i - b));
}
}  // namespace detail

// `static [const] Regex NAME ( R"(PATTERN)" )`: (offset, name, pattern).
PRISM_GEN_DISCARD_INLINE std::vector<std::tuple<std::size_t, std::string, std::string>> regex_defs(
    std::string_view s) {
    using namespace detail;
    std::vector<std::tuple<std::size_t, std::string, std::string>> out;
    for (std::size_t at = s.find("static"); at != std::string_view::npos; at = s.find("static", at + 1)) {
        std::size_t i = at + 6;
        std::size_t w = i;
        skip_ws(s, i);
        if (i == w) continue;
        std::size_t c = i;
        if (lit(s, c, "const")) {
            std::size_t w2 = c;
            skip_ws(s, c);
            if (c > w2) i = c;
        }
        if (!lit(s, i, "Regex")) continue;
        w = i;
        skip_ws(s, i);
        if (i == w) continue;
        auto name = word(s, i);
        if (name.empty()) continue;
        skip_ws(s, i);
        if (!lit(s, i, "(")) continue;
        skip_ws(s, i);
        if (!lit(s, i, "R\"(")) continue;
        // The shortest body followed by `)"`, spaces, `)`.
        for (std::size_t e = s.find(")\"", i); e != std::string_view::npos; e = s.find(")\"", e + 1)) {
            std::size_t k = e + 2;
            skip_ws(s, k);
            if (k < s.size() && s[k] == ')') {
                out.emplace_back(at, name, std::string(s.substr(i, e - i)));
                at = k;  // matches do not overlap
                break;
            }
        }
    }
    return out;
}

// `^\s*[\b]CALLEE\s*\([^;]*\)\s*;\s*$` -> CALLEE.
PRISM_GEN_DISCARD_INLINE std::optional<std::string> callee_part(std::string_view pat) {
    constexpr std::string_view head = R"(^\s*)", suffix = R"(\s*\([^;]*\)\s*;\s*$)";
    if (!pat.starts_with(head) || !pat.ends_with(suffix) || pat.size() < head.size() + suffix.size())
        return std::nullopt;
    auto mid = pat.substr(head.size(), pat.size() - head.size() - suffix.size());
    if (mid.starts_with(R"(\b)")) mid.remove_prefix(2);
    if (mid.find('\n') != std::string_view::npos) return std::nullopt;
    return std::string(mid);
}

// Scans one checkers source: the rows of every lint_discarded* call, in order.
PRISM_GEN_DISCARD_INLINE void scan_source(std::string_view s, std::vector<Row>& rows, std::vector<Skipped>& skipped) {
    using namespace detail;
    const auto defs = regex_defs(s);
    constexpr std::string_view kCall = "lint_discarded";
    for (std::size_t at = s.find(kCall); at != std::string_view::npos; at = s.find(kCall, at + 1)) {
        const std::size_t start = at;
        std::size_t i = at + kCall.size();
        std::string fn = "lint_discarded";
        for (std::string_view suf : {"_which", "_zero", "_fn"}) {
            std::size_t j = i;
            if (lit(s, j, suf)) {
                std::size_t k = j;
                skip_ws(s, k);
                if (k < s.size() && s[k] == '(') {
                    i = j;
                    fn += suf;
                }
                break;
            }
        }
        skip_ws(s, i);
        if (!lit(s, i, "(")) continue;
        skip_ws(s, i);
        bool shape = lit(s, i, "funcs,");
        skip_ws(s, i);
        shape = shape && lit(s, i, "lines,");
        skip_ws(s, i);
        shape = shape && lit(s, i, "rel,");
        skip_ws(s, i);
        if (!shape) continue;
        auto var = word(s, i);
        if (var.empty() || !lit(s, i, ",")) continue;
        skip_ws(s, i);
        if (!lit(s, i, "\"")) continue;
        std::size_t b = i;
        while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) ++i;
        if (i == b || i >= s.size() || s[i] != '"') continue;
        std::string cls(s.substr(b, i - b));
        at = i;  // matches do not overlap
        const std::string* pat = nullptr;
        for (auto& [off, name, p] : defs)
            if (name == var && off < start) pat = &p;
        if (!pat) {
            skipped.push_back({var, cls, "no regex"});
            continue;
        }
        auto callee = callee_part(*pat);
        if (!callee) {
            skipped.push_back({var, cls, *pat});
            continue;
        }
        try {
            for (auto& n : expand_regex(*callee)) rows.push_back({n, cls, fn == "lint_discarded_zero"});
        } catch (const std::exception& e) {
            skipped.push_back({var, cls, *pat + " (" + e.what() + ")"});
        }
    }
}

// The .inc text: first row per callee wins, sorted by callee.
PRISM_GEN_DISCARD_INLINE std::string render(const std::vector<Row>& rows) {
    std::map<std::string, std::pair<std::string, bool>> seen;
    for (auto& r : rows) seen.try_emplace(r.name, r.cls, r.zero);
    std::string out =
        "// Generated at build time by src/tools/gen_astlint_discard.cpp from the regex\n"
        "// lints' lint_discarded* rules (src/prism/checkers_*.cpp). Do not edit.\n"
        "// {callee, {class, first argument must be the literal 0}}; a trailing '*'\n"
        "// matches any suffix.\n";
    for (auto& [n, v] : seen)
        out += "    {\"" + n + "\", {\"" + v.first + "\", " + (v.second ? "true" : "false") + "}},\n";
    return out;
}

}  // namespace prism_gen_discard

#ifndef PRISM_GEN_DISCARD_NO_MAIN
int main(int argc, char** argv) {
    using namespace prism_gen_discard;
    if (argc < 3) {
        std::cerr << "usage: prism_gen_astlint_discard OUT.inc checkers_*.cpp...\n";
        return 2;
    }
    std::vector<std::string> files(argv + 2, argv + argc);
    std::sort(files.begin(), files.end());
    std::vector<Row> rows;
    std::vector<Skipped> skipped;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            std::cerr << "prism_gen_astlint_discard: cannot read " << f << "\n";
            return 1;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        scan_source(ss.str(), rows, skipped);
    }
    for (auto& s : skipped)
        std::cerr << "skipped (not a plain discarded-call rule): " << s.var << " " << s.cls << " " << s.why << "\n";
    const auto text = render(rows);
    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    out << text;
    return out ? 0 : 1;
}
#endif
