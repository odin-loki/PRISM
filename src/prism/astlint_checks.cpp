// Roadmap 2.8: the checks of the Clang-AST lint layer.
//
// Input: a unit's top-level declarations in the -ast-dump=json schema, from
// either front end (astlint_internal.hpp). Output: FAILED rows with
// extra.engine = "clang-ast", at the line the matching regex lint uses where
// one exists (so run_lints_ast can supersede it).
//
// What is checked, and how far:
//   * every function body of the unit's main file and of the scanned project
//     headers it includes; class members and enums nested in classes and
//     namespaces;
//   * templates: the pattern is walked, but a finding whose expression
//     depends on a template parameter (type spelled with a parameter name,
//     "type-parameter-N-M", "<dependent type>", unresolved lookups) is
//     dropped: dependent code stays unchecked, non-dependent code in a
//     template is checked;
//   * flow checks (use after free / move, uninitialised reads, null after
//     assignment, allocation/deallocation pairs) are straight-line only: each
//     block's statements in order, a branch or loop body reached from the
//     block forgets what it touches. Functions with goto labels, setjmp or
//     inline asm are not flow-checked.

#include "astlint_internal.hpp"

#include "prism/checkers.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace prism::astlint::detail {
namespace fs = std::filesystem;

std::string canon_path(const fs::path& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : c).generic_string();
}

std::string record_name(std::string_view tv) {
    std::string t(tv);
    for (;;) {
        bool again = false;
        for (auto* q : {"const ", "volatile ", "struct ", "class ", "union ", "::"}) {
            if (t.starts_with(q)) {
                t.erase(0, std::strlen(q));
                again = true;
            }
        }
        if (!again) break;
    }
    while (!t.empty() && (t.back() == ' ' || t.back() == '*' || t.back() == '&')) t.pop_back();
    for (auto* q : {" const", " volatile"})
        while (t.size() > std::strlen(q) && t.ends_with(q)) t.erase(t.size() - std::strlen(q));
    if (t.find('(') != std::string::npos || t.find('[') != std::string::npos) return {};
    return t;
}

namespace {

const json kNullJson{};

// ---------------------------------------------------------------- node helpers

std::string str_field(const json& n, const char* key) {
    if (!n.is_object()) return {};
    auto it = n.find(key);
    if (it == n.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

std::string kind(const json& n) {
    if (!n.is_object()) return {};
    auto it = n.find("kind");
    if (it == n.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool flag(const json& n, const char* key) {
    if (!n.is_object()) return false;
    auto it = n.find(key);
    return it != n.end() && it->is_boolean() && it->get<bool>();
}

const json& inner(const json& n) {
    if (!n.is_object()) return kNullJson;
    auto it = n.find("inner");
    if (it == n.end()) return kNullJson;
    return *it;
}

std::size_t n_children(const json& n) {
    const auto& in = inner(n);
    return in.is_array() ? in.size() : 0;
}

const json& child(const json& n, std::size_t i) {
    const auto& in = inner(n);
    if (!in.is_array() || i >= in.size()) return kNullJson;
    return in.at(i);
}

const json& last_child(const json& n) {
    auto c = n_children(n);
    return c ? child(n, c - 1) : kNullJson;
}

const json& ref_decl(const json& n) {
    if (!n.is_object()) return kNullJson;
    auto it = n.find("referencedDecl");
    if (it == n.end()) return kNullJson;
    return *it;
}

std::string qual_type(const json& n) {
    if (!n.is_object()) return {};
    auto it = n.find("type");
    if (it == n.end() || !it->is_object()) return {};
    return str_field(*it, "qualType");
}

std::string type_of(const json& n) {
    if (!n.is_object()) return {};
    auto it = n.find("type");
    if (it == n.end() || !it->is_object()) return {};
    auto d = it->find("desugaredQualType");
    std::string t = d != it->end() && d->is_string() ? d->get<std::string>() : str_field(*it, "qualType");
    for (;;) {
        if (t.starts_with("const ")) t.erase(0, 6);
        else if (t.starts_with("volatile ")) t.erase(0, 9);
        else break;
    }
    return t;
}

bool is_implicit_wrapper(std::string_view k) {
    return k == "ImplicitCastExpr" || k == "ExprWithCleanups" || k == "ConstantExpr" ||
           k == "MaterializeTemporaryExpr" || k == "CXXBindTemporaryExpr" || k == "FullExpr" ||
           k == "UnexposedExpr";  // libclang: implicit casts and similar wrappers
}

// Strip implicit conversions (not parentheses: `if ((x = f()))` says "meant").
const json& strip_implicit(const json& n) {
    const json* p = &n;
    while (is_implicit_wrapper(kind(*p)) && n_children(*p) >= 1) p = &child(*p, 0);
    return *p;
}

const json& strip_all(const json& n) {
    const json* p = &n;
    while ((is_implicit_wrapper(kind(*p)) || kind(*p) == "ParenExpr") && n_children(*p) >= 1) p = &child(*p, 0);
    return *p;
}

bool is_explicit_cast(std::string_view k) {
    return k == "CStyleCastExpr" || k == "CXXStaticCastExpr" || k == "CXXReinterpretCastExpr" ||
           k == "CXXConstCastExpr" || k == "CXXFunctionalCastExpr";
}

// Also through explicit casts (for "what object does this point at").
const json& strip_casts(const json& n) {
    const json* p = &n;
    while ((is_implicit_wrapper(kind(*p)) || kind(*p) == "ParenExpr" || is_explicit_cast(kind(*p))) &&
           n_children(*p) >= 1)
        p = &child(*p, 0);
    return *p;
}

const json& strip_parens(const json& n) {
    const json* p = &n;
    while (kind(*p) == "ParenExpr" && n_children(*p) >= 1) p = &child(*p, 0);
    return *p;
}

bool one_of(std::string_view s, std::initializer_list<std::string_view> xs) {
    return std::any_of(xs.begin(), xs.end(), [&](std::string_view x) { return s == x; });
}

bool is_signed_int(const std::string& t) {
    return one_of(t, {"int", "long", "long long", "short", "signed char", "char", "long int", "short int",
                      "long long int", "signed", "signed int", "signed long", "__int128", "__int128_t"});
}

bool is_unsigned_int(const std::string& t) {
    if (t.find('*') != std::string::npos || t.find('[') != std::string::npos) return false;
    if (t.starts_with("enum ")) return false;
    return t.starts_with("unsigned") || t == "__uint128_t";
}

bool is_floating(const std::string& t) {
    return one_of(t, {"float", "double", "long double", "_Float16", "__fp16", "__float128"});
}

bool is_builtin_arith(const std::string& t) {
    return is_signed_int(t) || is_unsigned_int(t) || is_floating(t) ||
           one_of(t, {"bool", "_Bool", "wchar_t", "char8_t", "char16_t", "char32_t"});
}

bool is_pointer_type(const std::string& t) {
    auto star = t.rfind('*');
    if (star == std::string::npos) return false;
    std::string rest = t.substr(star + 1);
    std::istringstream ss(rest);
    std::string w;
    while (ss >> w)
        if (!one_of(w, {"const", "volatile", "restrict", "__restrict", "__restrict__"})) return false;
    return true;
}

bool is_int_type(const std::string& t) { return is_signed_int(t) || is_unsigned_int(t); }

// Bit width of an integer type (LP64).
int int_width(const std::string& t) {
    if (one_of(t, {"char", "signed char", "unsigned char", "bool", "_Bool", "char8_t"})) return 8;
    if (one_of(t, {"short", "short int", "unsigned short", "unsigned short int", "char16_t"})) return 16;
    if (one_of(t, {"int", "signed", "signed int", "unsigned int", "unsigned", "wchar_t", "char32_t"})) return 32;
    if (one_of(t, {"long", "long int", "unsigned long", "unsigned long int", "long long", "long long int",
                   "unsigned long long", "unsigned long long int", "signed long"}))
        return 64;
    if (one_of(t, {"__int128", "__int128_t", "unsigned __int128", "__uint128_t"})) return 128;
    return 0;
}

std::string decl_name(const json& n) {
    const auto& s = strip_all(n);
    if (kind(s) == "DeclRefExpr") return std::string(str_field(ref_decl(s), "name"));
    if (kind(s) == "MemberExpr") return std::string(str_field(s, "name"));
    return {};
}

std::string ref_id(const json& n) {
    const auto& s = strip_all(n);
    if (kind(s) != "DeclRefExpr") return {};
    return str_field(ref_decl(s), "id");
}

std::optional<long long> int_value(const json& n0) {
    const auto& n = strip_all(n0);
    const auto& k = kind(n);
    if (k == "IntegerLiteral" || k == "CharacterLiteral") {
        auto it = n.find("value");
        if (it == n.end()) return std::nullopt;
        try {
            if (it->is_number_integer()) return it->get<long long>();
            if (it->is_string()) return std::stoll(it->get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
        return std::nullopt;
    }
    if (k == "UnaryOperator" && str_field(n, "opcode") == "-" && n_children(n) == 1) {
        auto v = int_value(child(n, 0));
        if (v) return -*v;
    }
    if (k == "DeclRefExpr" && str_field(ref_decl(n), "kind") == "EnumConstantDecl") return std::nullopt;
    return std::nullopt;
}

bool is_zero_literal(const json& n) {
    auto v = int_value(n);
    return v && *v == 0;
}

std::optional<double> float_value(const json& n0) {
    const auto& n = strip_all(n0);
    if (kind(n) != "FloatingLiteral") return std::nullopt;
    const auto& v = str_field(n, "value");
    try {
        return std::stod(std::string(v));
    } catch (...) {
        return std::nullopt;
    }
}

bool is_null_ptr_expr(const json& n0) {
    const auto& n = strip_casts(n0);
    const auto& k = kind(n);
    if (k == "CXXNullPtrLiteralExpr" || k == "GNUNullExpr") return true;
    return is_zero_literal(n);
}

// No variable, call or memory access anywhere below: a constant expression.
bool constant_expr(const json& n) {
    const auto& k = kind(n);
    if (k == "DeclRefExpr") return str_field(ref_decl(n), "kind") == "EnumConstantDecl";
    if (k == "CallExpr" || k == "CXXMemberCallExpr" || k == "CXXOperatorCallExpr" || k == "MemberExpr" ||
        k == "ArraySubscriptExpr" || k == "CXXConstructExpr" || k == "StmtExpr")
        return false;
    if (k == "UnaryOperator" && one_of(str_field(n, "opcode"), {"*", "++", "--"})) return false;
    for (auto& c : inner(n))
        if (!constant_expr(c)) return false;
    return true;
}

// String literal contents (decoded).
std::optional<std::string> string_lit(const json& n0) {
    const auto& n = strip_all(n0);
    if (kind(n) != "StringLiteral") return std::nullopt;
    if (auto it = n.find("str"); it != n.end() && it->is_string()) return it->get<std::string>();
    std::string v(str_field(n, "value"));
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return std::nullopt;  // L"..", u8"..": skip
    std::string out;
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
        char c = v.at(i);
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i + 1 > v.size()) break;
        char e = v.at(i);
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7': {
                int val = 0, cnt = 0;
                while (cnt < 3 && i + 1 < v.size() && v.at(i) >= '0' && v.at(i) <= '7') {
                    val = val * 8 + (v.at(i) - '0');
                    ++i;
                    ++cnt;
                }
                --i;
                out += static_cast<char>(val);
                break;
            }
            case 'x': {
                int val = 0;
                while (i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v.at(i + 1)))) {
                    ++i;
                    val = val * 16 + (std::isdigit(static_cast<unsigned char>(v.at(i))) ? v.at(i) - '0'
                                                                                          : (std::tolower(v.at(i)) - 'a' + 10));
                }
                out += static_cast<char>(val);
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

// Result type of a function type spelling ("const int &(int)" -> "const int &").
std::string result_type(std::string_view fnt) {
    if (auto arrow = fnt.find(") -> "); arrow != std::string_view::npos) return std::string(fnt.substr(arrow + 5));
    auto p = fnt.find('(');
    if (p == std::string_view::npos) return {};
    std::string r(fnt.substr(0, p));
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

bool fn_noexcept(std::string_view fnt) {
    auto p = fnt.rfind(')');
    if (p == std::string_view::npos) return false;
    auto tail = fnt.substr(p);
    if (tail.find("noexcept(false)") != std::string_view::npos) return false;
    if (tail.find("noexcept") != std::string_view::npos) return true;
    // "void () throw()": the empty dynamic exception specification.
    return fnt.ends_with("throw()");
}

// ---------------------------------------------------------------- positions

struct Pos {
    const std::string* file = nullptr;
    long long offset = -1;
    bool macro = false;
};

Pos loc_pos(const json& l) {
    if (!l.is_object()) return {};
    if (auto it = l.find("expansionLoc"); it != l.end()) {
        Pos p = loc_pos(*it);
        p.macro = true;
        return p;
    }
    auto f = l.find("_f");
    auto o = l.find("offset");
    if (f == l.end() || o == l.end() || !f->is_string() || !o->is_number_integer()) return {};
    Pos p;
    p.offset = o->get<long long>();
    p.macro = false;
    if (f->is_string()) {
        thread_local std::string loc_file;
        loc_file = f->get<std::string>();
        p.file = &loc_file;
    }
    return p;
}

Pos begin_of(const json& n) {
    if (!n.is_object()) return {};
    if (auto r = n.find("range"); r != n.end() && r->is_object()) {
        if (auto b = r->find("begin"); b != r->end()) {
            auto p = loc_pos(*b);
            if (p.file) return p;
        }
    }
    if (auto l = n.find("loc"); l != n.end()) return loc_pos(*l);
    return {};
}

Pos end_of(const json& n) {
    if (!n.is_object()) return {};
    if (auto r = n.find("range"); r != n.end() && r->is_object()) {
        if (auto e = r->find("end"); e != r->end()) return loc_pos(*e);
    }
    return {};
}

Pos name_of(const json& n) {
    if (auto l = n.find("loc"); l != n.end()) return loc_pos(*l);
    return {};
}

bool in_macro(const json& n) { return begin_of(n).macro; }

// ---------------------------------------------------------------- dependent code

bool dependent_kind(std::string_view k) {
    return k == "CXXDependentScopeMemberExpr" || k == "UnresolvedLookupExpr" || k == "UnresolvedMemberExpr" ||
           k == "CXXUnresolvedConstructExpr" || k == "DependentScopeDeclRefExpr" || k == "ParenListExpr" ||
           k == "PackExpansionExpr" || k == "SizeOfPackExpr" || k == "CXXFoldExpr" ||
           k == "DependentCoawaitExpr";
}

bool has_word(const std::string& s, const std::string& w) {
    for (auto p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) {
        bool l = p == 0 || !(std::isalnum(static_cast<unsigned char>(s.at(p - 1))) || s.at(p - 1) == '_');
        auto e = p + w.size();
        bool r = e >= s.size() || !(std::isalnum(static_cast<unsigned char>(s.at(e))) || s.at(e) == '_');
        if (l && r) return true;
    }
    return false;
}

bool dependent_type(const json& n, const std::vector<std::string>& params) {
    auto it = n.find("type");
    if (it == n.end() || !it->is_object()) return false;
    for (auto* key : {"qualType", "desugaredQualType"}) {
        const auto& t = str_field(*it, key);
        if (t.empty()) continue;
        if (t.find("dependent type") != std::string::npos || t.find("type-parameter-") != std::string::npos)
            return true;
        for (auto& p : params)
            if (has_word(std::string(t), p)) return true;
    }
    return false;
}

bool is_dependent(const json& n, const std::vector<std::string>& params) {
    if (!n.is_object()) return false;
    if (dependent_kind(kind(n)) || flag(n, "dependent")) return true;
    if (dependent_type(n, params)) return true;
    const auto& rd = ref_decl(n);
    if (rd.is_object() && one_of(str_field(rd, "kind"), {"NonTypeTemplateParmDecl", "NonTypeTemplateParameter"}))
        return true;
    for (auto& c : inner(n))
        if (is_dependent(c, params)) return true;
    return false;
}

// ---------------------------------------------------------------- context

struct FileCtx {
    std::string rel;
    std::string text;
    std::vector<std::size_t> starts;
    std::vector<std::string> lines;  // comment-stripped, for evidence
};

struct EnumInfo {
    std::string name;
    std::vector<std::pair<std::string, long long>> consts;
    bool values_known = true;
};

struct Ctx {
    const FileSet& files;
    Unit& unit;
    std::map<std::string, FileCtx> fctx;
    std::map<std::string, EnumInfo> enums;                                          // EnumDecl id -> info
    std::unordered_map<std::string, std::string> enum_of;                           // constant id -> enum id
    std::unordered_map<std::string, std::pair<std::string, long long>> const_info;  // id -> name, value
    std::vector<Finding> out;
    std::set<std::string> checked;  // canonical files whose declarations were walked
    std::set<std::string> defined;  // dump path: names of functions defined in checked files
    // CTRL-SIBLING-ASYMMETRY: integer parameters of the functions in a file.
    struct SibParam {
        std::string file, fn, name, type;
        bool guarded = false, tested = false;
        Pos subscript{};
    };
    std::vector<SibParam> sib_params;
    // LOCK-ORDER: the first two distinct locks each function acquires.
    struct LockOrd {
        std::string file, fn, first, second;
        Pos at{};
    };
    std::vector<LockOrd> lock_ords;

    FileCtx* file(const std::string* canon) {
        if (!canon || !files.wanted(*canon)) return nullptr;
        auto it = fctx.find(*canon);
        if (it != fctx.end()) return &it->second;
        FileCtx f;
        f.rel = files.rel_of(*canon);
        if (auto s = files.sources.find(*canon); s != files.sources.end()) {
            f.text = s->second;
        } else {
            std::ifstream in(fs::path(*canon), std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            f.text = ss.str();
        }
        f.starts.push_back(0);
        for (std::size_t i = 0; i < f.text.size(); ++i)
            if (f.text[i] == '\n') f.starts.push_back(i + 1);
        std::istringstream ls(strip_comments_keep_lines(f.text, true));
        std::string line;
        while (std::getline(ls, line)) f.lines.push_back(line);
        return &fctx.emplace(*canon, std::move(f)).first->second;
    }

    int line_of(const Pos& p) {
        auto* f = file(p.file);
        if (!f || p.offset < 0) return 0;
        auto it = std::upper_bound(f->starts.begin(), f->starts.end(), static_cast<std::size_t>(p.offset));
        return static_cast<int>(it - f->starts.begin());
    }

    std::string_view text(const Pos& b, const Pos& e) {
        if (!b.file || !e.file || *b.file != *e.file || b.offset < 0 || e.offset < b.offset) return {};
        auto* f = file(b.file);
        if (!f || static_cast<std::size_t>(e.offset) > f->text.size()) return {};
        return std::string_view(f->text).substr(static_cast<std::size_t>(b.offset),
                                                static_cast<std::size_t>(e.offset - b.offset));
    }

    std::string_view text_of(const json& n) {
        auto b = begin_of(n), e = end_of(n);
        if (b.macro || e.macro) return {};
        auto t = text(b, e);
        if (t.empty()) return t;
        // The end location is the start of the last token: extend over it.
        auto* f = file(e.file);
        std::size_t end = static_cast<std::size_t>(e.offset);
        while (end < f->text.size() &&
               (std::isalnum(static_cast<unsigned char>(f->text[end])) || f->text[end] == '_'))
            ++end;
        if (end == static_cast<std::size_t>(e.offset) && end < f->text.size()) ++end;
        return std::string_view(f->text).substr(static_cast<std::size_t>(b.offset),
                                                end - static_cast<std::size_t>(b.offset));
    }

    void add_at(const Pos& p, const std::string& fn, std::string_view cls, const std::string& msg,
                bool allow_macro = false) {
        if (p.macro && !allow_macro) return;
        int line = line_of(p);
        if (line <= 0) return;
        auto* f = file(p.file);
        std::vector<Finding> tmp;
        lint_add(tmp, f->rel, fn.empty() ? std::nullopt : std::optional<std::string>(fn), line, cls, msg,
                 f->lines);
        if (tmp.empty()) return;
        tmp.front().extra["engine"] = std::string(ENGINE);
        out.push_back(std::move(tmp.front()));
    }

    const DeclFacts* facts(const std::string& id) const {
        auto it = unit.facts.find(id);
        return it == unit.facts.end() ? nullptr : &it->second;
    }
};

const json* function_body(const json* decl);

struct BodySlice {
    std::string_view text;
    long long start = -1;
};

BodySlice fn_body_slice(Ctx& cx, const json* fn_node) {
    const json* body = function_body(fn_node);
    if (!body) return {};
    Pos bs = begin_of(*body);
    if (bs.offset < 0 && n_children(*body)) bs = begin_of(child(*body, 0));
    if (!bs.file || bs.offset < 0) return {};
    long long max_end = bs.offset;
    std::function<void(const json&)> visit = [&](const json& n) {
        if (!n.is_object()) return;
        Pos e = end_of(n);
        if (e.offset > max_end) max_end = e.offset;
        for (auto& c : inner(n)) visit(c);
    };
    visit(*body);
    auto* fc = cx.file(bs.file);
    if (!fc || max_end <= bs.offset) return {};
    return {std::string_view(fc->text).substr(static_cast<std::size_t>(bs.offset),
                                              static_cast<std::size_t>(max_end - bs.offset)),
            bs.offset};
}

// ---------------------------------------------------------------- enums

void collect_enums(const json& n, Ctx& cx) {
    if (!n.is_object()) {
        if (n.is_array())
            for (auto& c : n) collect_enums(c, cx);
        return;
    }
    if (kind(n) == "EnumDecl") {
        const std::string id = std::string(str_field(n, "id"));
        if (cx.enums.contains(id)) return;
        EnumInfo info;
        info.name = std::string(str_field(n, "name"));
        long long next = 0;
        for (auto& c : inner(n)) {
            if (kind(c) != "EnumConstantDecl") continue;
            long long v = next;
            if (auto it = c.find("value"); it != c.end() && it->is_number_integer()) {
                v = it->get<long long>();
            } else if (n_children(c) > 0) {
                const auto& init = child(c, 0);
                const auto& val = str_field(init, "value");
                if (kind(init) == "ConstantExpr" && !val.empty()) {
                    try {
                        v = std::stoll(std::string(val));
                    } catch (...) {
                        info.values_known = false;
                    }
                } else {
                    info.values_known = false;
                }
            }
            info.consts.emplace_back(std::string(str_field(c, "name")), v);
            cx.enum_of[std::string(str_field(c, "id"))] = id;
            cx.const_info[std::string(str_field(c, "id"))] = {std::string(str_field(c, "name")), v};
            next = v + 1;
        }
        if (!id.empty()) cx.enums[id] = std::move(info);
    }
    if (auto it = n.find("inner"); it != n.end()) collect_enums(*it, cx);
}

void collect_defined(const json& n, Ctx& cx) {
    if (!n.is_object()) {
        if (n.is_array())
            for (auto& c : n) collect_defined(c, cx);
        return;
    }
    if (kind(n) == "FunctionDecl" && !flag(n, "isImplicit")) {
        for (auto& c : inner(n))
            if (kind(c) == "CompoundStmt") cx.defined.insert(std::string(str_field(n, "name")));
        return;
    }
    if (one_of(kind(n), {"NamespaceDecl", "LinkageSpecDecl", "ExportDecl"}))
        for (auto& c : inner(n)) collect_defined(c, cx);
}

// Facts the dump path reads from the checked files' own declarations (the
// libclang path fills them from the referenced cursors directly).
void collect_facts(const json& n, Ctx& cx, const std::string& scope) {
    if (!n.is_object()) {
        if (n.is_array())
            for (auto& c : n) collect_facts(c, cx, scope);
        return;
    }
    const auto& k = kind(n);
    if (one_of(k, {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl",
                   "CXXConversionDecl"})) {
        const auto& id = str_field(n, "id");
        if (!id.empty() && !cx.unit.facts.contains(id)) {
            DeclFacts f;
            f.is_virtual = flag(n, "virtual");
            // "The project's own function": defined (with a body) in a checked
            // file. A bare prototype of a libc name is still the libc function.
            f.user = cx.defined.contains(std::string(str_field(n, "name"))) && k == "FunctionDecl";
            for (auto& c : inner(n)) {
                const auto& ck = kind(c);
                if (ck == "WarnUnusedResultAttr") f.nodiscard = true;
                if (one_of(ck, {"NoReturnAttr", "CXX11NoReturnAttr", "C11NoReturnAttr"})) f.noreturn = true;
            }
            auto rt = result_type(std::string(qual_type(n)));
            f.result = rt.ends_with("&&") ? "xvalue" : rt.ends_with("&") ? "lvalue" : "prvalue";
            cx.unit.facts[id] = f;
        }
    }
    std::string sub = scope;
    if (k == "NamespaceDecl" && !str_field(n, "name").empty())
        sub = scope + std::string(str_field(n, "name")) + "::";
    if ((k == "CXXRecordDecl" || k == "RecordDecl") && !flag(n, "isImplicit") && n.contains("inner")) {
        auto name = scope + std::string(str_field(n, "name"));
        if (!str_field(n, "name").empty() && !cx.unit.records.contains(name)) {
            RecordFacts r;
            if (auto dd = n.find("definitionData"); dd != n.end()) r.polymorphic = flag(*dd, "isPolymorphic");
            for (auto& c : inner(n)) {
                if (kind(c) == "FinalAttr") r.is_final = true;
                if (kind(c) == "CXXDestructorDecl" && flag(c, "virtual")) r.virtual_dtor = true;
                if (kind(c) == "CXXMethodDecl" && flag(c, "virtual")) r.polymorphic = true;
            }
            if (auto b = n.find("bases"); b != n.end() && b->is_array())
                for (auto& base : *b) r.bases.push_back(record_name(type_of(base)));
            cx.unit.records[name] = r;
        }
        sub = scope + str_field(n, "name") + "::";
    }
    if (auto it = n.find("inner"); it != n.end()) collect_facts(*it, cx, sub);
}

// ---------------------------------------------------------------- per-function state

struct Var {
    std::string name;
    int reads = 0;
    const json* first_write = nullptr;
};

struct Fn {
    const json* node = nullptr;
    std::string name;
    std::string kind;
    std::string ret;  // result type spelling
    bool noexcept_ = false;
    std::unordered_set<std::string> ptr_params;             // ParmVarDecl ids of pointer type
    std::unordered_set<std::string> array_params;           // declared with [] syntax
    std::unordered_map<std::string, const json*> locals;    // VarDecl / ParmVarDecl id -> node
    std::map<std::string, Var> vars;                        // dead-store candidates
    std::vector<std::string> order;
    std::vector<std::string> tparams;                       // enclosing template parameter names
    bool opaque = false;     // inline asm / setjmp: data flow is not visible
    bool has_labels = false; // goto: blocks are not straight-line
    std::vector<const json*> blocks;  // CompoundStmt bodies for the flow pass
    std::unordered_set<std::string> mutex_locals;   // local pthread_mutex_t / mtx_t VarDecl ids
    std::unordered_set<std::string> mutex_inited;   // init or pthread_mutex_init / mtx_init
    bool str_null_arg_reported = false;
    bool str_null_member_reported = false;
    std::set<std::pair<std::string, std::string>> ptr_chain_done;
};

bool is_function_kind(const std::string& k) {
    return k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl" || k == "CXXDestructorDecl" ||
           k == "CXXConversionDecl";
}

bool is_template_kind(const std::string& k) {
    return k == "FunctionTemplateDecl" || k == "ClassTemplateDecl" || k == "ClassTemplatePartialSpecializationDecl" ||
           k == "VarTemplateDecl" || k == "TypeAliasTemplateDecl" || k == "VarTemplatePartialSpecializationDecl";
}

bool is_call_kind(const std::string& k) {
    return k == "CallExpr" || k == "CXXMemberCallExpr" || k == "CXXOperatorCallExpr";
}

std::string callee_name(const json& call) {
    if (n_children(call) == 0) return {};
    const auto& c = strip_all(child(call, 0));
    std::string n;
    if (kind(c) == "DeclRefExpr") n = str_field(ref_decl(c), "name");
    else return {};
    if (n.starts_with("__builtin___") && n.ends_with("_chk")) n = n.substr(12, n.size() - 16);
    else if (n.starts_with("__builtin_")) n = n.substr(10);
    return n;
}

// The declaration id a call resolves to (function or method).
std::string callee_id(const json& call) {
    if (n_children(call) == 0) return {};
    const auto& c = strip_all(child(call, 0));
    if (kind(c) == "DeclRefExpr") return str_field(ref_decl(c), "id");
    if (kind(c) == "MemberExpr") return str_field(c, "referencedMemberDecl");
    return {};
}

std::vector<const json*> call_args(const json& call) {
    std::vector<const json*> a;
    std::size_t from = kind(call) == "CXXConstructExpr" ? 0 : 1;
    for (std::size_t i = from; i < n_children(call); ++i) a.push_back(&child(call, i));
    return a;
}

bool is_noreturn_name(const std::string& n) {
    return one_of(n, {"exit", "_exit", "_Exit", "abort", "quick_exit", "longjmp", "siglongjmp", "__assert_fail",
                      "__assert_rtn", "_assert", "err", "errx", "verr", "verrx", "terminate", "unreachable",
                      "trap", "__cxa_throw", "pthread_exit", "thrd_exit", "__stack_chk_fail"});
}

std::string strip_ws(std::string s) {
    auto notsp = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notsp));
    s.erase(std::find_if(s.rbegin(), s.rend(), notsp).base(), s.end());
    return s;
}

std::string re_escape(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (strchr(R"(\[]{}()*+?.|^$)", c)) o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

bool is_ident(std::string_view s) {
    if (s.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(s.front())) && s.front() != '_') return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

// checkers_core.cpp unlock_of / lock_of. `(?i)` is PCRE2; std::regex rejects
// it and the catch in astlint.cpp turns the whole unit into NOTRUN.
std::string rx_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::optional<std::string> unlock_of(std::string_view name) {
    static const prism::Regex lock(R"((?i)lock)");
    auto hits = lock.finditer(name);
    if (hits.empty()) return std::nullopt;
    auto& m = hits.back();
    if (rx_upper(std::string(name)).find("ASSERT") != std::string::npos) return std::nullopt;
    int st = m.spans[0].first;
    int from = std::max(0, st - 2);
    std::string pre(name.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(st - from)));
    for (char& c : pre) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (pre == "un") return std::nullopt;
    auto got = m.text;
    std::string rep = std::all_of(got.begin(), got.end(),
                                  [](unsigned char c) { return std::isupper(c); })
                          ? "UNLOCK"
                          : "unlock";
    return std::string(name.substr(0, static_cast<std::size_t>(m.spans[0].first))) + rep +
           std::string(name.substr(static_cast<std::size_t>(m.spans[0].second)));
}

std::optional<std::string> lock_of(std::string_view name) {
    static const prism::Regex un(R"((?i)unlock)");
    auto hits = un.finditer(name);
    if (hits.empty()) return std::nullopt;
    if (rx_upper(std::string(name)).find("ASSERT") != std::string::npos) return std::nullopt;
    auto& m = hits.back();
    auto got = m.text;
    std::string rep;
    if (std::all_of(got.begin(), got.end(), [](unsigned char c) { return std::isupper(c); }))
        rep = "LOCK";
    else if (!got.empty() && std::isupper(static_cast<unsigned char>(got[0])))
        rep = "Lock";
    else
        rep = "lock";
    return std::string(name.substr(0, static_cast<std::size_t>(m.spans[0].first))) + rep +
           std::string(name.substr(static_cast<std::size_t>(m.spans[0].second)));
}

bool is_unlock_name(std::string_view name) {
    static const prism::Regex unlk(R"((?i)unlock)");
    return unlk.search(name);
}

bool lock_callee_ok(std::string_view name) {
    if (name.size() >= 5) {
        auto c0 = static_cast<unsigned char>(name[0]);
        if ((c0 == 'c' || c0 == 'C') && name.size() >= 5 &&
            std::tolower(static_cast<unsigned char>(name[1])) == 'l' &&
            std::tolower(static_cast<unsigned char>(name[2])) == 'o' &&
            std::tolower(static_cast<unsigned char>(name[3])) == 'c' &&
            std::tolower(static_cast<unsigned char>(name[4])) == 'k')
            return false;
    }
    const std::string s(name);
    static const std::regex block(R"(\w*[bB]lock\w*)");
    if (std::regex_match(s, block)) return false;
    static const std::regex lock(R"([A-Za-z_]\w*lock[A-Za-z0-9_]*)");
    return std::regex_match(s, lock);
}

bool is_mutex_local_type(std::string_view qt) {
    return qt.find("pthread_mutex_t") != std::string_view::npos || qt.find("mtx_t") != std::string_view::npos;
}

std::string lock_arg_key(const json& arg, Ctx& cx) {
    const json* a = &strip_all(strip_parens(arg));
    if (kind(*a) == "UnaryOperator" && str_field(*a, "opcode") == "&") a = &strip_all(child(*a, 0));
    if (kind(*a) == "DeclRefExpr") return str_field(ref_decl(*a), "name");
    return strip_ws(std::string(cx.text_of(*a)));
}

using LockKey = std::pair<std::string, std::string>;

int max_before_line(const std::vector<int>& hits, int line) {
    int best = -1;
    for (int h : hits)
        if (h <= line && h > best) best = h;
    return best;
}

bool regex_search_sv(std::string_view hay, const std::regex& re) {
    return std::regex_search(hay.begin(), hay.end(), re);
}

void collect_max_fields(std::string_view text, std::unordered_set<std::string>& out) {
    static const std::regex re("\\b(\\w+_max)\\b");
    const std::string s(text);
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) out.insert(it->str(1));
}

// checkers_core.cpp copy_len_guarded / below_max_tested (same patterns).
bool copy_len_guarded(std::string_view size, std::string_view prior,
                      const std::unordered_set<std::string>& max_names) {
    thread_local std::unordered_map<std::string, std::regex> cache;
    if (cache.size() > 4096) cache.clear();
    auto se = re_escape(size);
    for (auto& m : max_names) {
        std::string key = std::string(size) + "\x1f" + m;
        auto it = cache.find(key);
        if (it == cache.end()) {
            auto me = re_escape(m);
            std::string pat = "\\b" + se + "\\s*(?:<=|>=|>|<)\\s*[\\w\\->.]*" + me + "\\b|\\b[\\w\\->.]*" +
                              me + "\\s*(?:<=|>=|>|<)\\s*" + se + "\\b";
            it = cache.emplace(std::move(key), std::regex(pat)).first;
        }
        if (regex_search_sv(prior, it->second)) return true;
    }
    return false;
}

bool below_max_tested(std::string_view v, std::string_view max, std::string_view prior) {
    auto ve = re_escape(v), me = re_escape(max);
    std::string pat = "\\b" + ve + "\\s*<=?\\s*[\\w\\->.]*\\b" + me + "\\b|\\b[\\w\\->.]*" + me +
                      "\\s*>=?\\s*" + ve + "\\b|\\(\\s*" + ve + "\\s*=(?!=)[^;]*\\)\\s*<=?\\s*[\\w\\->.]*\\b" +
                      me + "\\b";
    static thread_local std::unordered_map<std::string, std::regex> cache;
    if (cache.size() > 4096) cache.clear();
    std::string key = std::string(v) + "\x1f" + std::string(max);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(std::move(key), std::regex(pat)).first;
    return regex_search_sv(prior, it->second);
}

// checkers_core.cpp param_null_tested / member_test_re / member_test_pos (same patterns).
const std::unordered_map<std::string, std::vector<int>> kStrNullFn = {
    {"strlen", {0}}, {"strcpy", {1}}, {"strcmp", {0, 1}},
};

bool param_null_tested(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    const std::string tests[] = {
        "\\bif\\s*\\(\\s*" + v + "\\s*==\\s*(?:NULL|nullptr|0)\\b",
        "\\bif\\s*\\(\\s*(?:NULL|nullptr|0)\\s*==\\s*" + v + "\\b",
        "\\bif\\s*\\(\\s*" + v + "\\s*!=\\s*(?:NULL|nullptr|0)\\b",
        "\\bif\\s*\\(\\s*(?:NULL|nullptr|0)\\s*!=\\s*" + v + "\\b",
        "\\bif\\s*\\(\\s*!\\s*" + v + "\\b",
        "(?:\\|\\||&&|\\()\\s*\\(?\\s*" + v + "\\s*[!=]=\\s*(?:NULL|nullptr|0)\\b",
        "(?:\\|\\||&&|\\()\\s*\\(?\\s*(?:NULL|nullptr|0)\\s*[!=]=\\s*" + v + "\\b",
        "(?:\\|\\||&&|\\()\\s*!\\s*" + v + "\\b(?!\\s*(?:->|\\.|\\[|\\())",
    };
    for (auto& p : tests)
        if (regex_search_sv(body, std::regex(p))) return true;
    return false;
}

const prism::Regex& member_test_re(std::string_view param, std::string_view member) {
    thread_local std::unordered_map<std::string, std::unique_ptr<prism::Regex>> cache;
    std::string key = std::string(param) + "\x1f" + std::string(member);
    if (auto it = cache.find(key); it != cache.end()) return *it->second;
    if (cache.size() > 4096) cache.clear();
    auto v = re_escape(param), m = re_escape(member);
    auto acc = "(?<![\\w.])(?<!->)(?:" + v + "\\s*->\\s*" + m + "|" + v + "\\s*\\.\\s*" + m + ")\\b";
    std::string nul = "(?:NULL|nullptr|0|[A-Za-z_]\\w*_NULL)\\b";
    std::string pat = "(?:" + acc + "\\s*[!=]=\\s*" + nul + "|\\b" + nul + "\\s*[!=]=\\s*" + acc +
                      "|!\\s*\\(?\\s*" + acc + "(?!\\s*(?:->|\\.|\\[|\\())" +
                      "|(?:\\b(?:if|while)\\s*\\(|&&)\\s*" + acc + "\\s*(?:\\)|&&|\\?)" + "|" + acc +
                      "\\s*(?:&&|\\?)" + "|\\b\\w*(?:assert|ASSERT)\\w*\\s*\\(\\s*" + acc + "\\s*\\)" +
                      "|\\b\\w*NOT_?NULL\\w*\\s*\\(\\s*" + acc + "\\s*(?:\\)|,|->))";
    auto [it, _] = cache.emplace(std::move(key), std::make_unique<prism::Regex>(pat));
    return *it->second;
}

std::size_t member_test_pos(std::string_view param, std::string_view member, std::string_view body) {
    auto m = member_test_re(param, member).search_match(body);
    if (!m || m->spans.empty() || m->spans[0].first < 0) return std::string_view::npos;
    return static_cast<std::size_t>(m->spans[0].first);
}

const json* function_body(const json* decl) {
    if (!decl) return nullptr;
    for (auto& c : inner(*decl)) {
        const auto& k = kind(c);
        if (k == "CompoundStmt" || k == "CXXTryStmt") return &c;
    }
    return nullptr;
}

bool is_value_returning_type(const std::string& ret) {
    if (ret.find('*') != std::string::npos) return false;
    static const std::regex voidb(R"(\bvoid\b)");
    if (std::regex_search(ret, voidb)) return false;
    static const std::regex value_ret(R"(\b(?:unsigned\s+)?(?:int|long|short|char)\b)");
    return std::regex_search(ret, value_ret);
}

bool function_decl_noreturn(const json& f) {
    for (auto& c : inner(f)) {
        if (one_of(kind(c), {"NoReturnAttr", "CXX11NoReturnAttr", "C11NoReturnAttr"})) return true;
    }
    const auto qt = qual_type(f);
    return qt.find("_Noreturn") != std::string::npos || qt.find("[[noreturn]]") != std::string::npos;
}

bool has_wrap_mul_text(std::string_view s) {
    static const std::regex a(R"((?:\([^)]*\)\s*)*[A-Za-z_]\w*\s*\*\s*sizeof\s*\()");
    static const std::regex b(R"(sizeof\s*\([^)]+\)\s*\*\s*(?:\([^)]*\)\s*)*[A-Za-z_]\w*)");
    const std::string t = strip_ws(std::string(s));
    return std::regex_search(t, a) || std::regex_search(t, b);
}

bool has_bounds_guard(std::string_view var, std::string_view body) {
    const auto v = re_escape(var);
    if (std::regex_search(body.begin(), body.end(),
                          std::regex("\\bif\\s*\\([^)]*" + v + "\\s*<\\s*0[^)]*\\|\\|[^)]*" + v + "\\s*>=")))
        return true;
    if (std::regex_search(body.begin(), body.end(),
                          std::regex("\\bif\\s*\\([^)]*" + v + "\\s*>=\\s*\\d+[^)]*\\|\\|[^)]*" + v + "\\s*<")))
        return true;
    const bool has_lower = std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*0\\s*>\\s*" + v + "\\b")) ||
                           std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*" + v + "\\s*<\\s*0\\b")) ||
                           std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*" + v + "\\s*<=\\s*-1\\b"));
    const bool has_upper = std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*" + v + "\\s*>=\\s*\\d+\\b")) ||
                           std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*" + v + "\\s*>\\s*\\d+\\b")) ||
                           std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*\\d+\\s*<=\\s*" + v + "\\b")) ||
                           std::regex_search(body.begin(), body.end(), std::regex("\\bif\\s*\\(\\s*\\d+\\s*>\\s*" + v + "\\b"));
    return has_lower && has_upper;
}

const json* last_executable_stmt(const json& stmt) {
    const auto& k = kind(stmt);
    if (k == "CompoundStmt") {
        const auto& in = inner(stmt);
        if (!in.is_array()) return nullptr;
        for (auto it = in.rbegin(); it != in.rend(); ++it) {
            if (const json* s = last_executable_stmt(*it)) return s;
        }
        return nullptr;
    }
    if (k == "NullStmt") return nullptr;
    if (one_of(k, {"LabelStmt", "CaseStmt", "DefaultStmt", "AttributedStmt"})) {
        return n_children(stmt) ? last_executable_stmt(last_child(stmt)) : nullptr;
    }
    return &stmt;
}

const json& stmt_simple_tail(const json& s) {
    const json* p = &s;
    while ((kind(*p) == "CompoundStmt" || kind(*p) == "AttributedStmt" || kind(*p) == "LabelStmt") &&
           n_children(*p))
        p = &last_child(*p);
    return *p;
}

std::optional<std::string> fatal_tail_callee(const json& stmt) {
    const auto& core = strip_all(stmt_simple_tail(stmt));
    if (!is_call_kind(kind(core))) return std::nullopt;
    const auto n = callee_name(core);
    if (one_of(n, {"exit", "_exit", "abort", "err", "errx"})) return n;
    return std::nullopt;
}

void collect_atoi_vars(const json& n, std::unordered_set<std::string>& out) {
    if (!n.is_object()) return;
    const auto& k = kind(n);
    if (k == "VarDecl" && n_children(n) >= 1) {
        const auto& init = strip_all(last_child(n));
        if (is_call_kind(kind(init)) && one_of(callee_name(init), {"atoi", "atol", "atoll"}))
            out.insert(str_field(n, "id"));
    }
    if (k == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2) {
        const auto& lhs = strip_all(child(n, 0));
        const auto& rhs = strip_all(child(n, 1));
        if (kind(lhs) == "DeclRefExpr" && is_call_kind(kind(rhs)) &&
            one_of(callee_name(rhs), {"atoi", "atol", "atoll"}))
            out.insert(str_field(ref_decl(lhs), "id"));
    }
    for (auto& c : inner(n)) collect_atoi_vars(c, out);
}

bool mem_copy_callee(std::string_view name) {
    return name.find("memcpy") != std::string_view::npos || name.find("memmove") != std::string_view::npos;
}

bool libc_api_lint(std::string_view name) {
    return one_of(name, {"malloc", "calloc", "realloc", "reallocarray", "aligned_alloc", "strdup", "strndup",
                         "getenv", "secure_getenv", "system", "signal", "gets", "strtok", "tmpnam", "tempnam",
                         "tmpnam_r", "mktemp", "umask", "mkstemp", "mkstemps", "chmod", "fchmod", "memset",
                         "memcpy", "memmove", "memcmp", "bcopy", "strcpy", "strncpy", "strcat", "strncat",
                         "sprintf", "vsprintf", "snprintf", "wcscpy", "wcscat", "alloca", "__builtin_alloca",
                         "srand", "srandom"});
}

bool mentions_ast_name(const json& n, std::string_view nm) {
    if (!n.is_object()) return false;
    if (kind(n) == "DeclRefExpr" && str_field(ref_decl(n), "name") == nm) return true;
    for (auto& c : inner(n))
        if (mentions_ast_name(c, nm)) return true;
    return false;
}

bool malloc_call_has_nowait(const json& call, Ctx& cx) {
    if (cx.text_of(call).find("M_NOWAIT") != std::string_view::npos) return true;
    for (auto* a : call_args(call))
        if (mentions_ast_name(*a, "M_NOWAIT")) return true;
    return false;
}

bool is_rand_call_expr(const json& n) {
    const auto& c = strip_casts(n);
    return is_call_kind(kind(c)) && one_of(callee_name(c), {"rand", "random"});
}

bool mentions_time_call(const json& n) {
    if (!n.is_object()) return false;
    if (is_call_kind(kind(n)) && one_of(callee_name(n), {"time", "time64", "time32"})) return true;
    for (auto& ch : inner(n))
        if (mentions_time_call(ch)) return true;
    return false;
}

bool fn_name_crypto_related(std::string_view fn) {
    std::string low(fn);
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("key") != std::string::npos || low.find("crypto") != std::string::npos;
}

bool has_literal_zero_or_one(const json& n) {
    if (!n.is_object()) return false;
    if (kind(n) == "IntegerLiteral") {
        if (auto v = int_value(n)) return *v == 0 || *v == 1;
    }
    for (auto& c : inner(n))
        if (has_literal_zero_or_one(c)) return true;
    return false;
}

bool sig_dfl_or_ign(const json& h0) {
    const json* p = &h0;
    for (int depth = 0; depth < 6 && p->is_object(); ++depth) {
        const auto& h = strip_all(*p);
        if (kind(h) == "DeclRefExpr") {
            const auto n = str_field(ref_decl(h), "name");
            return n == "SIG_DFL" || n == "SIG_IGN";
        }
        if (kind(h) == "IntegerLiteral") {
            if (auto v = int_value(h)) return *v == 0 || *v == 1;
        }
        if (kind(h) == "ParenExpr" || kind(h) == "UnexposedExpr" || is_explicit_cast(kind(h)) ||
            kind(h) == "CStyleCastExpr" || is_implicit_wrapper(kind(h))) {
            if (n_children(h) >= 1) {
                p = &child(h, 0);
                continue;
            }
        }
        break;
    }
    return false;
}

bool flow_terminates(const json& s) {
    const auto& k = kind(s);
    if (one_of(k, {"ReturnStmt", "GotoStmt", "BreakStmt", "ContinueStmt", "CXXThrowExpr"})) return true;
    if (k == "CompoundStmt") {
        if (!n_children(s)) return false;
        return flow_terminates(last_child(s));
    }
    if (k == "IfStmt") {
        std::size_t ci = (flag(s, "hasInit") ? 1 : 0) + (flag(s, "hasVar") ? 1 : 0);
        if (n_children(s) < ci + 2) return false;
        return flow_terminates(child(s, ci + 1)) &&
               (n_children(s) < ci + 3 || flow_terminates(child(s, ci + 2)));
    }
    return false;
}

// When cond is `!p` / `p == NULL`, returns p's id and whether the then-branch is the NULL case.
std::optional<std::string> null_test_var(const json& cond, bool& null_in_then) {
    const auto& c = strip_all(cond);
    null_in_then = true;
    const json* var = nullptr;
    if (kind(c) == "UnaryOperator" && str_field(c, "opcode") == "!") {
        var = &strip_all(child(c, 0));
    } else if (kind(c) == "BinaryOperator" && one_of(str_field(c, "opcode"), {"==", "!="})) {
        null_in_then = str_field(c, "opcode") == "==";
        const auto& lhs = strip_all(strip_parens(child(c, 0)));
        if (is_null_ptr_expr(child(c, 1)))
            var = &lhs;
        else if (is_null_ptr_expr(child(c, 0)))
            var = &strip_all(child(c, 1));
        else if (kind(lhs) == "BinaryOperator" && str_field(lhs, "opcode") == "=")
            var = &strip_all(child(lhs, 0));
    }
    if (!var || kind(*var) != "DeclRefExpr") return std::nullopt;
    auto id = str_field(ref_decl(*var), "id");
    return id.empty() ? std::nullopt : std::optional{id};
}

// ---------------------------------------------------------------- simple checks

struct Checker {
    Ctx& cx;
    Fn& fn;
    bool body_slice_ready = false;
    BodySlice body_cache_{};

    bool dep(const json& n) const { return is_dependent(n, fn.tparams); }

    BodySlice body_slice() {
        if (!body_slice_ready) {
            body_cache_ = fn.node ? fn_body_slice(cx, fn.node) : BodySlice{};
            body_slice_ready = true;
        }
        return body_cache_;
    }

    std::optional<std::pair<std::string, std::string>> ptr_member_arg(const json& arg0) const {
        const auto& s = strip_all(arg0);
        if (kind(s) != "MemberExpr" || n_children(s) == 0) return std::nullopt;
        const auto& base = strip_all(child(s, 0));
        if (kind(base) != "DeclRefExpr") return std::nullopt;
        if (!fn.ptr_params.contains(str_field(ref_decl(base), "id"))) return std::nullopt;
        return std::pair{str_field(ref_decl(base), "name"), str_field(s, "name")};
    }

    void add(const json& at, std::string_view cls, const std::string& msg, bool allow_macro = false) {
        if (dep(at)) return;
        cx.add_at(begin_of(at), fn.name, cls, msg, allow_macro);
    }
    void add_pos(const Pos& p, const json& subject, std::string_view cls, const std::string& msg) {
        if (dep(subject)) return;
        cx.add_at(p, fn.name, cls, msg);
    }

    const json* local_var(const json& n) const {
        const auto& s = strip_all(n);
        if (kind(s) != "DeclRefExpr") return nullptr;
        auto it = fn.locals.find(str_field(ref_decl(s), "id"));
        return it == fn.locals.end() ? nullptr : it->second;
    }

    // An automatic object of this function (a local or a by-value parameter).
    bool automatic(const json* v) const {
        if (!v) return false;
        if (!str_field(*v, "storageClass").empty()) return false;
        if (qual_type(*v).find('&') != std::string::npos) return false;
        return true;
    }

    std::optional<long long> local_array_size(const json& n) const {
        const auto* v = local_var(n);
        if (!v || kind(*v) != "VarDecl" || !automatic(v)) return std::nullopt;
        auto t = type_of(*v);
        auto lb = t.find('['), rb = t.find(']');
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) return std::nullopt;
        auto elem = t.substr(0, lb);
        while (!elem.empty() && elem.back() == ' ') elem.pop_back();
        if (!one_of(elem, {"char", "signed char", "unsigned char"})) return std::nullopt;
        try {
            return std::stoll(t.substr(lb + 1, rb - lb - 1));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<long long> local_wchar_array_size(const json& n) const {
        const auto* v = local_var(n);
        if (!v || kind(*v) != "VarDecl" || !automatic(v)) return std::nullopt;
        // The spelled type: wchar_t is a typedef in C, so the desugared type
        // (libclang's canonical spelling) reads int[N].
        std::string t;
        if (auto it = v->find("type"); it != v->end() && it->is_object()) t = str_field(*it, "qualType");
        auto lb = t.find('['), rb = t.find(']');
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) return std::nullopt;
        auto elem = t.substr(0, lb);
        while (!elem.empty() && elem.back() == ' ') elem.pop_back();
        if (elem.starts_with("const ")) elem.erase(0, 6);
        if (elem != "wchar_t") return std::nullopt;
        try {
            return std::stoll(t.substr(lb + 1, rb - lb - 1));
        } catch (...) {
            return std::nullopt;
        }
    }

    bool sizeof_matches_local_array(const json& size_arg, const json& buf) const {
        const auto* e = sizeof_operand(strip_all(size_arg));
        if (!e) return false;
        return same_expr(*e, buf);
    }

    // ---- 1. assignment as condition, loop sign, enum hole, self-assign (original layer)

    void assign_cond(const json& cond) {
        const auto& n = strip_implicit(cond);
        if (kind(n) != "BinaryOperator" || str_field(n, "opcode") != "=") return;
        auto lhs = decl_name(child(n, 0));
        add(n, "CTRL-ASSIGN-COND",
            "assignment " + (lhs.empty() ? std::string("") : "to " + lhs + " ") +
                "used as a condition; == was probably meant (extra parentheses mark it deliberate)");
    }

    void loop_sign(const json& cond) {
        const auto& n = strip_all(cond);
        if (kind(n) != "BinaryOperator") return;
        const auto& op = str_field(n, "opcode");
        if (!one_of(op, {"<", ">", "<=", ">="})) return;
        for (std::size_t side = 0; side < 2; ++side) {
            const auto& s = strip_parens(child(n, side));
            if (kind(s) != "ImplicitCastExpr" || str_field(s, "castKind") != "IntegralCast") continue;
            const auto& src = child(s, 0);
            if (!is_signed_int(type_of(src)) || !is_unsigned_int(type_of(s))) continue;
            const auto& other = child(n, 1 - side);
            if (!is_unsigned_int(type_of(other))) continue;
            const auto& core = strip_all(src);
            if (kind(core) == "IntegerLiteral" || kind(core) == "CharacterLiteral") continue;
            if (kind(core) == "DeclRefExpr" && str_field(ref_decl(core), "kind") == "EnumConstantDecl") continue;
            if (begin_of(core).macro) continue;
            auto sn = decl_name(src);
            auto un = decl_name(other);
            add(n, "INT-SIGN-CONV",
                "loop condition compares signed " + (sn.empty() ? std::string("value") : sn) + " with unsigned " +
                    (un.empty() ? std::string("value") : un) +
                    ": the signed side is converted to unsigned, so a negative value compares huge");
            return;
        }
    }

    static void label_enum_consts(const json& n, std::vector<std::string>& ids, bool& has_default,
                                  bool& non_enum) {
        if (!n.is_object()) return;
        const auto& k = kind(n);
        if (k == "SwitchStmt") return;  // nested switch: its own labels
        if (k == "DefaultStmt") has_default = true;
        if (k == "CaseStmt") {
            if (flag(n, "isGNURange")) non_enum = true;
            const auto& lab = strip_all(child(n, 0));
            if (kind(lab) == "DeclRefExpr" && str_field(ref_decl(lab), "kind") == "EnumConstantDecl")
                ids.push_back(str_field(ref_decl(lab), "id"));
            else
                non_enum = true;
        }
        for (auto& c : inner(n)) label_enum_consts(c, ids, has_default, non_enum);
    }

    static bool has_user_conversion(const json& n) {
        if (!n.is_object()) return false;
        if (str_field(n, "castKind") == "UserDefinedConversion") return true;
        if (kind(n) == "CXXMemberCallExpr") return true;
        for (auto& c : inner(n))
            if (has_user_conversion(c)) return true;
        return false;
    }

    void switch_hole(const json& sw) {
        std::size_t ci = (flag(sw, "hasInit") ? 1 : 0) + (flag(sw, "hasVar") ? 1 : 0);
        const auto& cond = child(sw, ci);
        const auto& body = child(sw, ci + 1);
        if (!cond.is_object() || !body.is_object()) return;
        if (has_user_conversion(cond)) return;
        if (is_builtin_arith(type_of(strip_all(cond)))) return;  // switch on an int: holes are normal
        std::vector<std::string> ids;
        bool has_default = false, non_enum = false;
        for (auto& c : inner(body)) label_enum_consts(c, ids, has_default, non_enum);
        if (has_default || non_enum || ids.empty()) return;
        std::string eid;
        for (auto& id : ids) {
            auto it = cx.enum_of.find(id);
            if (it == cx.enum_of.end()) return;  // an enumerator this unit did not show
            if (!eid.empty() && eid != it->second) return;
            eid = it->second;
        }
        auto en = cx.enums.find(eid);
        if (en == cx.enums.end()) return;
        std::set<long long> vals;
        std::set<std::string> names;
        for (auto& id : ids) {
            auto info = cx.const_info.find(id);
            if (info == cx.const_info.end()) return;
            names.insert(info->second.first);
            vals.insert(info->second.second);
        }
        std::string miss;
        for (auto& [nm, v] : en->second.consts) {
            bool covered = en->second.values_known ? vals.contains(v) : names.contains(nm);
            if (covered) continue;
            if (!miss.empty()) miss += ", ";
            miss += nm;
        }
        if (miss.empty()) return;
        auto var = decl_name(cond);
        std::string ename = en->second.name.empty() ? std::string("(anonymous)") : en->second.name;
        add(sw, "INT-ENUM-HOLE",
            "switch (" + (var.empty() ? std::string("...") : var) + ") on enum " + ename + " misses " + miss +
                " and has no default");
    }

    std::optional<unsigned> eval_mask_value(const json& e0) const {
        const auto& n = strip_all(e0);
        if (auto v = int_value(n)) return static_cast<unsigned>(*v);
        if (kind(n) == "DeclRefExpr" && str_field(ref_decl(n), "kind") == "EnumConstantDecl") {
            auto it = cx.const_info.find(str_field(ref_decl(n), "id"));
            if (it != cx.const_info.end()) return static_cast<unsigned>(it->second.second);
        }
        if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "|" && n_children(n) == 2) {
            auto a = eval_mask_value(child(n, 0));
            auto b = eval_mask_value(child(n, 1));
            if (a && b) return *a | *b;
        }
        if (kind(n) == "ParenExpr" && n_children(n)) return eval_mask_value(child(n, 0));
        return std::nullopt;
    }

    int count_mask_idents(const json& e0) const {
        int n = 0;
        std::function<void(const json&)> go = [&](const json& x) {
            if (!x.is_object()) return;
            if (kind(x) == "DeclRefExpr") {
                const auto nm = str_field(ref_decl(x), "name");
                if (!nm.empty() && nm != "if" && nm != "switch") ++n;
                return;
            }
            for (auto& c : inner(x)) go(c);
        };
        go(e0);
        return n;
    }

    static void count_switch_cases(const json& n, int& cases, bool& has_default) {
        if (!n.is_object()) return;
        const auto& k = kind(n);
        if (k == "DefaultStmt") has_default = true;
        if (k == "CaseStmt") ++cases;
        for (auto& c : inner(n)) count_switch_cases(c, cases, has_default);
    }

    static void collect_switch_assigns(const json& n, std::unordered_set<std::string>& out) {
        if (!n.is_object()) return;
        if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2) {
            const auto& lhs = strip_all(child(n, 0));
            if (kind(lhs) == "DeclRefExpr") out.insert(str_field(ref_decl(lhs), "id"));
        }
        for (auto& c : inner(n)) collect_switch_assigns(c, out);
    }

    static bool ref_after(const json& n, const std::string& id) {
        if (!n.is_object()) return false;
        if (kind(n) == "DeclRefExpr" && str_field(ref_decl(n), "id") == id) return true;
        for (auto& c : inner(n)) {
            if (ref_after(c, id)) return true;
        }
        return false;
    }

    static bool var_decl_uninit(const json& v) {
        if (kind(v) != "VarDecl" || flag(v, "isImplicit")) return false;
        for (auto& c : inner(v)) {
            const auto& ck = kind(c);
            if (ck.ends_with("Attr")) continue;
            if (one_of(ck, {"IntegerLiteral", "FloatingLiteral", "CharacterLiteral", "StringLiteral", "CXXBoolLiteralExpr",
                            "ImplicitCastExpr", "DeclRefExpr", "CallExpr", "CXXConstructExpr", "InitListExpr",
                            "UnaryOperator", "BinaryOperator", "ConditionalOperator", "ParenExpr"}))
                return false;
        }
        return true;
    }

    void masked_switch(const json& sw, const json* parent, std::size_t idx) {
        std::size_t ci = (flag(sw, "hasInit") ? 1 : 0) + (flag(sw, "hasVar") ? 1 : 0);
        const auto& cond = strip_all(child(sw, ci));
        if (kind(cond) != "BinaryOperator" || str_field(cond, "opcode") != "&") return;
        const auto& body = child(sw, ci + 1);
        if (kind(body) != "CompoundStmt") return;
        int case_count = 0;
        bool has_default = false;
        for (auto& c : inner(body)) count_switch_cases(c, case_count, has_default);
        if (has_default) return;
        const json* mask = nullptr;
        for (std::size_t side = 0; side < 2; ++side) {
            const auto& s = strip_all(child(cond, side));
            if (kind(s) == "BinaryOperator" && str_field(s, "opcode") == "|") {
                mask = &s;
                break;
            }
            if (kind(s) == "ParenExpr" && n_children(s)) {
                const auto& in = strip_all(child(s, 0));
                if (kind(in) == "BinaryOperator" && str_field(in, "opcode") == "|") {
                    mask = &s;
                    break;
                }
            }
            if (eval_mask_value(s)) {
                mask = &s;
                break;
            }
        }
        if (!mask) return;
        int states = 0;
        if (auto mv = eval_mask_value(*mask)) {
            const int nbits = static_cast<int>(std::popcount(*mv));
            if (nbits > 4) return;
            states = 1 << nbits;
        } else {
            const int nbits = std::max(1, count_mask_idents(*mask));
            if (nbits > 4) return;
            states = 1 << nbits;
        }
        if (case_count >= states) return;
        std::unordered_set<std::string> assigned;
        for (auto& c : inner(body)) collect_switch_assigns(c, assigned);
        if (assigned.empty()) return;
        std::unordered_set<std::string> declared;
        if (parent && kind(*parent) == "CompoundStmt") {
            for (std::size_t j = 0; j < idx; ++j) {
                const auto& s = child(*parent, j);
                if (kind(s) == "DeclStmt") {
                    for (auto& d : inner(s))
                        if (var_decl_uninit(d)) declared.insert(str_field(d, "id"));
                } else if (kind(s) == "VarDecl" && var_decl_uninit(s)) {
                    declared.insert(str_field(s, "id"));
                }
            }
        }
        std::string used;
        for (auto& id : assigned) {
            if (!declared.contains(id)) continue;
            for (std::size_t j = idx + 1; parent && kind(*parent) == "CompoundStmt" && j < n_children(*parent); ++j) {
                if (ref_after(child(*parent, j), id)) {
                    auto it = fn.locals.find(id);
                    used = it != fn.locals.end() ? str_field(*it->second, "name") : id;
                    break;
                }
            }
            if (!used.empty()) break;
        }
        if (used.empty()) return;
        add(sw, "UNINIT-SWITCH",
            "masked switch has " + std::to_string(case_count) + " arms for " + std::to_string(states) +
                " states; " + used + " may escape uninitialised");
    }

    static bool same_expr(const json& a0, const json& b0) {
        const auto& a = strip_all(a0);
        const auto& b = strip_all(b0);
        const auto& k = kind(a);
        if (k.empty() || k != kind(b)) return false;
        if (k == "DeclRefExpr") {
            const auto& ia = str_field(ref_decl(a), "id");
            return !ia.empty() && ia == str_field(ref_decl(b), "id");
        }
        if (k == "MemberExpr")
            return str_field(a, "name") == str_field(b, "name") && flag(a, "isArrow") == flag(b, "isArrow") &&
                   same_expr(child(a, 0), child(b, 0));
        if (k == "IntegerLiteral") return str_field(a, "value") == str_field(b, "value");
        if (k == "UnaryOperator")
            return str_field(a, "opcode") == "*" && str_field(b, "opcode") == "*" && same_expr(child(a, 0), child(b, 0));
        if (k == "ArraySubscriptExpr") return same_expr(child(a, 0), child(b, 0)) && same_expr(child(a, 1), child(b, 1));
        if (k == "CXXThisExpr") return true;
        return false;
    }

    void self_assign(const json& n) {
        if (str_field(n, "opcode") != "=" || n_children(n) != 2) return;
        const auto& lhs = child(n, 0);
        for (const json* x : {&n, &lhs, &strip_all(lhs)}) {
            auto t = x->find("type");
            if (t == x->end()) return;
            if (str_field(*t, "qualType").find("volatile") != std::string::npos) return;  // device access
        }
        if (!same_expr(lhs, child(n, 1))) return;
        auto name = decl_name(lhs);
        add(n, "CTRL-SELF-ASSIGN",
            (name.empty() ? std::string("value") : name) +
                " is assigned to itself; the statement has no effect (another operand was probably meant)");
    }

    // ---- 2. calls

    struct SizeUse {
        std::vector<std::size_t> bufs;
        std::size_t size;
    };

    const json* sizeof_operand(const json& n) const {
        const auto& sz = strip_all(n);
        if (kind(sz) != "UnaryExprOrTypeTraitExpr" || str_field(sz, "name") != "sizeof" || n_children(sz) == 0)
            return nullptr;
        return &strip_all(child(sz, 0));
    }

    // Function-level: `sizeof(a)` of an array-declared parameter.
    void sizeof_array_param(const json& n) {
        if (kind(n) != "UnaryExprOrTypeTraitExpr" || str_field(n, "name") != "sizeof" || n_children(n) == 0) return;
        const auto& e = strip_all(child(n, 0));
        if (kind(e) != "DeclRefExpr") return;
        const auto& id = str_field(ref_decl(e), "id");
        if (!fn.array_params.contains(id)) return;
        auto pname = str_field(ref_decl(e), "name");
        add(n, "MEM-SIZEOF-PTR",
            "sizeof(" + pname + ") is the size of a pointer: the array parameter " + pname +
                " decays to a pointer (pass the length)");
    }

    void call(const json& c, const std::vector<const json*>& parents) {
        auto name = callee_name(c);
        if (name.empty()) return;
        auto id = callee_id(c);
        const auto* facts = cx.facts(id);
        const bool user = facts && facts->user;
        if (one_of(name, {"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp", "longjmp", "vfork"})) fn.opaque = true;
        auto args = call_args(c);
        if (user && !libc_api_lint(name)) return;  // a project definition shadowing a non-libc name
        if (name == "memset" && args.size() >= 3 && !in_macro(c) && is_zero_literal(*args[2]) &&
            !begin_of(strip_all(*args[2])).macro && !is_zero_literal(*args[1])) {
            const auto& fill = strip_all(*args[1]);
            bool swapped = sizeof_operand(fill) != nullptr;
            if (!swapped && kind(fill) == "DeclRefExpr")
                swapped = true;
            else if (!swapped) {
                auto txt = strip_ws(std::string(cx.text_of(fill)));
                if (is_ident(txt)) {
                    if (auto v = int_value(fill); !v || (*v != 0 && *v != 1)) swapped = true;
                }
            }
            if (swapped)
                add(c, "MEM-MEMSET-SWAP",
                    "memset() length is 0 and the fill byte is not: size and fill-byte arguments are swapped");
        }
        static const std::map<std::string, SizeUse, std::less<>> kSized{
            {"memset", {{0}, 2}},    {"memcpy", {{0, 1}, 2}}, {"memmove", {{0, 1}, 2}},
            {"memcmp", {{0, 1}, 2}}, {"bzero", {{0}, 1}},     {"explicit_bzero", {{0}, 1}},
        };
        if (auto it = kSized.find(name); it != kSized.end() && args.size() > it->second.size) {
            if (const auto* e = sizeof_operand(*args[it->second.size]); e && kind(*e) == "DeclRefExpr") {
                const auto& vid = str_field(ref_decl(*e), "id");
                bool is_buf = false;
                for (auto b : it->second.bufs) {
                    const auto& a = strip_all(*args[b]);
                    if (kind(a) == "DeclRefExpr" && str_field(ref_decl(a), "id") == vid) is_buf = true;
                }
                if (is_buf && fn.ptr_params.contains(vid)) {
                    auto pname = str_field(ref_decl(*e), "name");
                    add(c, "MEM-SIZEOF-PTR",
                        name + "() size is sizeof(" + pname + "), the size of a pointer: " + pname +
                            " is a pointer parameter, not the caller's buffer (pass the length)");
                }
            }
        }
        // p = malloc(sizeof(p)) / T *p = malloc(sizeof p): the pointer, not the pointee.
        if (one_of(name, {"malloc", "calloc", "realloc", "alloca", "kmalloc"}) && !args.empty()) {
            const json* target = nullptr;
            if (!parents.empty()) {
                // Walk up through implicit casts / explicit casts / parens.
                for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
                    const auto& pk = kind(**it);
                    if (is_implicit_wrapper(pk) || pk == "ParenExpr" || is_explicit_cast(pk)) continue;
                    if (pk == "BinaryOperator" && str_field(**it, "opcode") == "=") target = &child(**it, 0);
                    else if (pk == "VarDecl") target = *it;
                    break;
                }
            }
            if (target) {
                std::string tid = kind(*target) == "VarDecl" ? str_field(*target, "id") : ref_id(*target);
                for (auto* a : args) {
                    if (const auto* e = sizeof_operand(*a); e && kind(*e) == "DeclRefExpr" && !tid.empty() &&
                                                           str_field(ref_decl(*e), "id") == tid &&
                                                           is_pointer_type(type_of(*e))) {
                        auto vn = str_field(ref_decl(*e), "name");
                        add(c, "MEM-SIZEOF-PTR",
                            name + "() size is sizeof(" + vn + "), the size of the pointer " + vn +
                                ", not of what it points to (sizeof *" + vn + " was probably meant)");
                        break;
                    }
                }
            }
        }
        // Same buffer as source and destination.
        if (one_of(name, {"memcpy", "strcpy", "strncpy", "strcat", "strncat", "wmemcpy", "wcscpy"}) &&
            args.size() >= 2 && !in_macro(c) && same_expr(*args[0], *args[1])) {
            auto bn = decl_name(*args[0]);
            add(c, "MEM-OVERLAP",
                name + "() with the same object " + (bn.empty() ? std::string("") : "(" + bn + ") ") +
                    "as source and destination: overlapping copy is undefined");
        }
        if (name == "bcopy" && args.size() >= 2 && !in_macro(c) && same_expr(*args[0], *args[1])) {
            add(c, "MEM-BCOPY", "bcopy() with the same source and destination: the call does nothing useful");
        }
        if (name == "gets") add(c, "API-GETS", "gets() cannot bound its write: any input line can overflow the buffer (use fgets)");
        if (name == "strtok")
            add(c, "API-STRTOK-REENTRANT",
                "strtok() is not reentrant; strtok_r/strtok_s keep no static cursor");
        if (one_of(name, {"tmpnam", "tempnam", "tmpnam_r"}))
            add(c, "API-TMPNAM", "tmpnam/tempnam is predictable; use mkstemp/mkdtemp");
        if (name == "mktemp")
            add(c, "API-MKTEMP", "mktemp() is insecure; use mkstemp/mkdtemp");
        if (name == "umask" && !args.empty()) {
            if (auto v = int_value(*args[0]); v && *v == 0)
                add(c, "API-UMASK", "umask(0) leaves world-writable default");
        }
        if (one_of(name, {"mkstemp", "mkstemps"}) && !args.empty() && string_lit(*args[0]))
            add(c, "API-MKSTEMP", name + "() template is a string literal (not writable)");
        if (one_of(name, {"chmod", "fchmod"}) && args.size() >= 2) {
            constexpr int kWorld = 0777;
            if (auto v = int_value(*args[1]); v && *v == kWorld)
                add(c, "API-CHMOD-WORLD", name + "() mode is world-writable 0777");
        }
        if (name == "system" && !args.empty() && !string_lit(*args[0]) && !in_macro(c))
            add(c, "API-SYSTEM", "system() command is not a string literal");
        if (name == "signal" && args.size() >= 2) {
            const auto& h = strip_all(*args[1]);
            bool ok = sig_dfl_or_ign(h) || has_literal_zero_or_one(h);
            if (!ok) {
                auto t = strip_ws(std::string(cx.text_of(h)));
                ok = t == "SIG_DFL" || t == "SIG_IGN" || t.find("SIG_DFL") != std::string::npos ||
                     t.find("SIG_IGN") != std::string::npos;
            }
            if (!ok) add(c, "API-SIGNAL", "signal() installs a handler; prefer sigaction()");
        }
        // Strings into local char arrays.
        if (one_of(name, {"strcpy", "strcat"}) && args.size() >= 2) {
            auto sz = local_array_size(*args[0]);
            auto lit = string_lit(*args[1]);
            if (sz && lit && static_cast<long long>(lit->size()) + 1 > *sz) {
                add(c, "STR-OFF-BY-ONE",
                    name + "() of a " + std::to_string(lit->size()) + "-character literal (+ NUL) into " +
                        decl_name(*args[0]) + "[" + std::to_string(*sz) + "]: the copy overflows the array");
            }
            if (sz && !in_macro(c)) {
                auto bn = decl_name(*args[0]);
                add(c, "STR-UNBOUNDED-COPY",
                    name + "() into local " + (bn.empty() ? std::string("buffer") : bn) + "[…] with no bound");
            }
        }
        if (name == "strncpy" && args.size() >= 3) {
            auto sz = local_array_size(*args[0]);
            std::optional<long long> n = int_value(*args[2]);
            if (!n) {
                if (const auto* e = sizeof_operand(*args[2]); e && same_expr(*e, *args[0])) n = sz;
            }
            if (sz && n && *n >= *sz && !nul_store_after(*args[0], c)) {
                auto bn = decl_name(*args[0]);
                add(c, "STR-STRNCPY-NUL",
                    "strncpy() fills all " + std::to_string(*sz) + " bytes of " + bn +
                        " and no NUL is stored after it: a long source leaves " + bn + " unterminated");
            }
        }
        if (name == "memcpy" && args.size() >= 3) {
            auto sz = local_array_size(*args[0]);
            auto lit = string_lit(*args[1]);
            auto n = int_value(*args[2]);
            if (sz && lit && n && *n == static_cast<long long>(lit->size()) && *n < *sz &&
                !nul_store_after(*args[0], c)) {
                add(c, "STR-MISSING-NUL",
                    "memcpy() copies the " + std::to_string(*n) + " characters of \"" + *lit +
                        "\" without its terminating NUL into " + decl_name(*args[0]));
            }
        }
        if (mem_copy_callee(name) && args.size() >= 3 && !in_macro(c)) mem_copy_len(c, name, args);
        if (one_of(name, {"malloc", "realloc", "calloc", "reallocarray"}) && !in_macro(c)) {
            std::vector<const json*> size_args;
            if (name == "malloc") {
                if (!args.empty()) size_args = {args[0]};
            } else if (name == "realloc" && args.size() >= 2) {
                size_args = {args[1]};
            } else {
                for (auto* a : args)
                    if (has_wrap_mul_text(cx.text_of(*a))) size_args.push_back(a);
            }
            if (name == "calloc" || name == "reallocarray") {
                bool any = false;
                for (auto* a : args)
                    if (has_wrap_mul_text(cx.text_of(*a))) any = true;
                if (!any) size_args.clear();
            }
            for (auto* a : size_args) {
                if (!has_wrap_mul_text(cx.text_of(*a))) continue;
                add(c, "INT-WRAP-ALLOC", name + "() size uses n*sizeof; may wrap before cast");
                break;
            }
        }
        // glibc's <alloca.h> defines alloca(size) as __builtin_alloca(size): that expansion is the call.
        if (one_of(name, {"alloca", "__builtin_alloca"}) && !args.empty() &&
            (!in_macro(c) || (n_children(c) && str_field(ref_decl(strip_all(child(c, 0))), "name") == "__builtin_alloca"))) {
            const auto& sz = strip_all(*args[0]);
            if (!int_value(sz) && !constant_expr(sz)) {
                auto szt = arg_text(*args[0]);
                add(c, "MEM-ALLOCA", name + "(" + szt + ") size is not a constant", /*allow_macro=*/true);
            }
        }
        if (one_of(name, {"sprintf", "vsprintf"}) && !args.empty()) {
            if (local_array_size(*args[0]) && !in_macro(c)) {
                auto bn = decl_name(*args[0]);
                add(c, "STR-SPRINTF",
                    name + "() into local " + (bn.empty() ? std::string("buffer") : bn) + "[…] with no bound");
            }
        }
        if (name == "snprintf" && args.size() >= 2 && !cx.unit.cxx) {
            auto sz = local_array_size(*args[0]);
            if (sz && !in_macro(c) && !sizeof_matches_local_array(*args[1], *args[0])) {
                if (auto n = int_value(*args[1]); n && *n > *sz) {
                    auto bn = decl_name(*args[0]);
                    add(c, "STR-SNPRINTF",
                        "snprintf(" + bn + ", " + std::to_string(*n) + ", …) exceeds local " + bn + "[" +
                            std::to_string(*sz) + "]");
                }
            }
        }
        if (one_of(name, {"wcscpy", "wcscat"}) && !args.empty() && !cx.unit.cxx) {
            if (local_wchar_array_size(*args[0]) && !in_macro(c)) {
                auto bn = decl_name(*args[0]);
                add(c, "STR-WCSCPY",
                    name + "() into local " + (bn.empty() ? std::string("buffer") : bn) + "[…] with no bound");
            }
        }
        if (one_of(name, {"srand", "srandom"}) && !args.empty() && mentions_time_call(*args[0]))
            add(c, "CRYPTO-SRAND", "srand(time()) is not a CSPRNG seed");
        str_null_arg(c, name, args);
        str_null_member(c, name, args);
        format_call(c, name, args);
    }

    void str_null_arg(const json& c, const std::string& callee, const std::vector<const json*>& args) {
        if (fn.ptr_params.empty() || fn.str_null_arg_reported || in_macro(c)) return;
        auto fit = kStrNullFn.find(callee);
        if (fit == kStrNullFn.end()) return;
        auto slice = body_slice();
        if (slice.text.empty()) return;
        std::unordered_set<std::string> unchecked;
        for (auto& id : fn.ptr_params) {
            auto lit = fn.locals.find(id);
            if (lit == fn.locals.end()) continue;
            auto pname = str_field(*lit->second, "name");
            if (pname.empty() || param_null_tested(pname, slice.text)) continue;
            unchecked.insert(pname);
        }
        if (unchecked.empty()) return;
        for (int idx : fit->second) {
            if (idx >= static_cast<int>(args.size())) continue;
            const auto& arg = strip_all(*args[static_cast<std::size_t>(idx)]);
            if (kind(arg) != "DeclRefExpr") continue;
            auto pname = str_field(ref_decl(arg), "name");
            if (!unchecked.contains(pname)) continue;
            fn.str_null_arg_reported = true;
            add(c, "STR-NULL-ARG", callee + "() called with unchecked pointer parameter " + pname);
            return;
        }
    }

    void str_null_member(const json& c, const std::string& callee, const std::vector<const json*>& args) {
        if (fn.ptr_params.empty() || fn.str_null_member_reported || in_macro(c)) return;
        auto fit = kStrNullFn.find(callee);
        if (fit == kStrNullFn.end()) return;
        auto slice = body_slice();
        if (slice.text.empty()) return;
        Pos use = begin_of(c);
        if (use.offset < slice.start) return;
        auto use_off = static_cast<std::size_t>(use.offset - slice.start);
        for (int idx : fit->second) {
            if (idx >= static_cast<int>(args.size())) continue;
            auto parsed = ptr_member_arg(*args[static_cast<std::size_t>(idx)]);
            if (!parsed) continue;
            if (member_test_pos(parsed->first, parsed->second, slice.text) < use_off) continue;
            fn.str_null_member_reported = true;
            add(c, "STR-NULL-MEMBER",
                callee + "() called with unchecked member " + parsed->first + "->" + parsed->second);
            return;
        }
    }

    void ptr_chain(const json& me) {
        if (!flag(me, "isArrow") || in_macro(me) || n_children(me) == 0) return;
        const auto& mid = strip_all(child(me, 0));
        if (kind(mid) != "MemberExpr" || !flag(mid, "isArrow") || n_children(mid) == 0) return;
        const auto& base = strip_all(child(mid, 0));
        if (kind(base) != "DeclRefExpr") return;
        auto param = str_field(ref_decl(base), "name");
        auto member = str_field(mid, "name");
        if (param.empty() || member.empty()) return;
        auto key = std::pair{param, member};
        if (fn.ptr_chain_done.contains(key)) return;
        fn.ptr_chain_done.insert(key);
        auto slice = body_slice();
        if (slice.text.empty()) return;
        Pos use = begin_of(base);
        if (use.offset < slice.start) return;
        auto use_off = static_cast<std::size_t>(use.offset - slice.start);
        if (member_test_pos(param, member, slice.text) < use_off) return;
        add(mid, "PTR-CHAIN-NULL", "chained dereference " + param + "->" + member +
                                      "->... without a null check on " + param + "->" + member);
    }

    std::string arg_text(const json& arg) const {
        const auto& s = strip_all(arg);
        if (kind(s) == "DeclRefExpr") return str_field(ref_decl(s), "name");
        auto t = strip_ws(std::string(cx.text_of(s)));
        if (!t.empty()) return t;
        return decl_name(s);
    }

    void mem_copy_len(const json& c, const std::string& callee, const std::vector<const json*>& args) {
        const auto* body = function_body(fn.node);
        if (!body) return;
        auto call_pos = begin_of(c);
        auto body_pos = begin_of(*body);
        if (body_pos.offset < 0 && n_children(*body)) body_pos = begin_of(child(*body, 0));
        const std::string* canon = call_pos.file ? call_pos.file : body_pos.file;
        if (!canon) return;
        auto* fc = cx.file(canon);
        if (!fc) return;
        const int call_line = cx.line_of(call_pos);
        const int body_line = cx.line_of(body_pos);
        std::string prior;
        if (call_line > 0 && body_line > 0 && call_line > body_line) {
            for (int ln = body_line; ln < call_line && ln <= static_cast<int>(fc->lines.size()); ++ln) {
                if (!prior.empty()) prior.push_back('\n');
                prior += fc->lines.at(static_cast<std::size_t>(ln - 1));
            }
        }
        std::unordered_set<std::string> max_names;
        for (auto& ln : fc->lines) collect_max_fields(ln, max_names);
        collect_max_fields(prior, max_names);
        if (max_names.empty()) return;
        const bool plain = callee == "memcpy" || callee == "memmove";
        static const std::regex max_minus(
            "\\b(\\w+_max)\\s*-\\s*([A-Za-z_]\\w*)\\b(?!\\s*(?:->|\\.|\\[|\\())");
        std::optional<std::string> msg;
        for (std::size_t a : {std::size_t{2}, std::size_t{0}}) {
            if (a >= args.size()) continue;
            auto size = arg_text(*args.at(a));
            if (a == 2 && is_ident(size)) {
                if (plain && !copy_len_guarded(size, prior, max_names))
                    msg = callee + "() length " + size + " not checked against a *_max bound in this function";
                break;
            }
            std::smatch mm;
            if (!std::regex_search(size, mm, max_minus)) continue;
            auto mx = mm.str(1), v = mm.str(2);
            if (below_max_tested(v, mx, prior)) continue;
            msg = callee + "() " + (a == 2 ? "length" : "destination") + " uses " + mx + " - " + v + " with no " +
                  v + " < " + mx + " test before it (it can wrap)";
            break;
        }
        if (!msg) return;
        add(c, "MEM-COPY-LEN", *msg);
    }

    // A `buf[k] = 0` / `buf[k] = '\0'` store after `at` in this function.
    bool nul_store_after(const json& buf, const json& at) const {
        auto from = begin_of(at).offset;
        bool found = false;
        std::function<void(const json&)> rec = [&](const json& n) {
            if (found || !n.is_object()) return;
            if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "=" && begin_of(n).offset > from) {
                const auto& l = strip_all(child(n, 0));
                if (kind(l) == "ArraySubscriptExpr" && same_expr(child(l, 0), buf) && is_zero_literal(child(n, 1)))
                    found = true;
            }
            // strncpy(dst, src, sizeof dst - 1) style helpers that terminate: snprintf / strlcpy after it.
            for (auto& c : inner(n)) {
                if (rec) rec(c);
            }
        };
        if (fn.node && rec) rec(*fn.node);
        return found;
    }

    // ---- 3. printf family

    struct FmtFn {
        int fmt;
        bool va;  // takes a va_list
        bool scan;
    };

    void format_call(const json& c, const std::string& name, const std::vector<const json*>& args) {
        static const std::map<std::string, FmtFn, std::less<>> kFmt{
            {"printf", {0, false, false}},   {"fprintf", {1, false, false}},  {"dprintf", {1, false, false}},
            {"sprintf", {1, false, false}},  {"snprintf", {2, false, false}}, {"asprintf", {1, false, false}},
            {"syslog", {1, false, false}},   {"vprintf", {0, true, false}},   {"vfprintf", {1, true, false}},
            {"vsprintf", {1, true, false}},  {"vsnprintf", {2, true, false}}, {"vasprintf", {1, true, false}},
            {"vdprintf", {1, true, false}},  {"vsyslog", {1, true, false}},   {"err", {1, false, false}},
            {"errx", {1, false, false}},     {"warn", {0, false, false}},     {"warnx", {0, false, false}},
            {"scanf", {0, false, true}},     {"fscanf", {1, false, true}},    {"sscanf", {1, false, true}},
        };
        auto it = kFmt.find(name);
        if (it == kFmt.end() || static_cast<std::size_t>(it->second.fmt) >= args.size()) return;
        const auto& f = it->second;
        const json& farg = *args.at(static_cast<std::size_t>(f.fmt));
        auto lit = string_lit(farg);
        const auto& core = strip_casts(farg);
        const std::size_t given = args.size() - static_cast<std::size_t>(f.fmt) - 1;
        if (!lit) {
            // -Wformat-security: a non-literal format and nothing to format.
            if (f.va || f.scan || given != 0) return;
            if (is_null_ptr_expr(farg) || kind(core) == "PredefinedExpr" || kind(core) == "ConditionalOperator")
                return;
            if (is_call_kind(kind(core)) &&
                one_of(callee_name(core), {"gettext", "dgettext", "ngettext", "dcgettext", "_", "N_"}))
                return;
            if (in_macro(c)) return;
            auto fnm = decl_name(farg);
            add(c, "FMT-STRING",
                name + "() format " + (fnm.empty() ? std::string("") : "(" + fnm + ") ") +
                    "is not a string literal and there are no arguments: a '%' in it reads the stack "
                    "(use " + name + "(\"%s\", ...))");
            return;
        }
        // Parse the conversions.
        struct Conv {
            char c;
            std::string len;
        };
        std::vector<Conv> convs;
        int stars = 0;
        bool bad = false, percent_n = false;
        const std::string& s = *lit;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '%') continue;
            if (++i >= s.size()) {
                bad = true;
                break;
            }
            if (s[i] == '%') continue;
            // positional arguments (%1$d): not modelled
            std::size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            if (j < s.size() && s[j] == '$') {
                bad = true;
                break;
            }
            if (f.scan && i < s.size() && s[i] == '*') {  // assignment suppression
                ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                while (i < s.size() && std::strchr("hlLqjzt", s[i])) ++i;
                if (i < s.size() && s[i] == '[') {
                    while (i < s.size() && s[i] != ']') ++i;
                }
                continue;
            }
            while (i < s.size() && std::strchr("-+ #0'", s[i])) ++i;
            if (i < s.size() && s[i] == '*') {
                ++stars;
                convs.push_back({'*', ""});
                ++i;
            } else {
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            if (i < s.size() && s[i] == '.') {
                ++i;
                if (i < s.size() && s[i] == '*') {
                    ++stars;
                    convs.push_back({'*', ""});
                    ++i;
                } else {
                    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                }
            }
            std::string len;
            while (i < s.size() && std::strchr("hlLqjzt", s[i])) len += s[i++];
            if (i >= s.size()) {
                bad = true;
                break;
            }
            char cv = s[i];
            if (f.scan && cv == '[') {
                while (i < s.size() && s[i] != ']') ++i;
                convs.push_back({'[', len});
                continue;
            }
            if (!std::strchr("diouxXeEfFgGaAcspnCSm", cv)) {
                bad = true;
                break;
            }
            if (cv == 'm') continue;  // glibc %m takes no argument
            if (cv == 'n') percent_n = true;
            convs.push_back({cv, len});
        }
        (void)stars;
        if (percent_n && !f.scan)
            add(c, "FMT-PERCENT-N", name + "() format uses %n: the call writes through an argument pointer");
        if (bad || f.va) return;
        if (convs.size() != given) {
            add(c, "FMT-ARGS",
                name + "() format \"" + s.substr(0, 40) + (s.size() > 40 ? "..." : "") + "\" takes " +
                    std::to_string(convs.size()) + " argument(s) but the call passes " + std::to_string(given));
            return;
        }
        if (f.scan) return;
        for (std::size_t i = 0; i < convs.size(); ++i) {
            const std::size_t arg_i = static_cast<std::size_t>(f.fmt) + 1 + i;
            if (arg_i >= args.size()) continue;
            const json& a = *args.at(arg_i);
            auto t = type_of(strip_implicit(a));
            if (t.empty() || dep(a)) continue;
            const char cv = convs.at(i).c;
            const bool ptr = is_pointer_type(t) || t.find('[') != std::string::npos;
            const bool integ = is_int_type(t) || t == "bool" || t == "_Bool" || t.starts_with("enum ");
            const bool flt = is_floating(t);
            std::string want;
            if (std::strchr("diouxXc*", cv) && (flt || ptr)) want = "an integer";
            else if (std::strchr("eEfFgGaA", cv) && (integ || ptr)) want = "a floating-point value";
            else if (cv == 's' && (integ || flt)) want = "a string pointer";
            else if (cv == 'p' && (flt)) want = "a pointer";
            // Width (LP64): %d/%x take an int (narrower arguments are
            // promoted), %ld/%lld/%zd/%jd a 64-bit integer.
            if (want.empty() && integ && std::strchr("diouxX", cv)) {
                const int w = int_width(t);
                const auto& len = convs.at(i).len;
                const bool wide = one_of(len, {"l", "ll", "q", "j", "z", "t"});
                if (w == 64 && !wide && len != "L") want = "an int (use %l" + std::string(1, cv) + ")";
                else if (w > 0 && w <= 32 && wide) want = "a 64-bit integer";
            }
            if (want.empty()) continue;
            add(a, "FMT-ARGS",
                name + "() conversion %" + convs.at(i).len + std::string(1, cv) + " expects " + want +
                    " but argument " +
                    std::to_string(i + 1) + " has type " + t);
            return;
        }
    }

    // ---- 4. operators

    void binop(const json& n) {
        const auto& op = str_field(n, "opcode");
        if (n_children(n) != 2) return;
        const auto& l = child(n, 0);
        const auto& r = child(n, 1);
        // Unsigned compared below zero.
        if (one_of(op, {"<", ">=", ">", "<="}) && !in_macro(n)) {
            const json* var = nullptr;
            if ((op == "<" || op == ">=") && is_zero_literal(r) && !begin_of(strip_all(r)).macro) var = &l;
            if ((op == ">" || op == "<=") && is_zero_literal(l) && !begin_of(strip_all(l)).macro) var = &r;
            if (var) {
                const auto& core = strip_all(*var);
                auto t = type_of(core);
                if (is_unsigned_int(t) && !int_value(core) && !in_macro(core)) {
                    auto vn = decl_name(*var);
                    const bool never = op == "<" || op == ">";
                    add(n, "CTRL-DEAD-GUARD",
                        (vn.empty() ? std::string("unsigned value") : vn + " (" + t + ")") + " compared " +
                            (never ? "below zero: the test is never true" : "against zero: the test is always true"));
                }
            }
        }
        // Shifts.
        if (one_of(op, {"<<", ">>"})) shift(n, op, l, r, type_of(n));
        // Bitwise operator on two comparisons.
        if (one_of(op, {"&", "|"}) && boolish(l) && boolish(r) && !in_macro(n)) {
            add(n, "INT-BOOL-AS-BIT",
                std::string("bitwise ") + op + " on two comparisons: both sides are always evaluated (" +
                    (op == "&" ? "&&" : "||") + " was probably meant)");
        }
        // || of two bounds that cover every value.
        if (op == "||") tautology(n, l, r);
        // Division by a literal zero.
        if (one_of(op, {"/", "%"})) div_zero(n, op, r);
    }

    void compound(const json& n) {
        const auto& op = str_field(n, "opcode");
        if (n_children(n) != 2) return;
        if (one_of(op, {"<<=", ">>="})) {
            auto t = type_of(child(n, 0));
            if (int_width(t) && int_width(t) < 32) t = "int";
            shift(n, op.substr(0, 2), child(n, 0), child(n, 1), t);
        }
        if (one_of(op, {"/=", "%="})) div_zero(n, op.substr(0, 1), child(n, 1));
    }

    void shift(const json& n, const std::string& op, const json& l, const json& r, const std::string& t) {
        auto cnt = int_value(r);
        int w = int_width(t);
        if (!cnt || !w || in_macro(n)) return;
        if (*cnt < 0 || *cnt >= w) {
            add(n, "INT-SHIFT-UB",
                "shift " + op + " by " + std::to_string(*cnt) + " on a " + std::to_string(w) + "-bit " + t +
                    ": a negative count or one >= the width is undefined");
            return;
        }
        if (op != "<<" || cx.unit.cxx) return;  // C++20 defines signed left shifts
        auto lv = int_value(l);
        if (!lv || !is_signed_int(t) || *lv < 0 || *cnt >= 63) return;
        // lv << cnt must fit in the signed type's value bits.
        long double v = static_cast<long double>(*lv) * static_cast<long double>(1ULL << *cnt);
        long double max = static_cast<long double>((1ULL << (w - 1)) - 1);
        if (v > max) {
            add(n, "INT-SHIFT-UB",
                std::to_string(*lv) + " << " + std::to_string(*cnt) + " overflows " + t +
                    ": shifting into or past the sign bit is undefined in C");
        }
    }

    static bool boolish(const json& x0) {
        const auto& x = strip_all(x0);
        if (kind(x) == "BinaryOperator")
            return one_of(str_field(x, "opcode"), {"==", "!=", "<", ">", "<=", ">=", "&&", "||"});
        if (kind(x) == "UnaryOperator") return str_field(x, "opcode") == "!";
        return false;
    }

    struct Bound {
        const json* var = nullptr;
        bool lower = false;  // var >= v
        long long v = 0;
    };

    std::optional<Bound> bound(const json& x0) {
        const auto& x = strip_all(x0);
        if (kind(x) != "BinaryOperator") return std::nullopt;
        auto op = str_field(x, "opcode");
        const json* var = &child(x, 0);
        auto c = int_value(child(x, 1));
        if (!c) {
            c = int_value(child(x, 0));
            var = &child(x, 1);
            if (!c) return std::nullopt;
            // flip: c < v  ==  v > c
            if (op == "<") op = ">";
            else if (op == ">") op = "<";
            else if (op == "<=") op = ">=";
            else if (op == ">=") op = "<=";
        }
        const auto& vc = strip_all(*var);
        if (kind(vc) != "DeclRefExpr" && kind(vc) != "MemberExpr") return std::nullopt;
        if (op == ">=") return Bound{var, true, *c};
        if (op == ">") return Bound{var, true, *c + 1};
        if (op == "<") return Bound{var, false, *c - 1};
        if (op == "<=") return Bound{var, false, *c};
        return std::nullopt;
    }

    void tautology(const json& n, const json& l, const json& r) {
        auto a = bound(l), b = bound(r);
        if (!a || !b || a->lower == b->lower || !same_expr(*a->var, *b->var)) return;
        const auto& lo = a->lower ? *a : *b;
        const auto& hi = a->lower ? *b : *a;
        if (lo.v > hi.v + 1) return;
        auto vn = decl_name(*a->var);
        add(n, "INT-TAUTOLOGY",
            "(" + vn + " >= " + std::to_string(lo.v) + ") || (" + vn + " <= " + std::to_string(hi.v) +
                ") holds for every value: the bound check never fails (&& was probably meant)");
    }

    void div_zero(const json& n, const std::string& op, const json& r) {
        auto t = type_of(n);
        if (is_int_type(t) && is_zero_literal(r)) {
            add(n, "INT-DIV-ZERO", std::string(op == "%" ? "remainder" : "division") + " by the constant 0 is undefined", true);
        } else if (is_floating(t) && op == "/") {
            if (auto v = float_value(r); v && *v == 0.0)
                add(n, "FLOAT-UB", "division by the floating literal 0: the result is infinity or NaN (undefined outside IEEE 754 / Annex F)");
        }
    }

    // p = realloc(p, n): on failure the old block leaks.
    void realloc_self(const json& n) {
        if (str_field(n, "opcode") != "=") return;
        const auto& rhs = strip_casts(child(n, 1));
        if (!is_call_kind(kind(rhs)) || !one_of(callee_name(rhs), {"realloc", "reallocarray"})) return;
        auto args = call_args(rhs);
        if (args.empty() || !same_expr(child(n, 0), *args[0]) || in_macro(n)) return;
        auto pn = decl_name(child(n, 0));
        add(n, "MEM-REALLOC-SELF",
            pn + " = realloc(" + pn + ", ...): when realloc fails it returns NULL and the old block leaks "
                 "(assign to a temporary first)");
    }

    // x = std::move(x)
    void self_move(const json& n) {
        const json* lhs = nullptr;
        const json* rhs = nullptr;
        if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "=") {
            lhs = &child(n, 0);
            rhs = &child(n, 1);
        } else if (kind(n) == "CXXOperatorCallExpr" && n_children(n) == 3 && decl_name(child(n, 0)) == "operator=") {
            lhs = &child(n, 1);
            rhs = &child(n, 2);
        }
        if (!lhs) return;
        const auto& m = strip_all(*rhs);
        if (!is_call_kind(kind(m)) || callee_name(m) != "move" || n_children(m) != 2) return;
        if (!same_expr(*lhs, child(m, 1))) return;
        auto vn = decl_name(*lhs);
        add(n, "CXX-SELF-MOVE",
            vn + " = std::move(" + vn + "): an object move-assigned to itself is left in an unspecified state");
    }

    void move_const(const json& c) {
        if (callee_name(c) != "move" || n_children(c) != 2) return;
        const auto& a = strip_implicit(child(c, 1));
        const auto& qt = qual_type(a);
        if (!qt.starts_with("const ") || dep(a)) return;
        auto vn = decl_name(a);
        add(c, "CXX-MOVE-CONST",
            "std::move(" + vn + ") of a const object: the result binds to the copy, not the move, constructor "
                                "(the move does nothing)");
    }

    // ---- 5. C++ objects

    bool polymorphic_no_vdtor(const std::string& name, int depth, bool& poly, bool& vdtor) {
        auto it = cx.unit.records.find(name);
        if (it == cx.unit.records.end() || depth > 16) return false;
        poly = poly || it->second.polymorphic;
        vdtor = vdtor || it->second.virtual_dtor;
        for (auto& b : it->second.bases) {
            bool bp = false, bv = false;
            polymorphic_no_vdtor(b, depth + 1, bp, bv);
            poly = poly || bp;
            vdtor = vdtor || bv;
        }
        return true;
    }

    void check_cxx_delete(const json& d) {
        if (n_children(d) == 0) return;
        const auto& op = strip_all(child(d, 0));
        if (kind(op) == "CXXThisExpr") {
            add(d, "CXX-DELETE-THIS", "delete this: the object is destroyed while its member function still runs");
            return;
        }
        auto name = record_name(type_of(op));
        if (name.empty()) {
            // Dump path: records are known by their spelled name.
            name = record_name(std::string(qual_type(op)));
        }
        bool poly = false, vdtor = false;
        if (!polymorphic_no_vdtor(name, 0, poly, vdtor)) {
            // Unqualified fallback: the last component.
            auto p = name.rfind("::");
            if (p == std::string::npos || !polymorphic_no_vdtor(name.substr(p + 2), 0, poly, vdtor)) return;
        }
        auto rit = cx.unit.records.find(name);
        if (!poly || vdtor || (rit != cx.unit.records.end() && rit->second.is_final)) return;
        add(d, "CXX-MISSING-VIRTUAL-DTOR",
            "delete through " + name + "*, a polymorphic class without a virtual destructor: deleting a derived "
                                       "object this way is undefined");
    }

    void catch_stmt(const json& c) {
        const auto& v = child(c, 0);
        if (kind(v) != "VarDecl") return;
        auto qt = qual_type(v);
        auto t = type_of(v);
        if (qt.find('&') != std::string_view::npos || is_pointer_type(t) || is_builtin_arith(t) ||
            t.starts_with("enum "))
            return;
        if (in_macro(v) || dep(v)) return;
        add(v, "CXX-CATCH-BY-VALUE",
            "exception caught by value (" + std::string(qt) +
                "): a derived exception is sliced and copied (catch by const reference)");
    }

    void lock_init_call(const json& c) {
        auto name = callee_name(c);
        if (!one_of(name, {"pthread_mutex_init", "mtx_init"})) return;
        auto args = call_args(c);
        if (args.empty()) return;
        const auto& a = strip_all(*args[0]);
        if (kind(a) == "UnaryOperator" && str_field(a, "opcode") == "&") {
            if (kind(strip_all(child(a, 0))) == "DeclRefExpr")
                fn.mutex_inited.insert(str_field(ref_decl(strip_all(child(a, 0))), "id"));
            return;
        }
        if (kind(a) == "DeclRefExpr") fn.mutex_inited.insert(str_field(ref_decl(a), "id"));
    }

    void lock_acquire_call(const json& c) {
        auto name = callee_name(c);
        if (!one_of(name, {"pthread_mutex_lock", "pthread_mutex_trylock", "mtx_lock", "mtx_trylock"})) return;
        auto args = call_args(c);
        if (args.empty() || in_macro(c)) return;
        const auto& a = strip_all(*args[0]);
        std::string id;
        if (kind(a) == "UnaryOperator" && str_field(a, "opcode") == "&") {
            const auto& o = strip_all(child(a, 0));
            if (kind(o) == "DeclRefExpr") id = str_field(ref_decl(o), "id");
        } else if (kind(a) == "DeclRefExpr") {
            id = str_field(ref_decl(a), "id");
        }
        if (id.empty() || !fn.mutex_locals.contains(id) || fn.mutex_inited.contains(id)) return;
        auto vname = str_field(ref_decl(*fn.locals.at(id)), "name");
        add(c, "LOCK-MISSING-INIT",
            vname + " is locked without pthread_mutex_init/mtx_init or a static initializer");
        fn.mutex_inited.insert(id);  // one report per local
    }

    static bool lvalue_names_ptr(const json& lhs, std::string_view pid) {
        const auto& l = strip_all(lhs);
        if (kind(l) == "ArraySubscriptExpr") {
            const auto& b = strip_all(child(l, 0));
            return kind(b) == "DeclRefExpr" && str_field(ref_decl(b), "id") == pid;
        }
        if (kind(l) == "UnaryOperator" && str_field(l, "opcode") == "*") {
            const auto& p = strip_all(child(l, 0));
            return kind(p) == "DeclRefExpr" && str_field(ref_decl(p), "id") == pid;
        }
        return false;
    }

    static bool store_local_into_ptr(const json& stmt, std::string_view pid, std::string_view local) {
        if (kind(stmt) != "BinaryOperator" || str_field(stmt, "opcode") != "=") return false;
        if (!lvalue_names_ptr(child(stmt, 0), pid)) return false;
        const auto& r = strip_all(child(stmt, 1));
        return kind(r) == "DeclRefExpr" && str_field(ref_decl(r), "name") == local;
    }

    void crypto_misuse(const json& body) {
        bool reported = false;
        std::function<void(const json&)> walk = [&](const json& n) {
            if (reported || !n.is_object()) return;
            if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "=") {
                const auto& l = strip_all(child(n, 0));
                const auto& r = strip_casts(child(n, 1));
                if (kind(l) == "ArraySubscriptExpr") {
                    const auto& b = strip_all(child(l, 0));
                    if (kind(b) == "DeclRefExpr" && str_field(ref_decl(b), "name") == "key" && is_rand_call_expr(r)) {
                        add(n, "CRYPTO-MISUSE", "rand() used to fill key[]");
                        reported = true;
                        return;
                    }
                }
            }
            for (auto& c : inner(n)) walk(c);
        };
        walk(body);
        if (reported || !fn_name_crypto_related(fn.name) || fn.ptr_params.empty()) return;
        for (auto& pid : fn.ptr_params) {
            auto lit = fn.locals.find(pid);
            if (lit == fn.locals.end()) continue;
            auto pname = str_field(*lit->second, "name");
            std::function<void(const json&)> scan = [&](const json& n) {
                if (reported || !n.is_object()) return;
                if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "=" && lvalue_names_ptr(child(n, 0), pid) &&
                    is_rand_call_expr(strip_casts(child(n, 1)))) {
                    add(n, "CRYPTO-MISUSE", "rand() assigned into pointer parameter " + pname);
                    reported = true;
                    return;
                }
                for (auto& c : inner(n)) scan(c);
            };
            scan(body);
            if (reported) return;
        }
        std::unordered_set<std::string> rand_locals;
        std::function<void(const json&)> collect = [&](const json& n) {
            if (!n.is_object()) return;
            if (kind(n) == "BinaryOperator" && str_field(n, "opcode") == "=") {
                const auto& l = strip_all(child(n, 0));
                if (kind(l) == "DeclRefExpr" && is_rand_call_expr(strip_casts(child(n, 1))))
                    rand_locals.insert(str_field(ref_decl(l), "name"));
            }
            for (auto& c : inner(n)) collect(c);
        };
        collect(body);
        if (rand_locals.empty()) return;
        for (auto& pid : fn.ptr_params) {
            auto lit = fn.locals.find(pid);
            if (lit == fn.locals.end()) continue;
            auto pname = str_field(*lit->second, "name");
            for (auto& local : rand_locals) {
                std::function<void(const json&)> scan = [&](const json& n) {
                    if (reported || !n.is_object()) return;
                    if (store_local_into_ptr(n, pid, local)) {
                        add(n, "CRYPTO-MISUSE", "rand() in " + local + " copied into " + pname);
                        reported = true;
                        return;
                    }
                    if (kind(n) == "CallExpr" && one_of(callee_name(n), {"memcpy", "memmove"})) {
                        auto args = call_args(n);
                        if (args.size() >= 3) {
                            const auto& dst = strip_all(*args[0]);
                            const auto& src = strip_all(*args[1]);
                            if (kind(dst) == "DeclRefExpr" && str_field(ref_decl(dst), "id") == pid &&
                                kind(src) == "DeclRefExpr" && str_field(ref_decl(src), "name") == local) {
                                add(n, "CRYPTO-MISUSE", "rand() in " + local + " copied into " + pname);
                                reported = true;
                                return;
                            }
                        }
                    }
                    for (auto& c : inner(n)) scan(c);
                };
                scan(body);
                if (reported) return;
            }
        }
    }
};

// Compound statements whose last statement leaves (return, goto, break,
// continue, or a call to exit/abort/...): their effects do not reach the
// code after them. Offsets [begin, end] in the body's file.
bool leaving_stmt(const json& st) {
    const auto& s = strip_all(st);
    const auto& k = kind(s);
    if (k == "ReturnStmt" || k == "GotoStmt" || k == "BreakStmt" || k == "ContinueStmt" || k == "CXXThrowExpr")
        return true;
    if (k == "CallExpr")
        return one_of(callee_name(s), {"exit", "_exit", "_Exit", "quick_exit", "abort", "err", "errx", "longjmp",
                                        "siglongjmp", "trap", "unreachable"});
    return false;
}

void leaving_blocks_walk(const json& n, const json& body, const Pos& base,
                         std::vector<std::pair<long long, long long>>& out) {
    if (!n.is_object()) return;
    const auto& k = kind(n);
    if (k == "LambdaExpr" || k == "BlockExpr") return;
    if (&n != &body && k == "CompoundStmt" && n_children(n) > 0 && leaving_stmt(last_child(n))) {
        auto b = begin_of(n), e = end_of(n);
        if (b.file && base.file && *b.file == *base.file && b.offset >= 0 && e.offset >= 0)
            out.emplace_back(b.offset, e.offset);
    }
    for (auto& c : inner(n)) leaving_blocks_walk(c, body, base, out);
}

std::vector<std::pair<long long, long long>> leaving_blocks(const json& body) {
    std::vector<std::pair<long long, long long>> out;
    leaving_blocks_walk(body, body, begin_of(body), out);
    std::sort(out.begin(), out.end());
    return out;
}

// Replays leaving_blocks over events sorted by offset: state saved on entry,
// restored when the walk passes the block's end.
template <class State>
struct LeaveReplay {
    std::vector<std::pair<long long, long long>> blocks;
    std::size_t next = 0;
    std::vector<std::pair<long long, State>> stack;
    void at(long long off, State& st) {
        while (!stack.empty() && stack.back().first < off) {
            st = std::move(stack.back().second);
            stack.pop_back();
        }
        while (next < blocks.size() && blocks[next].first <= off) {
            if (blocks[next].second >= off) stack.emplace_back(blocks[next].second, st);
            ++next;
        }
    }
};

void walk_for_locks(const json& n, std::vector<const json*>& calls, std::vector<const json*>& returns) {
    if (!n.is_object()) {
        if (n.is_array())
            for (auto& c : n) walk_for_locks(c, calls, returns);
        return;
    }
    const auto& k = kind(n);
    if (is_call_kind(k)) calls.push_back(&n);
    if (k == "ReturnStmt") returns.push_back(&n);
    for (auto& c : inner(n)) walk_for_locks(c, calls, returns);
}

void lock_function_checks(const json& body, Fn& fn, Checker& ck, Ctx& cx) {
    std::vector<const json*> calls, returns;
    walk_for_locks(body, calls, returns);
    std::map<LockKey, std::vector<int>> acquire_lines, release_lines;
    struct Ev {
        int line;
        const json* at;
        LockKey key;
        bool release;
    };
    std::vector<Ev> seq;
    for (auto* c : calls) {
        if (in_macro(*c)) continue;
        auto name = callee_name(*c);
        if (!lock_callee_ok(name)) continue;
        auto args = call_args(*c);
        if (args.empty()) continue;
        auto arg = lock_arg_key(*args[0], cx);
        int ln = cx.line_of(begin_of(*c));
        if (ln <= 0) continue;
        if (is_unlock_name(name)) {
            seq.push_back({ln, c, {name, arg}, true});
            release_lines[{name, arg}].push_back(ln);
        } else if (unlock_of(name)) {
            seq.push_back({ln, c, {name, arg}, false});
            acquire_lines[{name, arg}].push_back(ln);
        }
    }
    // Same-line calls keep source order. std::sort is not stable.
    std::stable_sort(seq.begin(), seq.end(), [](const Ev& a, const Ev& b) {
        return begin_of(*a.at).offset < begin_of(*b.at).offset;
    });
    {
        std::vector<std::string> order;
        Pos first{};
        for (auto& e : seq) {
            if (e.release || !unlock_of(e.key.first) || e.key.second.empty()) continue;
            if (std::find(order.begin(), order.end(), e.key.second) != order.end()) continue;
            if (order.empty()) first = begin_of(*e.at);
            order.push_back(e.key.second);
            if (order.size() == 2) break;
        }
        if (order.size() == 2 && first.file)
            cx.lock_ords.push_back({*first.file, fn.name, order[0], order[1], first});
    }
    std::map<LockKey, std::optional<bool>> held;
    LeaveReplay<std::map<LockKey, std::optional<bool>>> replay{leaving_blocks(body), 0, {}};
    for (auto& e : seq) {
        replay.at(begin_of(*e.at).offset, held);
        if (e.release) {
            // lock_double_lock keys the mutex by the lock name.
            if (auto lock_name = lock_of(e.key.first)) held[{*lock_name, e.key.second}] = false;
            auto it = held.find(e.key);
            if (it != held.end() && it->second == false) {
                ck.add(*e.at, "LOCK-DOUBLE-UNLOCK",
                       e.key.first + "(" + e.key.second + ") released twice without an acquire between");
                break;
            }
            held[e.key] = false;
            continue;
        }
        auto paired = unlock_of(e.key.first);
        if (!paired) continue;
        auto it = held.find(e.key);
        if (it != held.end() && it->second == true) {
            ck.add(*e.at, "LOCK-DOUBLE-LOCK",
                   e.key.first + "(" + e.key.second + ") acquired twice without a release between");
            break;
        }
        held[e.key] = true;
        // lock_double_unlock keys the mutex by the unlock name and sets that
        // key on acquire. Without it, lock/unlock/lock/unlock looks like a
        // second release.
        held[{*paired, e.key.second}] = true;
    }
    std::vector<int> ret_lines;
    for (auto* r : returns) {
        if (in_macro(*r)) continue;
        int ln = cx.line_of(begin_of(*r));
        if (ln > 0) ret_lines.push_back(ln);
    }
    if (ret_lines.size() < 2) return;
    for (auto& [key, hits] : acquire_lines) {
        auto u = unlock_of(key.first);
        if (!u) continue;
        auto rel_it = release_lines.find({*u, key.second});
        if (rel_it == release_lines.end() || rel_it->second.empty()) continue;
        const auto& rels = rel_it->second;
        std::vector<int> held_returns;
        for (int r : ret_lines) {
            if (max_before_line(hits, r) > max_before_line(rels, r)) held_returns.push_back(r);
        }
        if (!held_returns.empty() && held_returns.size() < ret_lines.size()) {
            const json* at = nullptr;
            for (auto* ret : returns) {
                if (cx.line_of(begin_of(*ret)) == held_returns.front()) {
                    at = ret;
                    break;
                }
            }
            if (!at) continue;
            ck.add(*at, "LOCK-IMBALANCE",
                   key.first + " held on " + std::to_string(held_returns.size()) + " of " +
                       std::to_string(ret_lines.size()) + " returns; released on the others");
        }
    }
}

// ---------------------------------------------------------------- flow (straight-line)

enum class St { None, Freed, Nulled, Moved, Uninit, Malloc, New, NewArr };

struct VState {
    St st = St::None;
    int line = 0;
    std::string api;  // libc allocator name when st == Malloc
    bool nowait = false;
};

struct Flow {
    Checker& ck;
    Ctx& cx;
    Fn& fn;
    std::map<std::string, VState> st;
    bool stopped = false;
    // PTR-NULL-DEREF after `if (p == NULL)`: report at the if, once.
    const json* null_if = nullptr;
    bool null_reported = false;
    bool null_after = false;  // following the NULL path past the if

    Flow(Checker& c) : ck(c), cx(c.cx), fn(c.fn) {}

    bool tracked(const std::string& id) const { return !id.empty() && fn.locals.contains(id); }

    const json* var_of(const std::string& id) const {
        auto it = fn.locals.find(id);
        return it == fn.locals.end() ? nullptr : it->second;
    }

    void havoc(const json& n) {
        if (!n.is_object()) {
            if (n.is_array())
                for (auto& c : n) havoc(c);
            return;
        }
        if (kind(n) == "DeclRefExpr") st.erase(str_field(ref_decl(n), "id"));
        for (auto& c : inner(n)) havoc(c);
    }

    static bool is_record_value(const std::string& t) {
        return !t.empty() && !is_builtin_arith(t) && !is_pointer_type(t) && t.find('[') == std::string::npos &&
               t.find('&') == std::string::npos && !t.starts_with("enum ");
    }

    St alloc_state(const json& rhs) {
        const auto& c = strip_casts(rhs);
        if (is_call_kind(kind(c))) {
            const auto* f = cx.facts(callee_id(c));
            auto nm = callee_name(c);
            if (f && f->user && !libc_api_lint(nm)) return St::None;
            if (one_of(nm, {"malloc", "calloc", "realloc", "strdup", "strndup", "aligned_alloc", "reallocarray",
                            "getenv", "secure_getenv"}))
                return St::Malloc;
        }
        if (kind(c) == "CXXNewExpr") {
            if (flag(c, "arrayUnknown")) return St::None;
            return flag(c, "isArray") ? St::NewArr : St::New;
        }
        return St::None;
    }

    void assign(const std::string& id, const json* rhs) {
        if (!tracked(id)) return;
        st.erase(id);
        if (!rhs) return;
        const auto* v = var_of(id);
        if (v && is_pointer_type(type_of(*v)) && is_null_ptr_expr(*rhs) && !in_macro(strip_casts(*rhs)) &&
            kind(strip_casts(*rhs)) != "CXXNullPtrLiteralExpr") {
            st[id] = {St::Nulled, cx.line_of(begin_of(*rhs)), {}};
            return;
        }
        if (v && is_pointer_type(type_of(*v)) && kind(strip_casts(*rhs)) == "CXXNullPtrLiteralExpr") {
            st[id] = {St::Nulled, cx.line_of(begin_of(*rhs)), {}};
            return;
        }
        auto a = alloc_state(*rhs);
        if (a != St::None) {
            VState vs{a, cx.line_of(begin_of(*rhs)), {}, false};
            const auto& c = strip_casts(*rhs);
            if (is_call_kind(kind(c))) {
                vs.api = callee_name(c);
                if (one_of(vs.api, {"malloc", "realloc"}) && malloc_call_has_nowait(c, cx)) vs.nowait = true;
            }
            st[id] = std::move(vs);
        }
    }

    enum class Ctx2 { Stmt, Return, Branch };

    // The read of `id` at DeclRefExpr `ref`; `parents` innermost last.
    void read(const std::string& id, const json& ref, const std::vector<const json*>& parents, Ctx2 c) {
        auto it = st.find(id);
        if (it == st.end()) return;
        auto s = it->second;
        const auto name = str_field(ref_decl(ref), "name");
        // Deref context: ImplicitCast -> (Paren)* -> UnaryOperator * / MemberExpr arrow / ArraySubscript base.
        bool deref = false, as_arg = false, returned = false;
        {
            std::size_t i = parents.size();
            const json* below = &ref;
            while (i > 0) {
                const json& p = *parents.at(i - 1);
                const auto& pk = kind(p);
                if (is_implicit_wrapper(pk) || pk == "ParenExpr") {
                    below = &p;
                    --i;
                    continue;
                }
                if (pk == "UnaryOperator" && str_field(p, "opcode") == "*") deref = true;
                else if (pk == "MemberExpr" && flag(p, "isArrow")) deref = true;
                else if (pk == "ArraySubscriptExpr" && &child(p, 0) == below) deref = true;
                else if (is_call_kind(pk) && &child(p, 0) != below) as_arg = true;
                else if (pk == "ReturnStmt") returned = true;
                break;
            }
            if (i == 0 && c == Ctx2::Return) returned = true;
        }
        switch (s.st) {
            case St::Uninit: {
                st.erase(it);
                const auto* v = var_of(id);
                if (deref && v && is_pointer_type(type_of(*v)))
                    ck.add(ref, "PTR-UNINIT", name + " is dereferenced before it is given a value");
                else if (returned || c == Ctx2::Return)
                    ck.add(ref, "UNINIT-RETURN", name + " is returned before it is given a value");
                else if (c == Ctx2::Branch)
                    ck.add(ref, "UNINIT-BRANCH", name + " decides a branch before it is given a value");
                else
                    ck.add(ref, "UNINIT-READ", name + " is read before it is given a value");
                return;
            }
            case St::Freed:
                if (deref || as_arg) {
                    st.erase(it);
                    ck.add(ref, "MEM-UAF",
                           name + " is " + (deref ? "dereferenced" : "passed on") + " after it was freed on line " +
                               std::to_string(s.line));
                }
                return;
            case St::Nulled:
                if (deref) {
                    st.erase(it);
                    if (null_if && null_after) {
                        if (!null_reported)
                            ck.add(ref, "PTR-NULL-DEREF",
                                   name + " is dereferenced here, but the NULL test on line " + std::to_string(s.line) +
                                       " lets the NULL case fall through to this statement");
                        null_reported = true;
                    } else if (null_if) {
                        if (!null_reported)
                            ck.add(*null_if, "PTR-NULL-DEREF",
                                   name + " is tested for NULL here and dereferenced on line " +
                                       std::to_string(cx.line_of(begin_of(ref))) + " in the branch where it is NULL");
                        null_reported = true;
                    } else {
                        ck.add(ref, "PTR-NULL-DEREF",
                               name + " is dereferenced after it was set to NULL on line " + std::to_string(s.line));
                    }
                }
                return;
            case St::Malloc:
                if (deref) {
                    st.erase(it);
                    const std::string fn = s.api.empty() ? "malloc" : s.api;
                    if (fn == "getenv" || fn == "secure_getenv")
                        ck.add(ref, "API-GETENV-NULL", name + " from getenv() used without a NULL test");
                    else if (fn == "strdup" || fn == "strndup")
                        ck.add(ref, "API-STRDUP-NULL", name + " from strdup() used without a NULL test");
                    else if (s.nowait)
                        ck.add(ref, "MEM-NOWAIT", name + " used without a NULL check after M_NOWAIT allocation");
                    else
                        ck.add(ref, "PTR-UNCHECKED-ALLOC",
                               name + " used without a NULL check after " + fn + "()");
                }
                return;
            case St::Moved:
                st.erase(it);
                ck.add(ref, "CXX-USE-AFTER-MOVE",
                       name + " is used after it was moved from on line " + std::to_string(s.line));
                return;
            default: return;
        }
    }

    void free_var(const std::string& id, const json& at, bool is_delete, bool array_form) {
        if (!tracked(id)) return;
        auto it = st.find(id);
        const auto* v = var_of(id);
        const std::string name = v ? str_field(*v, "name") : std::string("pointer");
        if (it != st.end()) {
            auto s = it->second;
            if (s.st == St::Freed)
                ck.add(at, "MEM-DOUBLE-FREE", name + " is freed twice (first on line " + std::to_string(s.line) + ")");
            else if (s.st == St::Malloc && is_delete)
                ck.add(at, "MEM-MISMATCHED-FREE",
                       name + " comes from malloc-family allocation (line " + std::to_string(s.line) + ") but is released with delete");
            else if ((s.st == St::New || s.st == St::NewArr) && !is_delete)
                ck.add(at, "MEM-MISMATCHED-FREE",
                       name + " comes from new (line " + std::to_string(s.line) + ") but is released with free()");
            else if (s.st == St::NewArr && is_delete && !array_form)
                ck.add(at, "MEM-NEW-DELETE",
                       name + " comes from new[] (line " + std::to_string(s.line) + ") but is released with delete, not delete[]");
            else if (s.st == St::New && is_delete && array_form)
                ck.add(at, "MEM-NEW-DELETE",
                       name + " comes from new (line " + std::to_string(s.line) + ") but is released with delete[]");
        }
        st[id] = {St::Freed, cx.line_of(begin_of(at)), {}};
    }

    bool noreturn_call(const json& c) const {
        const auto* f = cx.facts(callee_id(c));
        if (f && f->noreturn) return true;
        if (f && f->user) return false;
        return is_noreturn_name(callee_name(c));
    }

    void ev(const json& n, std::vector<const json*>& parents, Ctx2 c) {
        if (!n.is_object() || stopped) return;
        const auto& k = kind(n);
        if (k == "UnaryExprOrTypeTraitExpr" || k == "CXXTypeidExpr" || k == "CXXNoexceptExpr") return;
        if (k == "LambdaExpr" || k == "StmtExpr" || k == "BlockExpr") {
            havoc(n);
            return;
        }
        auto recurse = [&](const json& x) {
            parents.push_back(&n);
            ev(x, parents, c);
            parents.pop_back();
        };
        auto recurse_all = [&]() {
            for (auto& x : inner(n)) recurse(x);
        };
        if (k == "BinaryOperator") {
            const auto& op = str_field(n, "opcode");
            if (op == "&&" || op == "||" || op == ",") {
                recurse(child(n, 0));
                if (op == ",") recurse(child(n, 1));
                else havoc(child(n, 1));
                return;
            }
            if (op == "=") {
                recurse(child(n, 1));
                const auto& l = strip_parens(child(n, 0));
                if (kind(l) == "DeclRefExpr" && tracked(str_field(ref_decl(l), "id"))) {
                    assign(str_field(ref_decl(l), "id"), &child(n, 1));
                    return;
                }
                recurse(child(n, 0));
                return;
            }
        }
        if (k == "CompoundAssignOperator") {
            recurse(child(n, 1));
            const auto& l = strip_parens(child(n, 0));
            if (kind(l) == "DeclRefExpr") {
                auto id = str_field(ref_decl(l), "id");
                if (st.contains(id) && st[id].st == St::Uninit) {
                    std::vector<const json*> pp = parents;
                    pp.push_back(&n);
                    read(id, l, pp, c);
                }
                st.erase(id);
                return;
            }
            recurse(child(n, 0));
            return;
        }
        if (k == "UnaryOperator") {
            const auto& op = str_field(n, "opcode");
            const auto& o = strip_parens(child(n, 0));
            if ((op == "++" || op == "--") && kind(o) == "DeclRefExpr") {
                auto id = str_field(ref_decl(o), "id");
                if (st.contains(id) && st[id].st == St::Uninit) {
                    std::vector<const json*> pp = parents;
                    pp.push_back(&n);
                    read(id, o, pp, c);
                }
                st.erase(id);
                return;
            }
            if (op == "&") {  // address taken: may be written through
                havoc(n);
                return;
            }
        }
        if (k == "ConditionalOperator" || k == "BinaryConditionalOperator") {
            recurse(child(n, 0));
            for (std::size_t i = 1; i < n_children(n); ++i) havoc(child(n, i));
            return;
        }
        if (k == "CXXDeleteExpr") {
            const auto& o = strip_all(child(n, 0));
            if (kind(o) == "DeclRefExpr" && tracked(str_field(ref_decl(o), "id"))) {
                if (flag(n, "arrayUnknown")) {
                    st.erase(str_field(ref_decl(o), "id"));
                    return;
                }
                free_var(str_field(ref_decl(o), "id"), n, true, flag(n, "isArrayAsWritten"));
                return;
            }
            recurse_all();
            return;
        }
        if (is_call_kind(k)) {
            auto name = callee_name(n);
            const auto* f = cx.facts(callee_id(n));
            const bool user = f && f->user;
            auto args = call_args(n);
            // Member call on a moved-from object; reinitialising members.
            if (k == "CXXMemberCallExpr" && n_children(n) >= 1) {
                const auto& me = strip_all(child(n, 0));
                if (kind(me) == "MemberExpr" && n_children(me) >= 1) {
                    const auto& base = strip_all(child(me, 0));
                    if (kind(base) == "DeclRefExpr") {
                        auto id = str_field(ref_decl(base), "id");
                        auto mname = str_field(me, "name");
                        auto it = st.find(id);
                        if (it != st.end() && it->second.st == St::Moved) {
                            if (one_of(mname, {"clear", "reset", "assign", "emplace", "operator=", "swap"})) {
                                st.erase(it);
                            } else {
                                std::vector<const json*> pp = parents;
                                pp.push_back(&n);
                                read(id, base, pp, c);
                            }
                        }
                    }
                }
                for (auto* a : args) recurse(*a);
                if (noreturn_call(n)) stopped = true;
                return;
            }
            if (k == "CXXOperatorCallExpr" && decl_name(child(n, 0)) == "operator=" && args.size() == 2) {
                recurse(*args[1]);
                const auto& l = strip_all(*args[0]);
                if (kind(l) == "DeclRefExpr") {
                    st.erase(str_field(ref_decl(l), "id"));
                    return;
                }
                recurse(*args[0]);
                return;
            }
            const bool is_free = !user && one_of(name, {"free", "cfree"});
            const bool is_move = one_of(name, {"move"}) && args.size() == 1;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const auto& a = strip_parens(*args[i]);
                if (is_free && i == 0) {
                    const auto& o = strip_all(a);
                    if (kind(o) == "DeclRefExpr" && tracked(str_field(ref_decl(o), "id"))) continue;
                }
                if (is_move) {
                    const auto& o = strip_all(a);
                    if (kind(o) == "DeclRefExpr") continue;
                }
                // Passed by reference (no lvalue-to-rvalue conversion): may be written.
                if (kind(a) == "DeclRefExpr" && k != "CXXOperatorCallExpr") {
                    const auto& id = str_field(ref_decl(a), "id");
                    auto it = st.find(id);
                    if (it != st.end() && it->second.st != St::Moved) {
                        st.erase(it);
                        continue;
                    }
                }
                recurse(*args[i]);
            }
            if (is_free && !args.empty()) {
                const auto& o = strip_all(*args[0]);
                if (kind(o) == "DeclRefExpr") free_var(str_field(ref_decl(o), "id"), n, false, false);
            }
            if (is_move) {
                const auto& o = strip_all(*args[0]);
                if (kind(o) == "DeclRefExpr") {
                    auto id = str_field(ref_decl(o), "id");
                    // Only a move whose result is used moves; `std::move(a);` alone is a cast.
                    bool consumed = false;
                    for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
                        if (is_implicit_wrapper(kind(**it)) || kind(**it) == "ParenExpr") continue;
                        consumed = true;
                        break;
                    }
                    const auto* v = var_of(id);
                    if (consumed && v && is_record_value(type_of(*v)) && tracked(id) &&
                        !qual_type(*v).starts_with("const "))
                        st[id] = {St::Moved, cx.line_of(begin_of(n)), {}};
                }
            }
            if (noreturn_call(n)) stopped = true;
            return;
        }
        if (k == "CXXThrowExpr") {
            recurse_all();
            stopped = true;
            return;
        }
        if (k == "DeclRefExpr") {
            const auto& id = str_field(ref_decl(n), "id");
            if (!st.contains(id)) return;
            const json* p = parents.empty() ? nullptr : parents.back();
            const auto& pk = p ? kind(*p) : std::string();
            if (pk == "ImplicitCastExpr" &&
                !one_of(str_field(*p, "castKind"), {"ArrayToPointerDecay", "FunctionToPointerDecay"})) {
                read(id, n, parents, c);
                return;
            }
            if (st[id].st == St::Moved) {
                // Any other use of a moved-from object (member access, copy, pass).
                if (pk == "MemberExpr" || pk == "CXXConstructExpr" || is_call_kind(pk)) read(id, n, parents, c);
                return;
            }
            if (pk == "MemberExpr" && flag(*p, "isArrow")) {
                read(id, n, parents, c);
                return;
            }
            if (pk == "ArraySubscriptExpr" && n_children(*p) >= 1) {
                const auto& base = strip_all(child(*p, 0));
                if (kind(base) == "DeclRefExpr" && str_field(ref_decl(base), "id") == id) {
                    read(id, n, parents, c);
                    return;
                }
            }
            // Bound to a reference / other use: forget it.
            st.erase(id);
            return;
        }
        recurse_all();
    }

    void eval(const json& e, Ctx2 c) {
        std::vector<const json*> parents;
        ev(e, parents, c);
    }

    static bool uninit_candidate(const json& v) {
        if (kind(v) != "VarDecl" || flag(v, "isImplicit") || v.contains("init")) return false;
        if (!str_field(v, "storageClass").empty()) return false;
        for (auto& c : inner(v)) {
            if (!kind(c).ends_with("Attr")) return false;  // an initialiser the front end did not flag
            if (kind(c) == "CleanupAttr") return false;
        }
        const auto& qt = qual_type(v);
        if (qt.find("volatile") != std::string::npos || qt.find('&') != std::string::npos) return false;
        auto t = type_of(v);
        return (is_builtin_arith(t) || is_pointer_type(t) || t.starts_with("enum ")) && !in_macro(v);
    }

    // Statements of one block, in order.
    void block(const json& b) {
        for (auto& s : inner(b)) {
            if (stopped) return;
            stmt(s);
        }
    }

    void stmt(const json& s) {
        const auto& k = kind(s);
        if (k == "CompoundStmt") {
            block(s);
            return;
        }
        if (k == "DeclStmt") {
            for (auto& v : inner(s)) {
                if (kind(v) != "VarDecl") continue;
                const auto& id = str_field(v, "id");
                const json* init = nullptr;
                for (auto& c : inner(v))
                    if (!kind(c).ends_with("Attr")) init = &c;
                if (init) {
                    eval(*init, Ctx2::Stmt);
                    assign(id, init);
                } else if (uninit_candidate(v)) {
                    st[id] = {St::Uninit, cx.line_of(begin_of(v)), {}};
                }
            }
            return;
        }
        if (k == "ReturnStmt") {
            if (n_children(s)) eval(child(s, 0), Ctx2::Return);
            stopped = true;
            return;
        }
        if (k == "IfStmt") {
            std::size_t i = 0;
            if (flag(s, "hasInit")) stmt(child(s, i++));
            if (flag(s, "hasVar")) stmt(child(s, i++));
            const auto& cond = strip_all(child(s, i));
            std::optional<std::string> cond_assign;
            if (kind(cond) == "BinaryOperator" && str_field(cond, "opcode") == "==") {
                const auto& lhs = strip_all(strip_parens(child(cond, 0)));
                if (kind(lhs) == "BinaryOperator" && str_field(lhs, "opcode") == "=") {
                    const auto& l = strip_all(child(lhs, 0));
                    if (kind(l) == "DeclRefExpr") {
                        auto id = str_field(ref_decl(l), "id");
                        cond_assign = id;
                        eval(child(lhs, 1), Ctx2::Stmt);
                        assign(id, &child(lhs, 1));
                    }
                }
            }
            bool null_then = true;
            bool null_guarded = false;
            if (auto vid = null_test_var(child(s, i), null_then)) {
                const json* branch = null_then ? &child(s, i + 1) : nullptr;
                if (!null_then && n_children(s) > i + 2) branch = &child(s, i + 2);
                if (branch && branch->is_object() && !branch->empty() && flow_terminates(*branch)) {
                    st.erase(*vid);
                    null_guarded = true;
                }
            } else if (cond_assign && is_null_ptr_expr(child(cond, 1))) {
                const json* branch = &child(s, i + 1);
                if (branch->is_object() && !branch->empty() && flow_terminates(*branch)) {
                    st.erase(*cond_assign);
                    null_guarded = true;
                }
            }
            if (!null_guarded) eval(child(s, i), Ctx2::Branch);
            for (std::size_t j = i + 1; j < n_children(s); ++j) havoc(child(s, j));
            return;
        }
        if (k == "SwitchStmt" || k == "WhileStmt") {
            std::size_t i = 0;
            if (flag(s, "hasInit")) stmt(child(s, i++));
            if (flag(s, "hasVar")) stmt(child(s, i++));
            eval(child(s, i), Ctx2::Branch);
            for (std::size_t j = i + 1; j < n_children(s); ++j) havoc(child(s, j));
            return;
        }
        if (k == "ForStmt" && n_children(s) == 5) {
            stmt(child(s, 0));
            eval(child(s, 2), Ctx2::Branch);
            havoc(child(s, 3));
            havoc(child(s, 4));
            return;
        }
        if (k == "BreakStmt" || k == "ContinueStmt" || k == "GotoStmt" || k == "IndirectGotoStmt") {
            stopped = true;
            return;
        }
        if (k == "NullStmt") return;
        if (k == "AttributedStmt") {
            for (auto& c : inner(s))
                if (!kind(c).ends_with("Attr")) stmt(c);
            return;
        }
        if (k.ends_with("Stmt") && k != "ExprWithCleanups") {
            havoc(s);  // loops, try, labels, ...: not straight-line
            return;
        }
        eval(s, Ctx2::Stmt);
    }
};

// ---------------------------------------------------------------- the walk

struct Walker {
    Ctx& cx;
    Fn& fn;
    Checker ck;
    int try_depth = 0;
    int lambda_depth = 0;
    bool saw_return = false;
    const json* catch_var = nullptr;
    std::vector<std::vector<std::string>> scopes;  // names of locals, for CTRL-SHADOW

    Walker(Ctx& c, Fn& f) : cx(c), fn(f), ck{c, f} {}

    bool missing_return_tail_ok(const json& stmt) {
        if (kind(stmt_simple_tail(stmt)) == "ReturnStmt") return true;
        if (fatal_tail_callee(stmt)) return true;
        if (terminates(stmt)) return true;
        const auto txt = strip_ws(std::string(cx.text_of(stmt_simple_tail(stmt))));
        static const std::regex tail(
            R"(^\s*(?:\(\s*void\s*\)\s*)?(?:abort|exit|_exit|__builtin_unreachable)\s*\()");
        static const std::regex fn(R"(\b(?:panic|fatal|errx)\w*\s*\()");
        return std::regex_search(txt, tail) || std::regex_search(txt, fn);
    }

    // _Noreturn / [[noreturn]] spelled before the body (libclang keeps no attribute node for _Noreturn).
    bool decl_head_noreturn(const json& f, const json& body) {
        const auto b = begin_of(f);
        const auto e = begin_of(body);
        if (!b.file || !e.file || *b.file != *e.file) return false;
        const auto t = cx.text(b, e);
        static const std::regex kw(R"((?:\b_Noreturn\b|\[\[\s*(?:std::|gnu::)?noreturn\b|\b__attribute__\s*\(\(\s*(?:__)?noreturn\b))");
        return std::regex_search(t.begin(), t.end(), kw);
    }

    void finish_tail_checks(const json& f, const json& body) {
        if (fn.name == "main" || in_macro(body)) return;
        const json* last = last_executable_stmt(body);
        if (!last) return;
        if (!function_decl_noreturn(f) && !decl_head_noreturn(f, body) && !saw_return) {
            if (auto callee = fatal_tail_callee(*last)) {
                ck.add(*last, "FUNC-NORETURN",
                       fn.name + "() ends in " + *callee + "(), not declared noreturn");
            }
        }
        if (is_value_returning_type(fn.ret)) {
            const bool tail_ok = missing_return_tail_ok(*last);
            if (!saw_return || !tail_ok) {
                const auto msg = !saw_return ? fn.name + "() has no return statement"
                                             : fn.name + "() does not end in return or fatal exit";
                // At the closing brace: control falls off the end there (regex row parity).
                const auto close = end_of(body);
                if (close.file && !close.macro)
                    ck.add_pos(close, *last, "CTRL-MISSING-RETURN", msg);
                else
                    ck.add(*last, "CTRL-MISSING-RETURN", msg);
            }
        }
        std::unordered_set<std::string> atoi_ids;
        collect_atoi_vars(body, atoi_ids);
        if (atoi_ids.empty()) return;
        const auto b = begin_of(body);
        const auto e = end_of(body);
        const std::string body_text(b.file && e.file && *b.file == *e.file ? std::string(cx.text(b, e)) : std::string{});
        if (body_text.empty()) return;
        std::vector<std::string> vars(atoi_ids.begin(), atoi_ids.end());
        std::sort(vars.begin(), vars.end());
        for (auto& var : vars) {
            auto it = fn.locals.find(var);
            const std::string vname = it != fn.locals.end() ? str_field(*it->second, "name") : var;
            if (has_bounds_guard(vname, body_text)) continue;
            const json* hit = nullptr;
            std::function<void(const json&)> find_use = [&](const json& n) {
                if (hit || !n.is_object()) return;
                if (kind(n) == "ArraySubscriptExpr" && n_children(n) >= 2 && ref_id(child(n, 1)) == var) {
                    hit = &n;
                    return;
                }
                if (is_call_kind(kind(n))) {
                    auto cn = callee_name(n);
                    auto a = call_args(n);
                    if (one_of(cn, {"malloc", "alloca"}) && !a.empty() && ref_id(*a[0]) == var) {
                        hit = &n;
                        return;
                    }
                    if (cn == "calloc" && !a.empty() && ref_id(*a[0]) == var) {
                        hit = &n;
                        return;
                    }
                    if (cn == "realloc" && a.size() >= 2 && ref_id(*a[1]) == var) {
                        hit = &n;
                        return;
                    }
                }
                for (auto& c : inner(n)) find_use(c);
            };
            find_use(body);
            if (hit) {
                ck.add(*hit, "INT-ATOI",
                       vname + " from atoi/atol/atoll used as index or allocation size without a range check");
            }
        }
    }

    static bool stmt_pos(const json* parent, std::size_t idx) {
        if (!parent) return false;
        const auto& pk = kind(*parent);
        if (pk == "CompoundStmt") return true;
        if (pk == "CaseStmt") return idx + 1 == n_children(*parent);
        if (pk == "DefaultStmt" || pk == "LabelStmt") return true;
        if (pk == "AttributedStmt") return true;
        if (pk == "IfStmt") {
            std::size_t cond = (flag(*parent, "hasInit") ? 1 : 0) + (flag(*parent, "hasVar") ? 1 : 0);
            return idx > cond;
        }
        if (pk == "WhileStmt" || pk == "CXXForRangeStmt") return idx + 1 == n_children(*parent);
        if (pk == "DoStmt") return idx == 0;
        if (pk == "ForStmt") return idx == 0 || idx == 3 || idx == 4;
        return false;
    }

    void discarded(const json& s) {
        const auto& core = strip_all(s);
        const auto& k = kind(core);
        if (!is_call_kind(k)) return;
        auto id = callee_id(core);
        const auto* f = cx.facts(id);
        const bool user = f && f->user;
        auto name = callee_name(core);
        if (k == "CallExpr" && !name.empty() && !user) {
            static const std::map<std::string, std::pair<const char*, bool>, std::less<>> kApi = {
#include "prism/astlint_discard.inc"
            };
            auto it = kApi.find(name);
            if (it == kApi.end()) {
                // pthread_rwlock_* style prefixes
                for (auto& [n, v] : kApi) {
                    if (n.ends_with("*") && name.starts_with(n.substr(0, n.size() - 1))) {
                        it = kApi.find(n);
                        break;
                    }
                }
            }
            if (it != kApi.end()) {
                auto args = call_args(core);
                if (it->second.second && (args.empty() || !is_zero_literal(*args[0]))) return;
                if (!in_macro(core))
                    ck.add(core, it->second.first,
                           name + "() return is discarded: its failure goes unnoticed (test the result)");
                return;
            }
        }
        if (f && f->nodiscard && !in_macro(core)) {
            auto nm = name.empty() ? decl_name(child(core, 0)) : name;
            ck.add(core, "CXX-NODISCARD", nm + "() is [[nodiscard]] and its result is discarded");
        }
    }

    // CTRL-FALLTHROUGH: a case body that runs into the next label.
    // `break_leaves` is false inside a nested switch: there break and
    // continue end only the inner statement, not the enclosing arm.
    bool terminates(const json& s, bool break_leaves = true) {
        const auto& k = kind(s);
        if (one_of(k, {"ReturnStmt", "GotoStmt", "IndirectGotoStmt", "CXXThrowExpr"})) return true;
        if (one_of(k, {"BreakStmt", "ContinueStmt"})) return break_leaves;
        if (k == "CompoundStmt") return n_children(s) && terminates(last_child(s), break_leaves);
        if (k == "AttributedStmt") {
            for (auto& c : inner(s))
                if (kind(c) == "FallThroughAttr") return break_leaves;
            return n_children(s) && terminates(last_child(s), break_leaves);
        }
        if (k == "LabelStmt") return n_children(s) && terminates(last_child(s), break_leaves);
        if (k == "IfStmt") {
            std::size_t cond = (flag(s, "hasInit") ? 1 : 0) + (flag(s, "hasVar") ? 1 : 0);
            if (n_children(s) < cond + 3) return false;
            return terminates(child(s, cond + 1), break_leaves) && terminates(child(s, cond + 2), break_leaves);
        }
        if (k == "SwitchStmt") return switch_leaves(s);
        if (k == "DoStmt" || k == "WhileStmt" || k == "ForStmt") {
            // while (1) { ... } without break
            return false;
        }
        const auto& core = strip_all(s);
        if (kind(core) == "CXXThrowExpr") return true;
        if (is_call_kind(kind(core))) {
            const auto* f = cx.facts(callee_id(core));
            if (f && f->noreturn) return true;
            if (!(f && f->user) && is_noreturn_name(callee_name(core))) return true;
        }
        return false;
    }

    static bool has_break(const json& s) {
        if (kind(s) == "BreakStmt") return true;
        for (auto& c : inner(s))
            if (has_break(c)) return true;
        return false;
    }

    // A nested switch ends the enclosing case arm when it has a default,
    // holds no break (a break leaves only the inner switch) and its last
    // statement ends in return / goto / throw / a noreturn call. With no
    // break, every group either leaves or runs into the next one, so all
    // paths reach that last statement.
    bool switch_leaves(const json& sw) {
        std::size_t ci = (flag(sw, "hasInit") ? 1 : 0) + (flag(sw, "hasVar") ? 1 : 0);
        if (n_children(sw) < ci + 2) return false;
        const auto& body = child(sw, ci + 1);
        if (kind(body) != "CompoundStmt" || has_break(body)) return false;
        bool has_default = false;
        const json* last_stmt = nullptr;
        for (auto& item : inner(body)) {
            const json* p = &item;
            while ((kind(*p) == "CaseStmt" || kind(*p) == "DefaultStmt") && n_children(*p)) {
                if (kind(*p) == "DefaultStmt") has_default = true;
                p = &last_child(*p);
            }
            if (kind(*p) == "DefaultStmt") has_default = true;
            if (kind(*p) != "CaseStmt" && kind(*p) != "DefaultStmt") last_stmt = p;
        }
        return has_default && last_stmt && terminates(*last_stmt, false);
    }

    const json& last_simple(const json& s) {
        const json* p = &s;
        while ((kind(*p) == "CompoundStmt" || kind(*p) == "AttributedStmt" || kind(*p) == "LabelStmt") && n_children(*p))
            p = &last_child(*p);
        return *p;
    }

    void fallthrough(const json& sw) {
        std::size_t ci = (flag(sw, "hasInit") ? 1 : 0) + (flag(sw, "hasVar") ? 1 : 0);
        const auto& body = child(sw, ci + 1);
        if (kind(body) != "CompoundStmt" || in_macro(sw)) return;
        struct Group {
            const json* label;
            std::vector<const json*> stmts;
        };
        std::vector<Group> groups;
        for (auto& item : inner(body)) {
            const json* p = &item;
            if (kind(*p) == "CaseStmt" || kind(*p) == "DefaultStmt") {
                groups.push_back({p, {}});
                while ((kind(*p) == "CaseStmt" || kind(*p) == "DefaultStmt") && n_children(*p)) {
                    p = &last_child(*p);
                    if (kind(*p) == "CaseStmt" || kind(*p) == "DefaultStmt") groups.back().label = p;
                }
                if (kind(*p) != "CaseStmt" && kind(*p) != "DefaultStmt") groups.back().stmts.push_back(p);
                continue;
            }
            if (!groups.empty()) groups.back().stmts.push_back(p);
        }
        for (std::size_t g = 0; g + 1 < groups.size(); ++g) {
            auto& gr = groups.at(g);
            std::vector<const json*> real;
            for (auto* s : gr.stmts)
                if (kind(*s) != "NullStmt") real.push_back(s);
            if (real.empty()) continue;
            const json& last = *gr.stmts.back();
            if (terminates(last)) continue;
            const auto& ls = last_simple(last);
            if (kind(ls) == "NullStmt" && gr.stmts.size() == 1) continue;
            // An annotation comment between the last statement and the next label.
            auto b = begin_of(ls);
            if (g + 1 >= groups.size()) continue;
            auto e = begin_of(*groups.at(g + 1).label);
            if (b.macro || e.macro) continue;
            auto t = std::string(cx.text(b, e));
            std::string low;
            for (char ch : t) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (std::regex_search(low, std::regex(R"(falls?[ \t-]*thr(ough|u)|no\s*break)"))) continue;
            // Reported at the arm's first statement, as the regex lint does
            // (one row per arm, on the line after its label).
            const json& at = begin_of(*real.front()).macro ? ls : *real.front();
            ck.add(at, "CTRL-FALLTHROUGH",
                   "this case falls through into the next label without break or a [[fallthrough]] annotation "
                   "(line " + std::to_string(cx.line_of(begin_of(ls))) + ")");
        }
    }

    const json* return_core(const json& r) {
        if (n_children(r) == 0) return nullptr;
        return &child(r, 0);
    }

    // The automatic variable an lvalue names (x, x.f, x[i]).
    const json* lvalue_base(const json& e0) {
        const json* e = &strip_parens(e0);
        for (;;) {
            const auto& k = kind(*e);
            if (k == "MemberExpr" && !flag(*e, "isArrow")) e = &strip_all(child(*e, 0));
            else if (k == "ArraySubscriptExpr") {
                const auto& b = strip_parens(child(*e, 0));
                if (kind(b) == "ImplicitCastExpr" && str_field(b, "castKind") == "ArrayToPointerDecay")
                    e = &strip_parens(child(b, 0));
                else
                    return nullptr;
            } else break;
        }
        if (kind(*e) != "DeclRefExpr") return nullptr;
        auto* v = ck.local_var(*e);
        return ck.automatic(v) ? v : nullptr;
    }

    bool local_array(const json* v) const {
        if (!v || !ck.automatic(v)) return false;
        if (!one_of(kind(*v), {"VarDecl", "ParmVarDecl"})) return false;
        return type_of(*v).find('[') != std::string::npos;
    }

    const json* decayed_local_array(const json& e0) {
        const auto& e = strip_parens(e0);
        if (kind(e) == "ImplicitCastExpr" && str_field(e, "castKind") == "ArrayToPointerDecay") {
            const auto& d = strip_parens(child(e, 0));
            if (kind(d) == "DeclRefExpr") {
                auto* v = ck.local_var(d);
                if (local_array(v)) return v;
            }
        }
        // strip_casts (above) may have removed the decay cast; `return buf` is then just DeclRefExpr.
        if (kind(e) == "DeclRefExpr") {
            auto* v = ck.local_var(e);
            if (local_array(v)) return v;
        }
        return nullptr;
    }

    void return_stmt(const json& r) {
        const json* e = return_core(r);
        if (!e || lambda_depth) return;
        const bool ret_ptr = is_pointer_type(fn.ret);
        const bool ret_ref = fn.ret.ends_with("&");
        if ((!ret_ptr && !ret_ref && !fn.ret.empty()) || ret_ptr) {
            dangling_view(r, *e);
            if (!ret_ptr) return;
        }
        const auto& core = strip_casts(*e);
        const json* esc = nullptr;
        std::string how;
        if (ret_ptr) {
            if (kind(core) == "UnaryOperator" && str_field(core, "opcode") == "&") {
                esc = lvalue_base(child(core, 0));
                how = "the address of";
            } else if (auto* v = decayed_local_array(core)) {
                esc = v;
                how = "the array";
            } else if (kind(core) == "BinaryOperator" && one_of(str_field(core, "opcode"), {"+", "-"})) {
                for (std::size_t i = 0; i < 2 && !esc; ++i)
                    if (auto* v = decayed_local_array(child(core, i))) {
                        esc = v;
                        how = "a pointer into the array";
                    }
            }
        } else if (ret_ref) {
            const auto& c2 = strip_implicit(*e);
            if (auto* v = lvalue_base(c2); v && fn.ret.find("&&") == std::string::npos) {
                esc = v;
                how = "a reference to";
            }
            // A temporary bound to the returned reference.
            if (!esc) {
                const json* t = e;
                bool materialized = false;
                while (is_implicit_wrapper(kind(*t)) || kind(*t) == "ParenExpr") {
                    if (kind(*t) == "MaterializeTemporaryExpr") materialized = true;
                    if (n_children(*t) == 0) break;
                    t = &child(*t, 0);
                }
                const auto& vc = str_field(*t, "valueCategory");
                const bool temp_kind = one_of(kind(*t), {"CXXFunctionalCastExpr", "CXXTemporaryObjectExpr",
                                                         "CXXConstructExpr", "IntegerLiteral", "FloatingLiteral",
                                                         "CXXBoolLiteralExpr", "CharacterLiteral"});
                if ((materialized || vc == "prvalue" || temp_kind) && !in_macro(*t) &&
                    kind(*t) != "ConditionalOperator") {
                    ck.add(r, "CXX-BIND-TMP",
                           "the returned reference binds to a temporary that is destroyed at the end of the return "
                           "statement (" + fn.ret + ")");
                    return;
                }
            }
        }
        if (!esc) return;
        auto vn = str_field(*esc, "name");
        ck.add(r, "MEM-STACK-ESCAPE",
               "returns " + how + " " + vn + ", which lives on this function's stack and dies when it returns");
    }

    // A view (string_view / span) of a local owning object returned by value.
    void dangling_view(const json& r, const json& e) {
        const auto& ret = fn.ret;
        if (is_pointer_type(ret)) {
            // `return s.c_str();` / `return v.data();` of a local owning object.
            const auto& c = strip_all(e);
            if (kind(c) != "CXXMemberCallExpr" || n_children(c) == 0) return;
            const auto& me = strip_all(child(c, 0));
            if (kind(me) != "MemberExpr" || !one_of(str_field(me, "name"), {"c_str", "data"}) || n_children(me) == 0)
                return;
            const auto& obj = strip_all(child(me, 0));
            if (kind(obj) != "DeclRefExpr") return;
            auto* v = ck.local_var(obj);
            if (!ck.automatic(v) || kind(*v) != "VarDecl") return;
            auto t = type_of(*v);
            if (is_pointer_type(t) || t.find("view") != std::string::npos || t.find("span") != std::string::npos) return;
            if (t.find("string") == std::string::npos && t.find("vector") == std::string::npos) return;
            auto vn = str_field(*v, "name");
            ck.add(r, "CXX-DANGLING-REF",
                   "returns " + vn + "." + str_field(me, "name") + "() of the local " + vn +
                       ", whose buffer is freed when the function returns");
            return;
        }
        if (ret.find("string_view") == std::string::npos && ret.find("span<") == std::string::npos) return;
        const json* p = &e;
        for (int guard = 0; guard < 16; ++guard) {
            const auto& k = kind(*p);
            if ((is_implicit_wrapper(k) || k == "ParenExpr" || k == "CXXConstructExpr" || k == "CXXFunctionalCastExpr") &&
                n_children(*p) >= 1) {
                p = &child(*p, 0);
                continue;
            }
            if (k == "CXXMemberCallExpr" && n_children(*p) >= 1 && kind(strip_all(child(*p, 0))) == "MemberExpr" &&
                str_field(strip_all(child(*p, 0)), "name").starts_with("operator")) {
                const auto& me = strip_all(child(*p, 0));
                if (n_children(me) == 0) return;
                p = &child(me, 0);
                continue;
            }
            break;
        }
        const auto& d = strip_all(*p);
        if (kind(d) != "DeclRefExpr") return;
        auto* v = ck.local_var(d);
        if (!ck.automatic(v) || kind(*v) != "VarDecl") return;
        auto t = type_of(*v);
        if (t.find("string_view") != std::string::npos || t.find("span") != std::string::npos || is_pointer_type(t))
            return;
        if (t.find("string") == std::string::npos && t.find("vector") == std::string::npos &&
            t.find("array") == std::string::npos && t.find('[') == std::string::npos)
            return;
        ck.add(r, "CXX-DANGLING-REF",
               "returns a " + ret + " of the local " + str_field(*v, "name") + ", which is destroyed when the function returns");
    }

    void throw_expr(const json& t) {
        if (lambda_depth) return;
        if (n_children(t) == 0) return;  // rethrow
        const auto& op = strip_all(child(t, 0));
        if (kind(op) == "CXXNewExpr") {
            ck.add(t, "CXX-THROW-NEW", "throw new ...: the exception is a pointer the handler must delete (throw by value)");
        }
        if (catch_var) {
            // throw e; inside the handler that caught e: copies (and slices) it.
            const json* o = &child(t, 0);
            while ((is_implicit_wrapper(kind(*o)) || kind(*o) == "CXXConstructExpr" || kind(*o) == "ParenExpr") &&
                   n_children(*o) >= 1)
                o = &child(*o, 0);
            if (kind(*o) == "DeclRefExpr" && str_field(ref_decl(*o), "id") == str_field(*catch_var, "id")) {
                ck.add(t, "CXX-THROW-COPY",
                       "throw " + std::string(str_field(*catch_var, "name")) +
                           "; throws a copy of the caught exception (sliced to " + std::string(qual_type(*catch_var)) +
                           "): use throw; to rethrow it");
            }
        }
        if (try_depth > 0) return;
        if (fn.kind == "CXXDestructorDecl" && fn.noexcept_) {
            ck.add(t, "CXX-THROW-DESTRUCTOR", "destructor throws: destructors are noexcept, so this calls std::terminate");
        } else if (fn.noexcept_) {
            ck.add(t, "CXX-THROW-NOEXCEPT", "throw in a noexcept function outside any try block: calls std::terminate");
        }
    }

    void virtual_in_ctor(const json& c) {
        if (lambda_depth || (fn.kind != "CXXConstructorDecl" && fn.kind != "CXXDestructorDecl")) return;
        if (kind(c) != "CXXMemberCallExpr" || n_children(c) == 0) return;
        const auto& me = strip_all(child(c, 0));
        if (kind(me) != "MemberExpr" || n_children(me) == 0) return;
        if (kind(strip_all(child(me, 0))) != "CXXThisExpr") return;
        const auto* f = cx.facts(str_field(me, "referencedMemberDecl"));
        if (!f || !f->is_virtual) return;
        auto t = cx.text_of(me);
        if (t.find("::") != std::string_view::npos) return;  // Base::f(): no virtual dispatch
        ck.add(c, "CXX-VIRTUAL-IN-CTOR",
               str_field(me, "name") + "() is virtual and called from a " +
                   (fn.kind == "CXXConstructorDecl" ? "constructor" : "destructor") +
                   ": the call does not reach an override in a derived class");
    }

    void empty_loop(const json& n) {
        const auto& k = kind(n);
        const json* cond = nullptr;
        const json* body = nullptr;
        bool no_cond = false;
        if (k == "ForStmt" && n_children(n) == 5) {
            cond = &child(n, 2);
            no_cond = !cond->is_object() || cond->empty() || !cond->contains("kind");
            body = &child(n, 4);
        } else if (k == "WhileStmt" && !flag(n, "hasVar") && n_children(n) == 2) {
            cond = &child(n, 0);
            body = &child(n, 1);
        } else if (k == "DoStmt" && n_children(n) == 2) {
            body = &child(n, 0);
            cond = &child(n, 1);
        }
        if (!body || in_macro(n)) return;
        bool empty = kind(*body) == "NullStmt" || (kind(*body) == "CompoundStmt" && n_children(*body) == 0);
        if (!empty) return;
        bool forever = no_cond;
        if (!forever && cond) {
            auto v = int_value(*cond);
            const auto& cc = strip_all(*cond);
            forever = (v && *v != 0) || (kind(cc) == "CXXBoolLiteralExpr" && flag(cc, "value"));
        }
        if (!forever) return;
        if (k == "ForStmt" && n_children(n) == 5 && child(n, 3).is_object() && child(n, 3).contains("kind")) return;
        ck.add(n, "CTRL-EMPTY-INFINITE", "empty infinite loop: the function hangs here (undefined in C++ without side effects)");
    }

    void int_trunc(const json& target_type_holder, const json& value, const json& at) {
        // value is the (implicit-cast) initialiser / right-hand side.
        const json* v = &strip_parens(value);
        if (kind(*v) != "ImplicitCastExpr" || str_field(*v, "castKind") != "IntegralCast") return;
        auto to = type_of(*v);
        const auto& src = strip_implicit(child(*v, 0));
        auto from = type_of(child(*v, 0));
        int wt = int_width(to), wf = int_width(from);
        if (!wt || !wf || wt >= wf || to == "bool" || to == "_Bool") return;
        if (int_value(src) || in_macro(src) || constant_expr(src)) return;
        // Narrowing only when some operand really is wider than the target
        // (short = short + short computes in int but cannot exceed it much).
        int widest = 0;
        std::function<void(const json&)> leaves = [&](const json& x) {
            const auto& xs = strip_all(x);
            const auto& xk = kind(xs);
            if (xk == "DeclRefExpr" || xk == "MemberExpr" || is_call_kind(xk) || xk == "ArraySubscriptExpr" ||
                (xk == "UnaryOperator" && str_field(xs, "opcode") == "*")) {
                widest = std::max(widest, int_width(type_of(xs)));
                return;
            }
            if (is_explicit_cast(xk)) {
                widest = std::max(widest, int_width(type_of(xs)));
                return;
            }
            for (auto& c : inner(xs)) {
                if (leaves) leaves(c);
            }
        };
        leaves(src);
        if (widest <= wt) return;
        const auto& sk = kind(src);
        if (sk == "UnaryExprOrTypeTraitExpr" || sk == "CharacterLiteral") return;
        if (sk == "DeclRefExpr" && str_field(ref_decl(src), "kind") == "EnumConstantDecl") return;
        if (sk == "BinaryOperator") {
            const auto& op = str_field(src, "opcode");
            if (op == "&" || op == "%" || op == ">>") return;  // masked / reduced: likely fits
        }
        if (sk == "ConditionalOperator") return;
        (void)target_type_holder;
        auto sn = decl_name(src);
        ck.add(at, "INT-TRUNC",
               (sn.empty() ? std::string("a ") + from + " value" : sn + " (" + from + ")") + " is narrowed to " + to +
                   " implicitly: values outside its range are truncated");
    }

    void shadow_decl(const json& v) {
        if (lambda_depth || scopes.empty()) return;
        const auto& name = str_field(v, "name");
        if (name.empty() || flag(v, "isImplicit") || in_macro(v) || name.starts_with("__")) return;
        for (std::size_t i = 0; i + 1 < scopes.size(); ++i) {
            if (std::find(scopes[i].begin(), scopes[i].end(), name) != scopes[i].end()) {
                ck.add(v, "CTRL-SHADOW",
                       "local " + name + " shadows " + (i == 0 ? "the parameter" : "an outer local") + " of the same name");
                break;
            }
        }
        scopes.back().push_back(name);
    }

    // parents.back() is an `=`: true when the expression around it uses the
    // assigned value (not a statement, a comma's left side or a for-increment).
    static bool assign_value_used(const std::vector<const json*>& parents) {
        std::size_t i = parents.size() - 1;  // the `=`
        const json* cur = parents.at(i);
        while (i > 0) {
            const json* up = parents.at(i - 1);
            const auto& uk = kind(*up);
            if (uk == "ExprWithCleanups" || uk == "ParenExpr") {
                cur = up;
                --i;
                continue;
            }
            if (uk == "BinaryOperator" && str_field(*up, "opcode") == ",") {
                const json* lhs = &child(*up, 0);
                return lhs != cur && assign_value_used_at(parents, i - 1);
            }
            std::size_t at = 0;
            for (std::size_t j = 0; j < n_children(*up); ++j)
                if (&child(*up, j) == cur) at = j;
            return !stmt_pos(up, at);
        }
        return false;
    }
    static bool assign_value_used_at(const std::vector<const json*>& parents, std::size_t i) {
        std::vector<const json*> head(parents.begin(), parents.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        return assign_value_used(head);
    }

    void walk(const json& n, const json* parent, std::size_t idx, std::vector<const json*>& parents) {
        if (!n.is_object()) return;
        const auto& k = kind(n);
        if (is_template_kind(k)) return;  // local templates / generic lambdas' patterns
        if (k == "GCCAsmStmt" || k == "MSAsmStmt") fn.opaque = true;
        if (k == "LabelStmt" || k == "GotoStmt" || k == "IndirectGotoStmt") fn.has_labels = true;
        bool pushed_scope = false;
        if (k == "CompoundStmt") fn.blocks.push_back(&n);
        if (!lambda_depth && one_of(k, {"CompoundStmt", "ForStmt", "IfStmt", "WhileStmt", "SwitchStmt",
                                        "CXXForRangeStmt", "CXXCatchStmt"})) {
            scopes.emplace_back();
            pushed_scope = true;
        }
        if (stmt_pos(parent, idx) && k != "CompoundStmt" && !lambda_depth) discarded(n);
        if (k == "VarDecl") {
            if (!lambda_depth) {
                fn.locals[str_field(n, "id")] = &n;
                shadow_decl(n);
                if (is_mutex_local_type(qual_type(n))) {
                    const auto& id = str_field(n, "id");
                    fn.mutex_locals.insert(id);
                    for (auto& c : inner(n))
                        if (!kind(c).ends_with("Attr")) {
                            fn.mutex_inited.insert(id);
                            break;
                        }
                }
            }
            if (eligible_local(n)) {
                const auto& id = str_field(n, "id");
                if (!fn.vars.contains(id)) {
                    fn.vars[id] = Var{str_field(n, "name"), 0, nullptr};
                    fn.order.push_back(id);
                }
            }
            for (auto& c : inner(n))
                if (!kind(c).ends_with("Attr")) {
                    int_trunc(n, c, n);
                    break;
                }
        } else if (k == "DeclRefExpr") {
            const auto& id = str_field(ref_decl(n), "id");
            if (auto it = fn.vars.find(id); it != fn.vars.end()) {
                const bool store = parent && kind(*parent) == "BinaryOperator" && str_field(*parent, "opcode") == "=" &&
                                   idx == 0;
                // `if ((n = dup(fd)) < 0)`: the stored value is used by the
                // enclosing expression, so the store is not dead even when n
                // is never read again (the usual error-check idiom).
                if (!store || assign_value_used(parents)) ++it->second.reads;
                else if (!it->second.first_write && !begin_of(*parent).macro)
                    it->second.first_write = parent;
            }
        } else if (k == "IfStmt") {
            ck.assign_cond(child(n, (flag(n, "hasInit") ? 1 : 0) + (flag(n, "hasVar") ? 1 : 0)));
            null_branch(n, parent, idx);
        } else if (k == "WhileStmt") {
            const auto& c = child(n, flag(n, "hasVar") ? 1 : 0);
            ck.assign_cond(c);
            ck.loop_sign(c);
            empty_loop(n);
        } else if (k == "DoStmt") {
            const auto& c = child(n, 1);
            ck.assign_cond(c);
            ck.loop_sign(c);
            empty_loop(n);
        } else if (k == "ForStmt" && n_children(n) == 5) {
            ck.assign_cond(child(n, 2));
            ck.loop_sign(child(n, 2));
            empty_loop(n);
        } else if (k == "SwitchStmt") {
            ck.switch_hole(n);
            ck.masked_switch(n, parent, idx);
            fallthrough(n);
        } else if (k == "BinaryOperator") {
            ck.self_assign(n);
            ck.binop(n);
            ck.realloc_self(n);
            ck.self_move(n);
            if (str_field(n, "opcode") == "=") int_trunc(child(n, 0), child(n, 1), n);
        } else if (k == "CompoundAssignOperator") {
            ck.compound(n);
        } else if (k == "CXXOperatorCallExpr") {
            ck.self_move(n);
        } else if (k == "CallExpr") {
            ck.call(n, parents);
            ck.move_const(n);
            if (!lambda_depth) {
                ck.lock_init_call(n);
                ck.lock_acquire_call(n);
            }
        } else if (k == "MemberExpr") {
            ck.ptr_chain(n);
        } else if (k == "CXXMemberCallExpr") {
            virtual_in_ctor(n);
        } else if (k == "ImplicitCastExpr") {
            int_div(n);
        } else if (k == "UnaryExprOrTypeTraitExpr") {
            ck.sizeof_array_param(n);
        } else if (k == "CXXDeleteExpr") {
            if (!lambda_depth) ck.check_cxx_delete(n);
        } else if (k == "CXXThrowExpr") {
            throw_expr(n);
        } else if (k == "ReturnStmt") {
            saw_return = true;
            return_stmt(n);
        } else if (k == "CXXCatchStmt") {
            ck.catch_stmt(n);
        }
        // Children, with the context this node sets up.
        parents.push_back(&n);
        const auto& in = inner(n);
        if (in.is_array()) {
            for (std::size_t i = 0; i < in.size(); ++i) {
                const json* saved_catch = catch_var;
                int saved_try = try_depth;
                if (k == "CXXTryStmt" && i == 0) ++try_depth;
                if (k == "CXXCatchStmt") {
                    if (kind(child(n, 0)) == "VarDecl") catch_var = &child(n, 0);
                }
                if (k == "LambdaExpr") ++lambda_depth;
                walk(in[i], &n, i, parents);
                if (k == "LambdaExpr") --lambda_depth;
                catch_var = saved_catch;
                try_depth = saved_try;
            }
        }
        parents.pop_back();
        if (pushed_scope) scopes.pop_back();
    }

    // if (p == NULL) { ... *p ... }
    // PTR-NULL-DEREF around `if (p == NULL)` / `if (!p)` / `if (p != NULL)`:
    //  * in the branch where p is NULL (the regex lint's rule), reported at the if;
    //  * after the if, when that branch can fall out of it (it neither leaves
    //    the function / loop nor gives p a value): the first dereference of p
    //    in the following statements of the same block, reported there.
    void null_branch(const json& n, const json* parent, std::size_t idx) {
        if (lambda_depth) return;
        std::size_t ci = (flag(n, "hasInit") ? 1 : 0) + (flag(n, "hasVar") ? 1 : 0);
        const auto& cond = strip_all(child(n, ci));
        const json* var = nullptr;
        bool null_in_then = true;
        const auto& ck_ = kind(cond);
        if (ck_ == "UnaryOperator" && str_field(cond, "opcode") == "!") {
            var = &strip_all(child(cond, 0));
        } else if (ck_ == "BinaryOperator" && one_of(str_field(cond, "opcode"), {"==", "!="})) {
            null_in_then = str_field(cond, "opcode") == "==";
            if (is_null_ptr_expr(child(cond, 1))) var = &strip_all(child(cond, 0));
            else if (is_null_ptr_expr(child(cond, 0))) var = &strip_all(child(cond, 1));
        }
        if (!var || kind(*var) != "DeclRefExpr" || !is_pointer_type(type_of(*var))) return;
        const auto& id = str_field(ref_decl(*var), "id");
        if (!fn.locals.contains(id)) return;
        const json* branch = null_in_then ? &child(n, ci + 1) : &child(n, ci + 2);
        const bool has_branch = branch->is_object() && !branch->empty();
        Flow fl(ck);
        fl.st[id] = {St::Nulled, cx.line_of(begin_of(n)), {}};
        fl.null_if = &n;
        if (has_branch) {
            fl.stmt(*branch);
            if (fl.null_reported || fl.stopped || terminates(*branch)) return;
        } else if (null_in_then) {
            return;
        }
        // The NULL path leaves the if: follow it through the rest of the block.
        if (!parent || kind(*parent) != "CompoundStmt" || begin_of(n).macro) return;
        auto st = fl.st.find(id);
        if (st == fl.st.end() || st->second.st != St::Nulled) return;
        fl.null_after = true;
        for (std::size_t j = idx + 1; j < n_children(*parent) && !fl.stopped && !fl.null_reported; ++j)
            fl.stmt(child(*parent, j));
    }

    void int_div(const json& cast) {
        if (str_field(cast, "castKind") != "IntegralToFloating") return;
        const auto& d = strip_parens(child(cast, 0));
        if (kind(d) != "BinaryOperator" || str_field(d, "opcode") != "/") return;
        if (!is_int_type(type_of(d))) return;
        const auto& l = strip_all(child(d, 0));
        const auto& r = strip_all(child(d, 1));
        auto lv = int_value(l), rv = int_value(r);
        if (rv && *rv == 1) return;
        if (lv && rv && *rv != 0 && *lv % *rv == 0) return;  // exact constant quotient
        auto to = type_of(cast);
        auto ln = decl_name(l), rn = decl_name(r);
        std::string what = (ln.empty() ? std::string("...") : ln) + " / " + (rn.empty() ? std::string("...") : rn);
        ck.add(d, "INT-DIV-TO-FLOAT",
               "integer division " + what + " truncates before the result is converted to " +
                   (to.empty() ? std::string("floating point") : to));
    }

    static bool eligible_local(const json& v) {
        if (kind(v) != "VarDecl" || flag(v, "isImplicit")) return false;
        if (!str_field(v, "storageClass").empty()) return false;  // static / extern / register
        if (str_field(v, "name").empty()) return false;
        if (auto l = v.find("loc"); l == v.end() || loc_pos(*l).macro) return false;
        for (auto& c : inner(v))
            if (kind(c) == "UnusedAttr" || kind(c) == "CleanupAttr") return false;
        auto t = v.find("type");
        if (t == v.end()) return false;
        const auto& qt = str_field(*t, "qualType");
        if (qt.find("volatile") != std::string::npos || qt.find('&') != std::string::npos) return false;
        auto ty = type_of(v);
        return is_builtin_arith(ty) || is_pointer_type(ty) || ty.starts_with("enum ");
    }
};

std::string rx_strip(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

const std::unordered_set<std::string>& rx_signed_words() {
    static const std::unordered_set<std::string> w = {
        "int", "short", "long", "char", "signed", "ssize_t", "ptrdiff_t", "int8_t", "int16_t",
        "int32_t", "int64_t", "intmax_t", "intptr_t", "off_t", "pid_t",
    };
    return w;
}

const prism::Regex& rx_unsigned_ty() {
    static const prism::Regex re(R"(unsigned|size_t|uint|u_int|u_char|u_short|u_long)");
    return re;
}


std::unordered_set<std::string> rx_flex_tags(std::string_view text) {
    static prism::Regex def(R"(\b(?:struct|class)\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>[^{}]*)\})");
    static prism::Regex tail(R"([A-Za-z_]\w*\s*\[\s*(?:0\s*)?\]\s*$)");
    std::unordered_set<std::string> tags;
    for (auto& m : def.finditer(text)) {
        std::vector<std::string> decls;
        auto body = m.named("body");
        std::size_t p = 0;
        while (p <= body.size()) {
            auto c = body.find(';', p);
            auto part = rx_strip(c == std::string::npos ? body.substr(p) : body.substr(p, c - p));
            if (!part.empty()) decls.push_back(part);
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (!decls.empty() && tail.search(decls.back())) tags.insert(m.named("tag"));
    }
    return tags;
}

// ---------------------------------------------------------------- RES-FD-LEAK / CONC-TOCTOU / CONC-ATOMICITY
// AST ports of checkers_core.cpp fd_leak, conc_toctou and conc_atomicity.
// Same row line as the regex rule, so the AST row supersedes it; the facts
// come from the tree (calls, declarations, operators), not from text.

namespace {

struct FlowEv {
    long long off;
    const json* at;
};

bool same_file_pos(const Pos& a, const Pos& b) { return a.file && b.file && *a.file == *b.file; }

void collect_nodes(const json& n, const std::function<void(const json&, const json*)>& f, const json* if_cond = nullptr) {
    if (!n.is_object()) return;
    const auto& k = kind(n);
    if (k == "LambdaExpr" || k == "BlockExpr") return;  // another function body
    f(n, if_cond);
    if (k == "IfStmt" && n_children(n) >= 2) {
        // inner: [init?] [condvar?] cond then [else]; the condition is the first
        // expression child before the then statement.
        const auto& in = inner(n);
        const json* cond = nullptr;
        std::size_t then_i = 0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            const auto& ck = kind(in.at(i));
            if (ck == "DeclStmt" || ck == "VarDecl") continue;
            if (!cond) {
                cond = &in.at(i);
                then_i = i + 1;
                break;
            }
        }
        for (std::size_t i = 0; i < in.size(); ++i)
            collect_nodes(in.at(i), f, cond && i == then_i ? cond : nullptr);
        return;
    }
    // Statements directly in a `then` block are under the condition too.
    for (auto& c : inner(n)) collect_nodes(c, f, k == "CompoundStmt" ? if_cond : nullptr);
}

// `!v`, `v == NULL/0/-1`, `v < 0`, `(v = f()) == NULL`: the then branch runs
// with nothing open.
bool failure_test_of(const json& cond0, const std::string& id) {
    const auto& c = strip_all(cond0);
    const auto& k = kind(c);
    if (k == "UnaryOperator" && str_field(c, "opcode") == "!" && n_children(c) == 1)
        return ref_id(child(c, 0)) == id;
    if (k == "BinaryOperator" && n_children(c) == 2) {
        const auto op = str_field(c, "opcode");
        const auto& l = strip_all(child(c, 0));
        const auto& r = strip_all(child(c, 1));
        auto is_var = [&](const json& x) {
            if (ref_id(x) == id) return true;
            // (v = f()) == NULL
            return kind(x) == "BinaryOperator" && str_field(x, "opcode") == "=" && n_children(x) == 2 &&
                   ref_id(child(x, 0)) == id;
        };
        auto is_fail_const = [&](const json& x) {
            if (is_null_ptr_expr(x)) return true;
            auto v = int_value(strip_casts(x));
            return v && (*v == 0 || *v == -1);
        };
        if (op == "==" && ((is_var(l) && is_fail_const(r)) || (is_var(r) && is_fail_const(l)))) return true;
        if (op == "<" && is_var(l) && is_zero_literal(r)) return true;
        if (op == "||") return failure_test_of(l, id) || failure_test_of(r, id);
    }
    return false;
}

}  // namespace

// MEM-LEAK (malloc/calloc/realloc vs free) and RES-FD-LEAK (fopen/open vs
// fclose/close): at least two returns not under a failure test of the
// variable, and the object still held at some but not all of them.
void resource_leak_checks(const json& body, Checker& ck, Ctx& cx) {
    (void)cx;
    struct Var {
        std::string name;
        std::vector<long long> opens, closes;
    };
    struct Kind {
        const char* cls;
        std::initializer_list<std::string_view> open, close;
        const char* opened;
        const char* closed;
    };
    static const Kind kinds[] = {
        {"MEM-LEAK", {"malloc", "calloc", "realloc"}, {"free", "kfree", "ck_free"}, "allocated", "freed"},
        {"RES-FD-LEAK", {"fopen", "open"}, {"fclose", "close"}, "open", "closed"},
    };
    const auto base = begin_of(body);
    if (!base.file) return;
    std::vector<std::pair<FlowEv, const json*>> returns;  // return, enclosing if condition
    std::vector<std::map<std::string, Var>> vars(std::size(kinds));
    collect_nodes(body, [&](const json& n, const json* if_cond) {
        const auto& k = kind(n);
        const auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        if (k == "ReturnStmt") {
            returns.push_back({{p.offset, &n}, if_cond});
            return;
        }
        for (std::size_t ki = 0; ki < std::size(kinds); ++ki) {
            const auto& kd = kinds[ki];
            auto opener = [&](const json& e) {
                const auto& s = strip_all(e);
                return is_call_kind(kind(s)) && one_of(callee_name(s), kd.open);
            };
            if (k == "VarDecl" && n_children(n) >= 1 && opener(last_child(n))) {
                auto& v = vars[ki][str_field(n, "id")];
                v.name = str_field(n, "name");
                v.opens.push_back(p.offset);
            } else if (k == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2 &&
                       opener(child(n, 1))) {
                const auto& lhs = strip_all(child(n, 0));
                if (kind(lhs) == "DeclRefExpr") {
                    auto& v = vars[ki][str_field(ref_decl(lhs), "id")];
                    v.name = str_field(ref_decl(lhs), "name");
                    v.opens.push_back(p.offset);
                }
            } else if (is_call_kind(k) && one_of(callee_name(n), kd.close)) {
                auto a = call_args(n);
                if (a.size() == 1) {
                    auto id = ref_id(*a[0]);
                    if (!id.empty()) vars[ki][id].closes.push_back(p.offset);
                }
            }
        }
    });
    if (returns.size() < 2) return;
    auto max_before = [](const std::vector<long long>& xs, long long at) {
        long long m = -1;
        for (auto x : xs)
            if (x < at) m = std::max(m, x);
        return m;
    };
    for (std::size_t ki = 0; ki < std::size(kinds); ++ki) {
        for (auto& [id, v] : vars[ki]) {
            if (v.opens.empty() || v.closes.empty()) continue;
            std::vector<const FlowEv*> var_returns;
            for (auto& [r, cond] : returns)
                if (!(cond && failure_test_of(*cond, id))) var_returns.push_back(&r);
            if (var_returns.size() < 2) continue;
            std::vector<const FlowEv*> held;
            for (auto* r : var_returns)
                if (max_before(v.opens, r->off) > max_before(v.closes, r->off)) held.push_back(r);
            if (held.empty() || held.size() == var_returns.size()) continue;
            ck.add(*held.front()->at, kinds[ki].cls,
                   v.name + " " + kinds[ki].opened + " on " + std::to_string(v.opens.size()) + " path(s) but " +
                       kinds[ki].closed + " before only " + std::to_string(var_returns.size() - held.size()) +
                       " of " + std::to_string(var_returns.size()) + " returns");
        }
    }
}

// MEM-VLA-SIZE: an automatic array whose bound is not a constant. A
// constant bound is folded into the type (int[16] for int a[N]); a
// variable-length one keeps its expression (int[n]).
void vla_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    if (!base.file) return;
    collect_nodes(body, [&](const json& n, const json*) {
        if (kind(n) != "VarDecl") return;
        const auto sc = str_field(n, "storageClass");
        if (sc == "static" || sc == "extern") return;
        const auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        const auto t = qual_type(n);
        if (t.find('(') != std::string::npos) return;  // pointer to array / function types
        for (auto lb = t.find('['); lb != std::string::npos; lb = t.find('[', lb + 1)) {
            auto rb = t.find(']', lb);
            if (rb == std::string::npos) return;
            auto sz = strip_ws(t.substr(lb + 1, rb - lb - 1));
            if (sz.empty() || std::all_of(sz.begin(), sz.end(), [](unsigned char c) { return std::isdigit(c); }))
                continue;
            ck.add(n, "MEM-VLA-SIZE", str_field(n, "name") + "[" + sz + "] is a variable-length local array");
            return;
        }
    });
}

namespace {

// The leftmost operand of an `if` condition, through `!`: `if (v ...)`.
bool if_guards(const json& cond0, const std::string& id) {
    const json* c = &strip_all(cond0);
    while (true) {
        const auto& k = kind(*c);
        if (k == "DeclRefExpr") return ref_id(*c) == id;
        if ((k == "UnaryOperator" || k == "BinaryOperator") && n_children(*c) >= 1) {
            c = &strip_all(child(*c, 0));
            continue;
        }
        return false;
    }
}

bool word_in(std::string_view t, const std::unordered_set<std::string>& words) {
    static prism::Regex w(R"([A-Za-z_]\w*)");
    for (auto& m : w.finditer(t))
        if (words.contains(m.text)) return true;
    return false;
}

bool signed_int_type(std::string_view t) {
    if (t.find('*') != std::string_view::npos || t.find('[') != std::string_view::npos) return false;
    if (rx_unsigned_ty().search(t)) return false;
    return word_in(t, rx_signed_words());
}

}  // namespace

// MEM-PTR-ARITH: p[n] or *(p + n) with p a pointer parameter and n an
// integer parameter, unless both have an `if` guard in the function.
void ptr_arith_checks(const json& f, const json& body, Checker& ck) {
    static const std::unordered_set<std::string> ints = {"int", "long", "short", "unsigned", "size_t", "uint"};
    std::map<std::string, std::string> ptrs, nums;  // id -> name
    for (auto& c : inner(f)) {
        if (kind(c) != "ParmVarDecl" || str_field(c, "name").empty()) continue;
        auto t = std::string(qual_type(c));
        if (t.find('*') != std::string::npos || t.find('[') != std::string::npos) {
            ptrs[str_field(c, "id")] = str_field(c, "name");
        } else {
            auto lo = t;
            for (auto& ch : lo) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (word_in(lo, ints)) nums[str_field(c, "id")] = str_field(c, "name");
        }
    }
    if (ptrs.empty() || nums.empty()) return;
    const auto base = begin_of(body);
    std::set<std::string> guarded;
    std::vector<std::tuple<long long, const json*, std::string, std::string>> hits;  // off, node, p id, n id
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "IfStmt") {
            for (auto& c : inner(n)) {
                if (kind(c) == "DeclStmt" || kind(c) == "VarDecl") continue;
                for (auto& [id, _] : ptrs)
                    if (if_guards(c, id)) guarded.insert(id);
                for (auto& [id, _] : nums)
                    if (if_guards(c, id)) guarded.insert(id);
                break;
            }
            return;
        }
        const auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        std::string pid, nid;
        if (k == "ArraySubscriptExpr" && n_children(n) == 2) {
            pid = ref_id(child(n, 0));
            nid = ref_id(child(n, 1));
        } else if (k == "UnaryOperator" && str_field(n, "opcode") == "*" && n_children(n) == 1) {
            const auto& a = strip_all(child(n, 0));
            if (kind(a) == "BinaryOperator" && str_field(a, "opcode") == "+" && n_children(a) == 2) {
                pid = ref_id(child(a, 0));
                nid = ref_id(child(a, 1));
            }
        }
        if (ptrs.contains(pid) && nums.contains(nid)) hits.emplace_back(p.offset, &n, pid, nid);
    });
    std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });
    for (auto& [off, n, pid, nid] : hits) {
        if (guarded.contains(pid) && guarded.contains(nid)) continue;
        const auto& pn = ptrs[pid];
        const auto& nn = nums[nid];
        ck.add(*n, "MEM-PTR-ARITH", pn + "[" + nn + "] or *(" + pn + "+" + nn + ") without guard on " + pn + " or " + nn);
        return;
    }
}

// INFOLEAK-PAD: memcpy(dst, &s, sizeof ...) of a local struct that was
// neither initialised nor memset to zero.
void infoleak_pad_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    std::map<std::string, std::string> structs;  // id -> name
    std::set<std::string> zeroed;
    std::vector<std::pair<const json*, std::string>> copies;
    auto addr_of = [](const json& e) -> std::string {
        const auto& s = strip_all(e);
        if (kind(s) == "UnaryOperator" && str_field(s, "opcode") == "&" && n_children(s) == 1)
            return ref_id(child(s, 0));
        return ref_id(s);
    };
    auto has_sizeof = [](const json& e) {
        bool found = false;
        collect_nodes(e, [&](const json& x, const json*) {
            if (kind(x) == "UnaryExprOrTypeTraitExpr") found = true;
        });
        return found;
    };
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "VarDecl") {
            const auto sc = str_field(n, "storageClass");
            if (sc == "static" || sc == "extern" || n_children(n) > 0) return;
            auto t = std::string(qual_type(n));
            if (t.starts_with("struct ") && t.find('*') == std::string::npos && t.find('[') == std::string::npos)
                structs[str_field(n, "id")] = str_field(n, "name");
            return;
        }
        if (!is_call_kind(k)) return;
        auto cn = callee_name(n);
        auto a = call_args(n);
        if (cn == "memset" && a.size() == 3 && is_zero_literal(*a[1])) {
            auto id = addr_of(*a[0]);
            if (!id.empty()) zeroed.insert(id);
        } else if (cn == "memcpy" && a.size() == 3) {
            const auto& src = strip_all(*a[1]);
            if (kind(src) != "UnaryOperator" || str_field(src, "opcode") != "&" || !has_sizeof(*a[2])) return;
            auto p = begin_of(n);
            if (!same_file_pos(p, base) || p.macro) return;
            auto id = ref_id(child(src, 0));
            if (!id.empty()) copies.emplace_back(&n, id);
        }
    });
    std::set<std::string> seen;
    for (auto& [n, id] : copies) {
        if (!structs.contains(id) || zeroed.contains(id) || !seen.insert(id).second) continue;
        ck.add(*n, "INFOLEAK-PAD", "memcpy copies local struct " + structs[id] + " without zeroing padding first");
    }
}

// MEM-FLEX-ARRAY: malloc/realloc/calloc sized by a bare sizeof of a struct
// that ends in a flexible array member (or of *p, p pointing to one).
void flex_array_checks(const json& body, Checker& ck, Ctx& cx) {
    const auto base = begin_of(body);
    auto* fc = cx.file(base.file);
    if (!fc) return;
    std::string text;
    for (auto& l : fc->lines) text += l + "\n";
    auto tags = rx_flex_tags(text);
    if (tags.empty()) return;
    static prism::Regex type_form(R"(^sizeof\s*\(\s*(?:struct|class)\s+([A-Za-z_]\w*)\s*\))");
    static prism::Regex tag_of(R"(^(?:const\s+|volatile\s+)*(?:struct|class)\s+([A-Za-z_]\w*)$)");
    collect_nodes(body, [&](const json& n, const json*) {
        if (!is_call_kind(kind(n))) return;
        auto cn = callee_name(n);
        auto a = call_args(n);
        const json* size = nullptr;
        if (cn == "malloc" && a.size() >= 1) size = a[0];
        else if ((cn == "realloc" || cn == "calloc") && a.size() >= 2) size = a[1];
        if (!size) return;
        auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        const auto& sz = strip_all(*size);
        if (kind(sz) != "UnaryExprOrTypeTraitExpr") return;
        std::string tag;
        if (n_children(sz) >= 1) {
            auto t = std::string(qual_type(strip_all(child(sz, 0))));
            if (auto m = tag_of.search_match(t)) tag = m->group(1);
        }
        if (tag.empty())
            if (auto m = tag_of.search_match(sz.contains("argType") ? str_field(sz["argType"], "qualType") : std::string())) tag = m->group(1);
        if (tag.empty())
            if (auto m = type_form.search_match(strip_ws(std::string(cx.text_of(sz))))) tag = m->group(1);
        if (tag.empty() || !tags.contains(tag)) return;
        ck.add(n, "MEM-FLEX-ARRAY", cn + "() sizes struct " + tag + " at the header; flexible array needs extra bytes");
    });
}

// MEM-ONESIDED-INDEX: `if (i > n)` / `if (i >= n)` on a signed i that also
// indexes an array, with no floor test (i < 0, i >= 0, -1 compares, an
// unsigned cast) and no constant or loop-init assignment.
void onesided_index_checks(const json& f, const json& body, Checker& ck) {
    std::map<std::string, std::string> signed_vars;
    for (auto& c : inner(f))
        if (kind(c) == "ParmVarDecl" && !str_field(c, "name").empty() && signed_int_type(qual_type(c)))
            signed_vars[str_field(c, "id")] = str_field(c, "name");
    std::set<std::string> floored, pinned, indexed;
    std::vector<std::pair<const json*, std::string>> tests;
    auto neg1 = [](const json& e) {
        auto v = int_value(e);
        return v && (*v == 0 || *v == -1);
    };
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "VarDecl" && signed_int_type(qual_type(n))) {
            if (n_children(n) == 0) signed_vars[str_field(n, "id")] = str_field(n, "name");
            else if (int_value(last_child(n))) pinned.insert(str_field(n, "id"));
            if (n_children(n) > 0) signed_vars[str_field(n, "id")] = str_field(n, "name");
        } else if (k == "ForStmt" && n_children(n) >= 1) {
            const auto& init = child(n, 0);
            collect_nodes(init, [&](const json& x, const json*) {
                if (kind(x) == "VarDecl") pinned.insert(str_field(x, "id"));
                if (kind(x) == "BinaryOperator" && str_field(x, "opcode") == "=")
                    if (auto id = ref_id(child(x, 0)); !id.empty()) pinned.insert(id);
            });
        } else if (k == "BinaryOperator" && n_children(n) == 2) {
            const auto op = str_field(n, "opcode");
            const auto& l = child(n, 0);
            const auto& r = child(n, 1);
            if (op == "=" && int_value(r) && *int_value(r) >= 0) {
                if (auto id = ref_id(l); !id.empty()) pinned.insert(id);
            } else if (one_of(op, {"<", ">=", ">", "==", "!=", "<="})) {
                if (auto id = ref_id(l); !id.empty() && neg1(r)) floored.insert(id);
                if (auto id = ref_id(r); !id.empty() && neg1(l)) floored.insert(id);
            }
        } else if (is_explicit_cast(k) && n_children(n) >= 1) {
            auto t = std::string(qual_type(n));
            if (rx_unsigned_ty().search(t))
                if (auto id = ref_id(child(n, 0)); !id.empty()) floored.insert(id);
        } else if (k == "ArraySubscriptExpr" && n_children(n) == 2) {
            if (auto id = ref_id(child(n, 1)); !id.empty()) indexed.insert(id);
        } else if (k == "IfStmt") {
            for (auto& c : inner(n)) {
                if (kind(c) == "DeclStmt" || kind(c) == "VarDecl") continue;
                const auto& s = strip_all(c);
                if (kind(s) == "BinaryOperator" && one_of(str_field(s, "opcode"), {">", ">="}) &&
                    kind(strip_all(child(s, 1))) == "DeclRefExpr")
                    if (auto id = ref_id(child(s, 0)); !id.empty()) tests.emplace_back(&n, id);
                break;
            }
        }
    });
    std::set<std::string> seen;
    for (auto& [n, id] : tests) {
        if (!signed_vars.contains(id) || floored.contains(id) || pinned.contains(id) || !indexed.contains(id)) continue;
        if (!seen.insert(id).second) continue;
        const auto& nm = signed_vars[id];
        ck.add(*n, "MEM-ONESIDED-INDEX", nm + " is a signed index bounded above only; no " + nm + " < 0 test");
    }
}

namespace {

enum class AUse { Ptr, PtrOrArg, PtrOrGetenv, PtrOrArgOrGetenv, Fd, FdOrVoidCast };
enum class AGuard { Null, NullCheck, LtZero, MapFailed, Mq };

struct AssignRule {
    const char* cls;
    std::initializer_list<std::string_view> fnames, skip;
    AUse use;
    AGuard guard;
};

// The api_assign_unguarded rules of checkers_api.cpp, as a table.
const AssignRule kAssignRules[] = {
    {"API-MMAP", {"mmap"}, {}, AUse::Ptr, AGuard::MapFailed},
    {"API-GETCWD", {"getcwd"}, {}, AUse::PtrOrGetenv, AGuard::Null},
    {"API-DLOPEN", {"dlopen"}, {"dlopen"}, AUse::PtrOrArg, AGuard::Null},
    {"API-ACCEPT", {"accept4", "accept"}, {"accept", "accept4"}, AUse::Fd, AGuard::LtZero},
    {"API-REALPATH", {"realpath"}, {"realpath"}, AUse::PtrOrArgOrGetenv, AGuard::Null},
    {"API-SOCKET", {"socket"}, {"accept", "accept4"}, AUse::Fd, AGuard::LtZero},
    {"API-DUP", {"dup3", "dup2", "dup"}, {"dup", "dup2", "dup3", "accept", "accept4"}, AUse::Fd, AGuard::LtZero},
    {"API-OPENAT", {"openat"}, {"openat"}, AUse::Fd, AGuard::LtZero},
    {"API-OPENDIR", {"opendir"}, {"opendir"}, AUse::PtrOrArg, AGuard::Null},
    {"API-GETPWUID", {"getpwnam", "getpwuid"}, {"getpwuid", "getpwnam"}, AUse::PtrOrArg, AGuard::Null},
    {"API-SHM-OPEN", {"shm_open"}, {"shm_open"}, AUse::Fd, AGuard::LtZero},
    {"API-MEMFD", {"memfd_create"}, {"memfd_create"}, AUse::Fd, AGuard::LtZero},
    {"API-GETLOGIN", {"ttyname", "getlogin"}, {"getlogin", "ttyname"}, AUse::PtrOrArg, AGuard::Null},
    {"API-INOTIFY", {"inotify_init1", "inotify_init"}, {"inotify_init", "inotify_init1"}, AUse::Fd, AGuard::LtZero},
    {"API-PTSNAME", {"ptsname"}, {"ptsname"}, AUse::PtrOrArg, AGuard::Null},
    {"API-FMEMOPEN", {"open_memstream", "fmemopen"}, {"fmemopen", "open_memstream"}, AUse::PtrOrArg, AGuard::Null},
    {"API-PIDFD", {"pidfd_open"}, {"pidfd_open"}, AUse::Fd, AGuard::LtZero},
    {"API-FANOTIFY", {"fanotify_init"}, {"fanotify_init"}, AUse::Fd, AGuard::LtZero},
    {"API-GETGRNAM", {"getspnam", "getgrgid", "getgrnam"}, {"getgrnam", "getgrgid", "getspnam"}, AUse::PtrOrArg, AGuard::Null},
    {"API-USERFAULTFD", {"userfaultfd"}, {"userfaultfd"}, AUse::Fd, AGuard::LtZero},
    {"API-GETPASS", {"getpass"}, {"getpass"}, AUse::PtrOrArg, AGuard::Null},
    {"API-OPENAT2", {"openat2"}, {"openat2"}, AUse::Fd, AGuard::LtZero},
    {"API-LANDLOCK", {"landlock_create_ruleset"}, {"landlock_create_ruleset"}, AUse::Fd, AGuard::LtZero},
    {"API-SIGNALFD", {"signalfd"}, {"signalfd"}, AUse::Fd, AGuard::LtZero},
    {"API-PERF-EVENT", {"perf_event_open"}, {"perf_event_open"}, AUse::Fd, AGuard::LtZero},
    {"API-PKEY", {"pkey_alloc"}, {"pkey_alloc"}, AUse::Fd, AGuard::LtZero},
    {"API-FSOPEN", {"fsopen"}, {"fsopen"}, AUse::Fd, AGuard::LtZero},
    {"API-MQ-OPEN", {"mq_open"}, {"mq_open"}, AUse::FdOrVoidCast, AGuard::Mq},
    {"API-MEMFD-SECRET", {"memfd_secret"}, {"memfd_secret"}, AUse::Fd, AGuard::LtZero},
    {"API-KQUEUE", {"kqueue"}, {"kqueue"}, AUse::Fd, AGuard::LtZero},
    {"API-SHMGET", {"shmget"}, {"shmget"}, AUse::Fd, AGuard::LtZero},
    {"API-SEMGET", {"semget"}, {"semget"}, AUse::Fd, AGuard::LtZero},
    {"API-MSGGET", {"msgget"}, {"msgget"}, AUse::Fd, AGuard::LtZero},
    {"API-REALLOCARRAY", {"reallocarray"}, {"reallocarray"}, AUse::PtrOrArg, AGuard::NullCheck},
    {"API-REALLOCF", {"reallocf"}, {"reallocf"}, AUse::PtrOrArg, AGuard::NullCheck},
    {"API-VALLOC", {"valloc"}, {"valloc"}, AUse::PtrOrArg, AGuard::NullCheck},
};

std::string assign_msg(const std::string& var, std::string_view f, AGuard g) {
    std::string base = var + " from " + std::string(f) + "() used without a ";
    switch (g) {
        case AGuard::Null: return base + "NULL test";
        case AGuard::NullCheck: return base + "NULL check";
        case AGuard::LtZero: return base + "< 0 test";
        case AGuard::MapFailed: return base + "MAP_FAILED test";
        case AGuard::Mq: return base + "(mqd_t)-1 / < 0 test";
    }
    return {};
}

// Walks expressions, not into sizeof/alignof (unevaluated).
void walk_eval(const json& n, const std::function<void(const json&)>& f) {
    if (!n.is_object()) return;
    const auto& k = kind(n);
    if (k == "UnaryExprOrTypeTraitExpr" || k == "LambdaExpr" || k == "BlockExpr") return;
    f(n);
    for (auto& c : inner(n)) walk_eval(c, f);
}

}  // namespace

// API-* "x = f(...); then x used before any failure test" rules: the
// regex api_assign_unguarded table on AST nodes.
void api_assign_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    if (!base.file) return;
    struct Open {
        const AssignRule* rule;
        std::string id, name, callee;
        const json* at;
        long long b, e;
    };
    std::vector<Open> opens;
    struct Ev {
        long long b, e;
        std::string id;
        int what;  // 0 deref, 1 call arg (callee), 2 return, 3 arith, 4 void cast, 5 getenv-style arg
        std::string callee;
        AGuard g{};
        bool guard = false;
        std::vector<AGuard> kinds;
    };
    std::vector<Ev> evs;
    auto is_var = [](const json& x) -> std::string {
        const auto& s = strip_casts(x);
        if (auto id = ref_id(s); !id.empty()) return id;
        if (kind(s) == "BinaryOperator" && str_field(s, "opcode") == "=" && n_children(s) == 2)
            return ref_id(child(s, 0));
        return {};
    };
    auto neg1 = [](const json& x) {
        auto v = int_value(strip_casts(x));
        return v && *v == -1;
    };
    auto zero = [](const json& x) { return is_null_ptr_expr(x); };
    auto guard_ev = [&](const json& n, const std::string& id, std::vector<AGuard> kinds) {
        auto b = begin_of(n), e = end_of(n);
        Ev ev{b.offset, e.offset, id, -1, {}, {}, true, std::move(kinds)};
        evs.push_back(std::move(ev));
    };
    static const std::set<std::string> getenv_like = {"strlen", "strcpy", "strcat", "strcmp", "atoi", "atol", "atoll",
                                                      "printf", "fprintf", "sprintf", "snprintf", "warn", "err", "syslog"};
    walk_eval(body, [&](const json& n) {
        const auto& k = kind(n);
        const auto p = begin_of(n);
        if (!same_file_pos(p, base)) return;
        // Openers.
        auto opener = [&](const json& init, const std::string& id, const std::string& name, const json& at) {
            const auto& c = strip_casts(init);
            if (!is_call_kind(kind(c))) return;
            auto cn = callee_name(c);
            for (auto& r : kAssignRules)
                if (one_of(cn, r.fnames)) {
                    opens.push_back({&r, id, name, cn, &at, begin_of(at).offset, end_of(c).offset});
                    return;
                }
        };
        if (k == "VarDecl" && n_children(n) >= 1 && !p.macro) opener(last_child(n), str_field(n, "id"), str_field(n, "name"), n);
        if (k == "BinaryOperator" && n_children(n) == 2) {
            const auto op = str_field(n, "opcode");
            const auto& l = child(n, 0);
            const auto& r = child(n, 1);
            if (op == "=") {
                const auto& ls = strip_all(l);
                if (kind(ls) == "DeclRefExpr" && !p.macro) opener(r, ref_id(ls), decl_name(ls), n);
                return;
            }
            if (op == "==" || op == "!=") {
                for (auto [a, b] : {std::pair{&l, &r}, std::pair{&r, &l}}) {
                    auto id = is_var(*a);
                    if (id.empty()) continue;
                    std::vector<AGuard> g;
                    if (zero(*b)) g = {AGuard::Null, AGuard::NullCheck};
                    if (neg1(*b)) g = {AGuard::LtZero, AGuard::MapFailed, AGuard::Mq};
                    if (!g.empty()) guard_ev(n, id, g);
                }
            } else if (op == "<" || op == ">=" || op == "<=" || op == ">") {
                auto id = is_var(l);
                auto v = int_value(strip_casts(r));
                if (!id.empty() && v && ((op == "<" && *v == 0) || (op == ">=" && *v == 0) || (op == "<=" && *v == -1) ||
                                         (op == ">" && *v == -1)))
                    guard_ev(n, id, {AGuard::LtZero, AGuard::Mq});
            } else if (one_of(op, {"+", "-", "*", "/", "%"})) {
                for (auto* x : {&l, &r})
                    if (auto id = ref_id(*x); !id.empty()) evs.push_back({p.offset, p.offset, id, 3, {}, {}, false, {}});
            }
            return;
        }
        if (k == "UnaryOperator" && n_children(n) == 1) {
            const auto op = str_field(n, "opcode");
            auto id = ref_id(child(n, 0));
            if (id.empty()) return;
            if (op == "!") guard_ev(n, id, {AGuard::Null, AGuard::NullCheck});
            else if (op == "*") evs.push_back({p.offset, p.offset, id, 0, {}, {}, false, {}});
            return;
        }
        if (k == "IfStmt" || k == "WhileStmt" || k == "ConditionalOperator") {
            for (auto& c : inner(n)) {
                if (kind(c) == "DeclStmt" || kind(c) == "VarDecl") continue;
                if (auto id = ref_id(c); !id.empty()) guard_ev(c, id, {AGuard::Null, AGuard::NullCheck});
                break;
            }
            return;
        }
        if (k == "ArraySubscriptExpr" && n_children(n) == 2) {
            if (auto id = ref_id(child(n, 0)); !id.empty()) evs.push_back({p.offset, p.offset, id, 0, {}, {}, false, {}});
            return;
        }
        if (k == "MemberExpr" && flag(n, "isArrow") && n_children(n) >= 1) {
            if (auto id = ref_id(child(n, 0)); !id.empty()) evs.push_back({p.offset, p.offset, id, 0, {}, {}, false, {}});
            return;
        }
        if (k == "ReturnStmt" && n_children(n) == 1) {
            if (auto id = ref_id(child(n, 0)); !id.empty()) evs.push_back({p.offset, p.offset, id, 2, {}, {}, false, {}});
            return;
        }
        if (k == "CStyleCastExpr" && n_children(n) >= 1 && qual_type(n) == "void") {
            if (auto id = ref_id(child(n, 0)); !id.empty()) evs.push_back({p.offset, p.offset, id, 4, {}, {}, false, {}});
            return;
        }
        if (is_call_kind(k)) {
            auto cn = callee_name(n);
            if (cn.starts_with("assert")) {
                for (auto* a : call_args(n))
                    if (auto id = is_var(*a); !id.empty()) guard_ev(n, id, {AGuard::Null, AGuard::NullCheck});
                return;
            }
            for (auto* a : call_args(n))
                if (auto id = ref_id(strip_casts(*a)); !id.empty())
                    evs.push_back({begin_of(*a).offset, begin_of(*a).offset, id, getenv_like.contains(cn) ? 5 : 1, cn, {}, false, {}});
        }
    });
    std::set<std::pair<std::string, long long>> seen;
    for (auto& o : opens) {
        const auto& r = *o.rule;
        long long use = -1;
        for (auto& e : evs) {
            if (e.guard || e.id != o.id || e.b <= o.e) continue;
            bool hit = false;
            switch (r.use) {
                case AUse::Ptr: hit = e.what == 0; break;
                case AUse::PtrOrArg: hit = e.what == 0 || ((e.what == 1 || e.what == 5) && !one_of(e.callee, r.skip)); break;
                case AUse::PtrOrGetenv: hit = e.what == 0 || e.what == 5; break;
                case AUse::PtrOrArgOrGetenv:
                    hit = e.what == 0 || e.what == 5 || (e.what == 1 && !one_of(e.callee, r.skip));
                    break;
                case AUse::Fd:
                case AUse::FdOrVoidCast:
                    hit = ((e.what == 1 || e.what == 5) && !one_of(e.callee, r.skip)) || e.what == 2 || e.what == 3 ||
                          (r.use == AUse::FdOrVoidCast && e.what == 4);
                    break;
            }
            if (hit && (use < 0 || e.b < use)) use = e.b;
        }
        if (use < 0) continue;
        bool guarded = false;
        for (auto& e : evs) {
            if (!e.guard || e.id != o.id) continue;
            if (std::find(e.kinds.begin(), e.kinds.end(), r.guard) == e.kinds.end()) continue;
            if (e.e >= o.b && e.b <= use) guarded = true;
        }
        if (guarded || !seen.insert({o.id, o.b}).second) continue;
        ck.add(*o.at, r.cls, assign_msg(o.name, o.callee, r.guard));
    }
}

// MEM-CAPACITY-FIRST: a capacity field is grown (+=, *=, <<=, ++) before
// the allocation that is supposed to earn it, and the allocation's failure
// arm leaves without restoring the field.
std::string member_chain(const json& e) {
    const auto& s = strip_all(strip_parens(e));
    if (kind(s) == "MemberExpr" && n_children(s) == 1) {
        auto base = member_chain(child(s, 0));
        if (base.empty()) return {};
        return base + (s.value("isArrow", false) ? "->" : ".") + str_field(s, "name");
    }
    if (kind(s) == "DeclRefExpr") return std::string(str_field(ref_decl(s), "name"));
    return {};
}

void capacity_first_checks(const json& body, Checker& ck, Ctx& cx) {
    static const std::set<std::string> allocs{"realloc", "reallocarray", "malloc", "calloc",
                                              "mallocarray", "memcpy", "memmove"};
    static const std::set<std::string> dead{"err", "errx", "xo_err", "xo_errx", "abort", "exit", "panic",
                                            "_errx", "out_of_mem", "AbortProgram", "fatal", "bsdar_errc"};
    static Regex nullt(R"(==\s*(?:NULL|nullptr|0)\b|(?:NULL|nullptr)\s*==|!\s*[A-Za-z_])");
    const auto base = begin_of(body);
    auto grown = [](const json& st) -> std::string {
        const auto& e = strip_all(st);
        const auto& k = kind(e);
        if (k == "CompoundAssignOperator") {
            auto op = str_field(e, "opcode");
            if ((op == "+=" || op == "*=" || op == "<<=") && n_children(e) == 2) {
                const auto& l = strip_all(child(e, 0));
                if (kind(l) == "MemberExpr") return member_chain(l);
            }
        } else if (k == "UnaryOperator") {
            auto op = str_field(e, "opcode");
            if ((op == "++") && n_children(e) == 1 && kind(strip_all(child(e, 0))) == "MemberExpr")
                return member_chain(child(e, 0));
        }
        return {};
    };
    auto mentions_chain = [&](const json& n, const std::string& var) {
        bool hit = false;
        collect_nodes(n, [&](const json& x, const json*) {
            if (!hit && kind(x) == "MemberExpr" && member_chain(x) == var) hit = true;
        });
        return hit;
    };
    auto alloc_with = [&](const json& n, const std::string& var) {
        bool hit = false;
        collect_nodes(n, [&](const json& x, const json*) {
            if (hit || !is_call_kind(kind(x)) || !allocs.contains(std::string(callee_name(x)))) return;
            if (mentions_chain(x, var)) hit = true;
        });
        return hit;
    };
    auto leaves = [](const json& n) {
        bool hit = false;
        collect_nodes(n, [&](const json& x, const json*) {
            auto k = kind(x);
            if (k == "ReturnStmt" || k == "GotoStmt" || k == "BreakStmt" || k == "ContinueStmt") hit = true;
        });
        return hit;
    };
    auto calls_dead_or_release = [&](const json& n, const std::string& var) {
        bool hit = false;
        auto dot = var.find_first_of(".-");
        std::string owner = dot == std::string::npos ? std::string() : var.substr(0, dot);
        collect_nodes(n, [&](const json& x, const json*) {
            if (hit || !is_call_kind(kind(x))) return;
            auto cn = std::string(callee_name(x));
            if (dead.contains(cn)) hit = true;
            std::string low = cn;
            for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!owner.empty() && (low.find("free") != std::string::npos || low.find("destroy") != std::string::npos)) {
                auto a = call_args(x);
                if (a.size() == 1 && member_chain(*a[0]) == owner) hit = true;
            }
        });
        return hit;
    };
    std::set<std::string> reported;
    collect_nodes(body, [&](const json& n, const json*) {
        if (kind(n) != "CompoundStmt") return;
        const auto& st = inner(n);
        for (std::size_t i = 0; i < st.size(); ++i) {
            auto var = grown(st.at(i));
            if (var.empty()) continue;
            const auto p = begin_of(st.at(i));
            if (!same_file_pos(p, base) || p.macro) continue;
            std::optional<std::size_t> aidx;
            for (std::size_t j = i + 1; j < std::min(i + 6, st.size()); ++j)
                if (alloc_with(st.at(j), var)) {
                    aidx = j;
                    break;
                }
            if (!aidx) continue;
            const json* arm = nullptr;
            for (std::size_t j = *aidx; j < std::min(*aidx + 7, st.size()); ++j) {
                const auto& s2 = st.at(j);
                if (kind(s2) != "IfStmt" || n_children(s2) < 2) continue;
                const json* cond = nullptr;
                for (auto& c : inner(s2)) {
                    if (kind(c) == "DeclStmt" || kind(c) == "VarDecl") continue;
                    cond = &c;
                    break;
                }
                if (!cond || !nullt.search(std::string(cx.text_of(*cond)) + " ")) {
                    // libclang text can run one token long; also accept a ! operator node.
                    bool neg = false;
                    if (cond)
                        collect_nodes(*cond, [&](const json& x, const json*) {
                            if (kind(x) == "UnaryOperator" && str_field(x, "opcode") == "!") neg = true;
                            if (kind(x) == "BinaryOperator" && str_field(x, "opcode") == "==" && n_children(x) == 2 &&
                                (is_null_ptr_expr(child(x, 0)) || is_null_ptr_expr(child(x, 1))))
                                neg = true;
                        });
                    if (!neg) continue;
                }
                if (!leaves(s2)) continue;
                arm = &s2;
                break;
            }
            if (!arm) continue;
            if (calls_dead_or_release(*arm, var)) continue;
            // The arm restores or mentions the field.
            bool restores = false;
            {
                bool skip_cond = true;
                for (auto& c : inner(*arm)) {
                    if (skip_cond && kind(c) != "DeclStmt" && kind(c) != "VarDecl") {
                        skip_cond = false;
                        continue;
                    }
                    if (mentions_chain(c, var)) restores = true;
                }
            }
            if (restores || !reported.insert(var).second) continue;
            ck.add(st.at(i), "MEM-CAPACITY-FIRST", var + " grown before the allocation that is supposed to earn it");
        }
    });
}

// CXX index-token family (regex cxx_index_token_scan): the result of a
// numeric std function used as an array index without a range check, either
// directly (`a[std::gcd(x, y)]`) or through a local it was assigned to.
struct IndexTokenRule {
    const char* cls;
    std::vector<std::string> fns;
    const char* what;
};

const std::vector<IndexTokenRule>& index_token_rules() {
    static const std::vector<IndexTokenRule> r{
        {"CXX-BYTESWAP", {"byteswap"}, "CXX-BYTESWAP"},
        {"CXX-ROTL", {"rotl", "rotr"}, "std::rotl/rotr"},
        {"CXX-BIT-WIDTH", {"bit_width"}, "CXX-BIT-WIDTH"},
        {"CXX-MIDPOINT", {"midpoint"}, "CXX-MIDPOINT"},
        {"CXX-COUNTL-ZERO", {"countl_zero", "countl_one", "countr_zero", "countr_one"}, "CXX-COUNTL-ZERO"},
        {"CXX-GCD", {"gcd"}, "CXX-GCD"},
        {"CXX-LCM", {"lcm"}, "CXX-LCM"},
        {"CXX-CLAMP", {"clamp"}, "CXX-CLAMP"},
        {"CXX-TRANSFORM-REDUCE", {"transform_reduce"}, "CXX-TRANSFORM-REDUCE"},
        {"CXX-REDUCE", {"reduce"}, "CXX-REDUCE"},
        {"CXX-ADD-SAT", {"add_sat", "sub_sat", "mul_sat", "div_sat", "saturate_cast"}, "CXX-ADD-SAT"},
    };
    return r;
}

void index_token_checks(const json& body, Checker& ck, Ctx& cx) {
    const auto base = begin_of(body);
    auto rule_of = [](const json& e) -> const IndexTokenRule* {
        const auto& c = strip_all(strip_parens(e));
        if (!is_call_kind(kind(c))) return nullptr;
        auto cn = callee_name(c);
        for (auto& r : index_token_rules())
            if (std::find(r.fns.begin(), r.fns.end(), cn) != r.fns.end()) return &r;
        return nullptr;
    };
    std::map<std::string, const IndexTokenRule*> tainted;  // var id -> rule
    std::set<std::string> bounded;                         // var ids compared with < <= > >=
    std::vector<const json*> subs;
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "VarDecl" && n_children(n) > 0) {
            if (auto* r = rule_of(child(n, n_children(n) - 1))) tainted[std::string(str_field(n, "id"))] = r;
        } else if (k == "BinaryOperator" && n_children(n) == 2) {
            auto op = str_field(n, "opcode");
            if (op == "=") {
                auto id = ref_id(child(n, 0));
                if (auto* r = rule_of(child(n, 1)); r && !id.empty()) tainted[id] = r;
            } else if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                for (int i = 0; i < 2; ++i)
                    if (auto id = ref_id(child(n, i)); !id.empty()) bounded.insert(id);
            }
        } else if (k == "ArraySubscriptExpr" && n_children(n) == 2) {
            subs.push_back(&n);
        } else if (k == "CXXOperatorCallExpr" && n_children(n) == 3) {
            const auto& op = strip_all(child(n, 0));
            if (kind(op) == "DeclRefExpr" && str_field(ref_decl(op), "name") == "operator[]") subs.push_back(&n);
        }
    });
    std::sort(subs.begin(), subs.end(), [](auto* a, auto* b) { return begin_of(*a).offset < begin_of(*b).offset; });
    std::set<std::string> reported;
    for (auto* n : subs) {
        const auto p = begin_of(*n);
        if (!same_file_pos(p, base) || p.macro) continue;
        const bool op_call = kind(*n) == "CXXOperatorCallExpr";
        const json& cont = child(*n, op_call ? 1 : 0);
        const json& idx = child(*n, op_call ? 2 : 1);
        const IndexTokenRule* r = rule_of(idx);
        std::string what;
        if (!r) {
            auto id = ref_id(idx);
            auto it = tainted.find(id);
            if (id.empty() || it == tainted.end() || bounded.contains(id)) continue;
            r = it->second;
            what = std::string(str_field(ref_decl(strip_all(idx)), "name"));
        }
        if (!reported.insert(r->cls).second) continue;
        std::string cname = member_chain(cont);
        if (cname.empty()) cname = "container";
        std::string msg = what.empty()
                              ? std::string(r->what) + " result used as an array index without a range check"
                              : cname + "[" + what + "] indexes with " + r->what + " without a range check";
        (void)cx;
        ck.add(*n, r->cls, msg);
    }
}

// CXX container-method family (regex cxx_find_no_end, cxx_at_no_contains,
// cxx_method_no_empty): a risky member call on a local or parameter of a
// standard container type when the function never makes the matching check
// on that same object.
struct ContainerRule {
    const char* cls;
    std::vector<std::string> types;   // template names
    std::vector<std::string> risky;   // methods that need the check
    std::vector<std::string> checks;  // methods that count as the check
    const char* msg;
};

const std::vector<ContainerRule>& container_rules() {
    static const std::vector<std::string> fin{"find"}, endc{"end"}, at{"at"}, has{"contains", "count", "find"},
        emp{"empty"};
    static const char* fmsg = ".find() dereferenced without != end()";
    static const std::vector<ContainerRule> r{
        {"CXX-FLAT-SET", {"flat_set"}, fin, endc, fmsg},
        {"CXX-FLAT-MULTIMAP", {"flat_multimap"}, fin, endc, fmsg},
        {"CXX-FLAT-MULTISET", {"flat_multiset"}, fin, endc, fmsg},
        {"CXX-SET-FIND", {"set"}, fin, endc, fmsg},
        {"CXX-UNORDERED-SET", {"unordered_set"}, fin, endc, fmsg},
        {"CXX-MULTIMAP-FIND", {"multimap"}, fin, endc, fmsg},
        {"CXX-MULTISET-FIND", {"multiset"}, fin, endc, fmsg},
        {"CXX-UNORDERED-MULTIMAP", {"unordered_multimap"}, fin, endc, fmsg},
        {"CXX-UNORDERED-MULTISET", {"unordered_multiset"}, fin, endc, fmsg},
        {"CXX-MAP-AT", {"map"}, at, has, ".at() without count()/find()/contains()"},
        {"CXX-UNORDERED-AT", {"unordered_map"}, at, has, ".at() without count()/find()/contains()"},
        {"CXX-FORWARD-LIST", {"forward_list"}, {"front"}, emp, ".front() without empty()"},
        {"CXX-LIST-FRONT", {"list"}, {"front", "back"}, emp, ".front()/.back() without empty()"},
        {"CXX-PRIORITY-QUEUE", {"priority_queue"}, {"top", "pop"}, emp, ".top()/.pop() without empty()"},
    };
    return r;
}

// "const std::set<int> &" -> "set"
std::string template_name(std::string t) {
    auto lt = t.find('<');
    if (lt == std::string::npos) return {};
    t = t.substr(0, lt);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    auto c = t.find_last_of(": ");
    return c == std::string::npos ? t : t.substr(c + 1);
}

void container_method_checks(const json& body, const Fn& fn, Checker& ck) {
    const auto base = begin_of(body);
    struct Call {
        const json* at;
        std::string var, method;
    };
    std::vector<Call> calls;
    std::map<std::string, std::string> tname;  // var id -> template name
    for (auto& [id, d] : fn.locals) tname[id] = template_name(std::string(qual_type(*d)));
    collect_nodes(body, [&](const json& n, const json*) {
        if (kind(n) == "VarDecl") {
            tname[std::string(str_field(n, "id"))] = template_name(std::string(qual_type(n)));
            return;
        }
        if (kind(n) != "CXXMemberCallExpr" || n_children(n) == 0) return;
        const auto& me = strip_all(child(n, 0));
        if (kind(me) != "MemberExpr" || n_children(me) != 1) return;
        auto id = ref_id(child(me, 0));
        if (id.empty()) return;
        calls.push_back({&n, id, std::string(str_field(me, "name"))});
    });
    for (auto& r : container_rules()) {
        std::set<std::string> checked;
        for (auto& c : calls)
            if (std::find(r.checks.begin(), r.checks.end(), c.method) != r.checks.end()) checked.insert(c.var);
        for (auto& c : calls) {
            if (std::find(r.risky.begin(), r.risky.end(), c.method) == r.risky.end()) continue;
            auto it = tname.find(c.var);
            if (it == tname.end() || std::find(r.types.begin(), r.types.end(), it->second) == r.types.end()) continue;
            if (checked.contains(c.var)) continue;
            const auto p = begin_of(*c.at);
            if (!same_file_pos(p, base) || p.macro) continue;
            const auto& me = strip_all(child(*c.at, 0));
            auto name = std::string(str_field(ref_decl(strip_all(child(me, 0))), "name"));
            ck.add(*c.at, r.cls, name + r.msg);
            break;
        }
    }
}

// MEM-REALLOC-ZERO: after realloc(p, 0) (not `p = realloc(p, 0)`), the
// first later index, deref, `->`, free() or return of p, unless p is
// assigned in between.
void realloc_zero_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    if (!base.file) return;
    std::set<const json*> self_assigned;
    struct Zero { long long off; std::string id; const json* call; };
    std::vector<Zero> zeros;
    std::map<std::string, std::string> names;
    struct Ev { long long off; std::string id; const json* n; bool assign; };
    std::vector<Ev> evs;
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        const auto p = begin_of(n);
        if (!same_file_pos(p, base)) return;
        if (k == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2) {
            auto id = ref_id(child(n, 0));
            if (id.empty()) return;
            evs.push_back({end_of(n).offset, id, &n, true});
            const auto& r = strip_all(child(n, 1));
            if (kind(r) == "CallExpr" && callee_name(r) == "realloc") {
                auto a = call_args(r);
                if (a.size() >= 1 && ref_id(*a[0]) == id) self_assigned.insert(&r);
            }
            return;
        }
        if (k == "CallExpr") {
            auto cn = callee_name(n);
            auto a = call_args(n);
            if (cn == "realloc" && a.size() == 2 && is_zero_literal(*a[1])) {
                auto id = ref_id(*a[0]);
                if (!id.empty()) {
                    names[id] = decl_name(*a[0]);
                    zeros.push_back({end_of(n).offset, id, &n});
                }
            } else if (cn == "free" && a.size() == 1) {
                auto id = ref_id(*a[0]);
                if (!id.empty()) evs.push_back({p.offset, id, &n, false});
            }
            return;
        }
        std::string id;
        if (k == "ArraySubscriptExpr" && n_children(n) == 2) id = ref_id(child(n, 0));
        else if (k == "UnaryOperator" && str_field(n, "opcode") == "*" && n_children(n) == 1) id = ref_id(child(n, 0));
        else if (k == "MemberExpr" && flag(n, "isArrow") && n_children(n) >= 1) id = ref_id(child(n, 0));
        else if (k == "ReturnStmt" && n_children(n) == 1) id = ref_id(child(n, 0));
        if (!id.empty()) evs.push_back({p.offset, id, &n, false});
    });
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.off < b.off; });
    for (const auto& z : zeros) {
        if (self_assigned.count(z.call)) continue;
        const auto& id = z.id;
        for (auto& e : evs) {
            if (e.off <= z.off || e.id != id) continue;
            if (e.assign) break;
            ck.add(*e.n, "MEM-REALLOC-ZERO", names[id] + " used after realloc(" + names[id] + ", 0)");
            break;
        }
    }
}

// API-POPEN: a popen() stream still open at some return, or opened after
// the last pclose() (no null-guard filter, no two-return minimum).
void popen_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    if (!base.file) return;
    struct Var {
        std::string name;
        std::vector<std::pair<long long, const json*>> opens;
        std::vector<long long> closes;
    };
    std::map<std::string, Var> vars;
    std::vector<FlowEv> returns;
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        const auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        if (k == "ReturnStmt") {
            returns.push_back({p.offset, &n});
            return;
        }
        auto opener = [](const json& e) {
            const auto& s = strip_all(e);
            return is_call_kind(kind(s)) && callee_name(s) == "popen";
        };
        if (k == "VarDecl" && n_children(n) >= 1 && opener(last_child(n))) {
            auto& v = vars[str_field(n, "id")];
            v.name = str_field(n, "name");
            v.opens.push_back({p.offset, &n});
        } else if (k == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2 &&
                   opener(child(n, 1))) {
            const auto& lhs = strip_all(child(n, 0));
            if (kind(lhs) == "DeclRefExpr") {
                auto& v = vars[str_field(ref_decl(lhs), "id")];
                v.name = str_field(ref_decl(lhs), "name");
                v.opens.push_back({p.offset, &n});
            }
        } else if (is_call_kind(k) && callee_name(n) == "pclose") {
            auto a = call_args(n);
            if (a.size() == 1) {
                auto id = ref_id(*a[0]);
                if (!id.empty()) vars[id].closes.push_back(p.offset);
            }
        }
    });
    for (auto& [id, v] : vars) {
        if (v.opens.empty()) continue;
        auto last_before = [](const auto& xs, long long at, auto proj) {
            long long m = -1;
            for (auto& x : xs)
                if (proj(x) < at) m = std::max(m, proj(x));
            return m;
        };
        auto off_of = [](const std::pair<long long, const json*>& x) { return x.first; };
        auto self = [](long long x) { return x; };
        const json* leak = nullptr;
        for (auto& r : returns) {
            if (last_before(v.opens, r.off, off_of) > last_before(v.closes, r.off, self)) {
                leak = r.at;
                break;
            }
        }
        if (!leak) {
            auto last_open = std::max_element(v.opens.begin(), v.opens.end());
            long long last_close = v.closes.empty() ? -1 : *std::max_element(v.closes.begin(), v.closes.end());
            if (last_open->first > last_close) leak = last_open->second;
        }
        if (leak) ck.add(*leak, "API-POPEN", v.name + " from popen() not pclose()'d on all exits");
    }
}

// API-CHROOT: chroot() with no later chdir() in the function.
void chroot_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    if (!base.file) return;
    std::vector<std::pair<long long, const json*>> roots;
    long long last_chdir = -1;
    collect_nodes(body, [&](const json& n, const json*) {
        if (!is_call_kind(kind(n))) return;
        const auto p = begin_of(n);
        if (!same_file_pos(p, base)) return;
        const auto nm = callee_name(n);
        if (nm == "chroot" && !p.macro) roots.push_back({p.offset, &n});
        else if (nm == "chdir" || nm == "fchdir") last_chdir = std::max(last_chdir, p.offset);
    });
    for (auto& [off, at] : roots)
        if (last_chdir < off) ck.add(*at, "API-CHROOT", "chroot() without chdir(/) into the new root");
}

namespace {
// `v`, `!v`, `v == 0`, `0 == v`, `v != 0`: a test that splits on v being zero.
bool zero_test_of(const json& cond0, const std::string& id) {
    const auto& c = strip_all(cond0);
    if (ref_id(c) == id) return true;
    const auto& k = kind(c);
    if (k == "UnaryOperator" && str_field(c, "opcode") == "!" && n_children(c) == 1) return ref_id(child(c, 0)) == id;
    if (k == "BinaryOperator" && n_children(c) == 2 && one_of(str_field(c, "opcode"), {"==", "!="})) {
        const auto& l = child(c, 0);
        const auto& r = child(c, 1);
        return (ref_id(l) == id && is_zero_literal(r)) || (ref_id(r) == id && is_zero_literal(l));
    }
    return false;
}
}  // namespace

// INT-CLZ-ZERO (C): __builtin_clz/ctz(ll) on a literal 0, or on a variable
// with no zero test before the call.
void clz_zero_checks(const json& body, Checker& ck, Ctx& cx) {
    if (cx.unit.cxx) return;
    const auto base = begin_of(body);
    if (!base.file) return;
    std::vector<std::pair<long long, const json*>> conds;  // if conditions in source order
    std::vector<const json*> calls;
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "IfStmt") {
            for (auto& c : inner(n)) {
                if (one_of(kind(c), {"DeclStmt", "VarDecl"})) continue;
                conds.push_back({begin_of(n).offset, &c});
                break;
            }
        } else if (is_call_kind(k) && n_children(n) >= 1) {
            const auto raw = str_field(ref_decl(strip_all(child(n, 0))), "name");
            if (one_of(raw, {"__builtin_clz", "__builtin_ctz", "__builtin_clzll", "__builtin_ctzll"})) calls.push_back(&n);
        }
    });
    for (auto* c : calls) {
        const auto p = begin_of(*c);
        if (!same_file_pos(p, base) || p.macro) continue;
        auto a = call_args(*c);
        if (a.empty()) continue;
        bool bad = false;
        if (auto v = int_value(*a[0]); v && *v == 0) {
            bad = true;
        } else if (auto id = ref_id(*a[0]); !id.empty()) {
            bad = true;
            for (auto& [off, cond] : conds)
                if (off < p.offset && zero_test_of(*cond, id)) bad = false;
        }
        if (bad)
            ck.add(*c, "INT-CLZ-ZERO",
                   std::string(str_field(ref_decl(strip_all(child(*c, 0))), "name")) + "() on 0 is undefined");
    }
}

// TRUST-UNVALIDATED-INPUT: a variable from atoi/strtol/strtoul or scanf("…", &v)
// used as an index or divisor with no if-condition on it in the function.
void unvalidated_input_checks(const json& body, Checker& ck) {
    const auto base = begin_of(body);
    if (!base.file) return;
    std::map<std::string, std::string> parsed;  // id -> name
    std::vector<const json*> conds;
    auto parser = [](const json& e) {
        const auto& s = strip_all(e);
        return is_call_kind(kind(s)) && one_of(callee_name(s), {"atoi", "strtol", "strtoul"});
    };
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "IfStmt") {
            for (auto& c : inner(n)) {
                if (one_of(kind(c), {"DeclStmt", "VarDecl"})) continue;
                conds.push_back(&c);
                break;
            }
        } else if (k == "VarDecl" && n_children(n) >= 1 && parser(last_child(n))) {
            parsed[str_field(n, "id")] = str_field(n, "name");
        } else if (k == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2 && parser(child(n, 1))) {
            const auto& lhs = strip_all(child(n, 0));
            if (kind(lhs) == "DeclRefExpr") parsed[str_field(ref_decl(lhs), "id")] = str_field(ref_decl(lhs), "name");
        } else if (is_call_kind(k) && callee_name(n) == "scanf") {
            for (auto* a : call_args(n)) {
                const auto& s = strip_all(*a);
                if (kind(s) == "UnaryOperator" && str_field(s, "opcode") == "&" && n_children(s) == 1) {
                    const auto& r = strip_all(child(s, 0));
                    if (kind(r) == "DeclRefExpr") parsed[str_field(ref_decl(r), "id")] = str_field(ref_decl(r), "name");
                }
            }
        }
    });
    if (parsed.empty()) return;
    std::function<bool(const json&, const std::string&)> mentions = [&](const json& n, const std::string& id) {
        if (!n.is_object()) return false;
        if (kind(n) == "DeclRefExpr" && str_field(ref_decl(n), "id") == id) return true;
        for (auto& c : inner(n))
            if (mentions(c, id)) return true;
        return false;
    };
    std::vector<std::pair<std::string, std::string>> order(parsed.begin(), parsed.end());
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second < b.second; });
    for (auto& [id, name] : order) {
        if (std::any_of(conds.begin(), conds.end(), [&](const json* c) { return mentions(*c, id); })) continue;
        const json* hit = nullptr;
        long long hit_off = -1;
        collect_nodes(body, [&](const json& n, const json*) {
            const auto& k = kind(n);
            bool use = false;
            if (k == "ArraySubscriptExpr" && n_children(n) >= 2) use = ref_id(child(n, 1)) == id;
            else if ((k == "BinaryOperator" || k == "CompoundAssignOperator") && n_children(n) == 2 &&
                     one_of(str_field(n, "opcode"), {"/", "%", "/=", "%="}))
                use = ref_id(child(n, 1)) == id;
            if (!use) return;
            const auto p = begin_of(n);
            if (!same_file_pos(p, base) || p.macro) return;
            if (!hit || p.offset < hit_off) {
                hit = &n;
                hit_off = p.offset;
            }
        });
        if (hit)
            ck.add(*hit, "TRUST-UNVALIDATED-INPUT",
                   name + " from parsed input used as index or divisor without an if (" + name + " …) guard");
    }
}

void conc_toctou_checks(const json& body, Checker& ck, Ctx& cx) {
    (void)cx;
    const auto base = begin_of(body);
    if (!base.file) return;
    const json* check = nullptr;
    const json* use = nullptr;
    long long check_off = -1;
    collect_nodes(body, [&](const json& n, const json*) {
        if (use || !is_call_kind(kind(n))) return;
        const auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        const auto name = callee_name(n);
        if (!check) {
            if (one_of(name, {"access", "stat", "lstat"}) && !call_args(n).empty()) {
                check = &n;
                check_off = p.offset;
            }
            return;
        }
        if (p.offset <= check_off || !one_of(name, {"open", "fopen", "unlink", "remove", "chmod"})) return;
        auto ca = call_args(*check);
        auto ua = call_args(n);
        if (ua.empty()) return;
        // The same path when both are a plain variable or a literal; anything
        // else (computed paths) is taken as the same path, like the regex rule.
        const auto ci = ref_id(*ca[0]), ui = ref_id(*ua[0]);
        if (!ci.empty() && !ui.empty() && ci != ui) return;
        const auto cn = decl_name(*ca[0]), un = decl_name(*ua[0]);
        if (!cn.empty() && !un.empty() && cn != un) return;
        auto cs = string_lit(*ca[0]), us = string_lit(*ua[0]);
        if (cs && us && *cs != *us) return;
        use = &n;
    });
    if (check && use)
        ck.add(*use, "CONC-TOCTOU", "path checked then opened or modified without holding authority across the gap");
}

void conc_atomicity_checks(const json& body, Fn& fn, Checker& ck, Ctx& cx) {
    (void)cx;
    const auto base = begin_of(body);
    if (!base.file) return;
    bool locks = false;
    std::vector<std::pair<std::string, const json*>> rmw;  // global name, first site
    std::set<std::string> seen;
    auto global_int = [&](const json& e) -> std::string {
        const auto& s = strip_all(e);
        if (kind(s) != "DeclRefExpr") return {};
        const auto& d = ref_decl(s);
        if (str_field(d, "kind") != "VarDecl") return {};
        if (fn.locals.contains(str_field(d, "id"))) return {};
        auto t = qual_type(s);
        if (t.starts_with("volatile ")) t.erase(0, 9);
        if (!one_of(t, {"int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "size_t",
                        "short", "unsigned short"}))
            return {};
        return str_field(d, "name");
    };
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (is_call_kind(k)) {
            auto nm = callee_name(n);
            for (auto& ch : nm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (nm.find("lock") != std::string::npos || nm.find("pthread_mutex") != std::string::npos) locks = true;
            return;
        }
        // RAII guards: std::lock_guard, unique_lock, scoped_lock, a *Lock*/*Guard* type.
        if (k == "VarDecl") {
            auto t = std::string(qual_type(n));
            for (auto& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (t.find("lock") != std::string::npos || t.find("guard") != std::string::npos ||
                t.find("mutex") != std::string::npos)
                locks = true;
        }
        const auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        std::string g;
        if (k == "UnaryOperator" && one_of(str_field(n, "opcode"), {"++"}) && n_children(n) == 1)
            g = global_int(child(n, 0));
        else if (k == "CompoundAssignOperator" && str_field(n, "opcode") == "+=" && n_children(n) == 2)
            g = global_int(child(n, 0));
        else if (k == "BinaryOperator" && str_field(n, "opcode") == "=" && n_children(n) == 2) {
            g = global_int(child(n, 0));
            const auto& r = strip_all(child(n, 1));
            if (!g.empty() && !(kind(r) == "BinaryOperator" && str_field(r, "opcode") == "+" && n_children(r) == 2 &&
                                global_int(child(r, 0)) == g))
                g.clear();
        }
        if (g.empty() || seen.contains(g)) return;
        seen.insert(g);
        rmw.push_back({g, &n});
    });
    if (locks) return;
    for (auto& [g, at] : rmw)
        ck.add(*at, "CONC-ATOMICITY", "global '" + g + "' incremented without mutex protection");
}

// CTRL-SIBLING-ASYMMETRY, per function: each integer parameter, whether an
// `if (p < ...)`-style test with a return guards it, whether any `if`
// mentions it, and its first use inside a subscript.
void sibling_params(const json& f, const json& body, const std::string& fname, Ctx& cx) {
    if (is_pointer_type(result_type(std::string(qual_type(f))))) return;
    static prism::Regex ity(R"((?i)\b(?:int|long|short|unsigned|size_t|uint)\b)");
    const auto base = begin_of(body);
    if (!base.file) return;
    std::map<std::string, Ctx::SibParam> ps;  // id -> param
    for (auto& c : inner(f)) {
        if (kind(c) != "ParmVarDecl" || str_field(c, "name").empty()) continue;
        auto t = std::string(qual_type(c));
        if (t.find('*') != std::string::npos || t.find('[') != std::string::npos || !ity.search(t)) continue;
        std::string sq;
        for (auto ch : t) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!sq.empty() && sq.back() != ' ') sq.push_back(' ');
            } else {
                sq.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        }
        while (!sq.empty() && sq.back() == ' ') sq.pop_back();
        ps[str_field(c, "id")] = {*base.file, fname, str_field(c, "name"), sq};
    }
    if (ps.empty()) return;
    auto mentions = [](const json& e, std::set<std::string>& ids) {
        collect_nodes(e, [&](const json& x, const json*) {
            if (auto id = ref_id(x); !id.empty() && kind(x) == "DeclRefExpr") ids.insert(id);
        });
    };
    auto has_return = [](const json& e) {
        bool r = false;
        collect_nodes(e, [&](const json& x, const json*) { r = r || kind(x) == "ReturnStmt"; });
        return r;
    };
    std::vector<std::pair<long long, std::string>> subs;
    collect_nodes(body, [&](const json& n, const json*) {
        const auto& k = kind(n);
        if (k == "IfStmt") {
            const auto& in = inner(n);
            for (std::size_t i = 0; i < in.size(); ++i) {
                if (kind(in.at(i)) == "DeclStmt" || kind(in.at(i)) == "VarDecl") continue;
                std::set<std::string> ids;
                mentions(in.at(i), ids);
                for (auto& id : ids)
                    if (auto it = ps.find(id); it != ps.end()) it->second.tested = true;
                const auto& c = strip_all(in.at(i));
                if (kind(c) == "BinaryOperator" && n_children(c) == 2 && i + 1 < in.size() && has_return(in.at(i + 1))) {
                    auto op = str_field(c, "opcode");
                    auto id = ref_id(child(c, 0));
                    bool ok = one_of(op, {"<", ">", ">=", "<="}) || (op == "==" && is_null_ptr_expr(child(c, 1)));
                    if (ok)
                        if (auto it = ps.find(id); it != ps.end()) it->second.guarded = true;
                }
                break;
            }
            return;
        }
        if (k != "ArraySubscriptExpr") return;
        auto p = begin_of(n);
        if (!same_file_pos(p, base) || p.macro) return;
        std::set<std::string> ids;
        mentions(n, ids);
        for (auto& id : ids)
            if (ps.contains(id)) subs.emplace_back(p.offset, id);
    });
    std::sort(subs.begin(), subs.end());
    for (auto& [off, id] : subs) {
        auto& sp = ps[id];
        if (sp.subscript.file) continue;
        sp.subscript = Pos{base.file, off, false};
    }
    for (auto& [_, sp] : ps) cx.sib_params.push_back(std::move(sp));
}

void lock_order_report(Ctx& cx) {
    std::set<std::pair<std::string, std::string>> reported;  // (file, fn)
    for (std::size_t i = 0; i < cx.lock_ords.size(); ++i) {
        for (std::size_t j = i + 1; j < cx.lock_ords.size(); ++j) {
            auto& a = cx.lock_ords[i];
            auto& b = cx.lock_ords[j];
            if (a.file != b.file || a.first != b.second || a.second != b.first) continue;
            for (auto* o : {&a, &b}) {
                if (!reported.insert({o->file, o->fn}).second) continue;
                cx.add_at(o->at, o->fn, "LOCK-ORDER",
                          "locks " + o->first + " then " + o->second +
                              "; another function locks them in the opposite order");
            }
        }
    }
}

void sibling_report(Ctx& cx) {
    std::map<std::pair<std::string, std::string>, std::vector<const Ctx::SibParam*>> by;  // (file, name)
    for (auto& sp : cx.sib_params) by[{sp.file, sp.name}].push_back(&sp);
    std::set<std::pair<std::string, std::string>> reported;
    for (auto& [key, entries] : by) {
        if (entries.size() < 2) continue;
        for (auto* e : entries) {
            if (e->tested || !e->subscript.file) continue;
            bool similar = false;
            for (auto* g : entries)
                if (g != e && g->guarded && g->type == e->type) similar = true;
            if (!similar || !reported.insert({e->fn, e->name}).second) continue;
            cx.add_at(e->subscript, e->fn, "CTRL-SIBLING-ASYMMETRY",
                      e->name + " guarded in a sibling but used as subscript here without a test");
        }
    }
}

void analyze_function(const json& f, Ctx& cx, const std::vector<std::string>& tparams) {
    Fn fn;
    fn.node = &f;
    fn.name = std::string(str_field(f, "name"));
    fn.kind = std::string(kind(f));
    fn.ret = result_type(std::string(qual_type(f)));
    fn.noexcept_ = fn_noexcept(std::string(qual_type(f))) || fn.kind == "CXXDestructorDecl";
    fn.tparams = tparams;
    const json* body = nullptr;
    Walker w(cx, fn);
    w.scopes.emplace_back();  // parameters
    for (auto& c : inner(f)) {
        const auto& k = kind(c);
        if (k == "ParmVarDecl") {
            fn.locals[str_field(c, "id")] = &c;
            if (!str_field(c, "name").empty()) w.scopes.back().push_back(str_field(c, "name"));
            if (is_pointer_type(type_of(c))) fn.ptr_params.insert(str_field(c, "id"));
            auto t = cx.text_of(c);
            if (t.find('[') != std::string_view::npos && is_pointer_type(type_of(c)))
                fn.array_params.insert(str_field(c, "id"));
        }
        if (k == "CompoundStmt" || k == "CXXTryStmt") body = &c;
    }
    if (!body) return;
    // A destructor's type is only "noexcept" when the front end resolved it.
    if (fn.kind == "CXXDestructorDecl" && qual_type(f).find("noexcept(false)") != std::string::npos) fn.noexcept_ = false;
    std::vector<const json*> parents;
    w.walk(*body, nullptr, 0, parents);
    w.ck.crypto_misuse(*body);
    lock_function_checks(*body, fn, w.ck, cx);
    resource_leak_checks(*body, w.ck, cx);
    conc_toctou_checks(*body, w.ck, cx);
    vla_checks(*body, w.ck);
    realloc_zero_checks(*body, w.ck);
    ptr_arith_checks(f, *body, w.ck);
    infoleak_pad_checks(*body, w.ck);
    flex_array_checks(*body, w.ck, cx);
    onesided_index_checks(f, *body, w.ck);
    api_assign_checks(*body, w.ck);
    capacity_first_checks(*body, w.ck, cx);
    index_token_checks(*body, w.ck, cx);
    container_method_checks(*body, fn, w.ck);
    if (fn.tparams.empty()) sibling_params(f, *body, fn.name, cx);
    popen_checks(*body, w.ck);
    chroot_checks(*body, w.ck);
    clz_zero_checks(*body, w.ck, cx);
    unvalidated_input_checks(*body, w.ck);
    conc_atomicity_checks(*body, fn, w.ck, cx);
    if (fn.opaque) return;
    for (auto& id : fn.order) {
        auto& v = fn.vars[id];
        if (v.reads > 0 || !v.first_write) continue;
        w.ck.add(*v.first_write, "CTRL-DEAD-STORE",
                 "value stored to " + v.name + " is never read (" + v.name + " is not read anywhere in the function)");
    }
    if (fn.has_labels) return;
    for (auto* b : fn.blocks) {
        Flow fl(w.ck);
        fl.block(*b);
    }
    w.finish_tail_checks(f, *body);
}

void walk_decl(const json& d, Ctx& cx, std::vector<std::string>& tparams) {
    if (!d.is_object() || flag(d, "isImplicit") || flag(d, "_extra")) return;
    const auto& k = kind(d);
    if (is_template_kind(k)) {
        // The pattern, with the parameter names that make code dependent.
        std::size_t mark = tparams.size();
        for (auto& c : inner(d))
            if (one_of(kind(c), {"TemplateTypeParmDecl", "NonTypeTemplateParmDecl", "TemplateTemplateParmDecl"}) &&
                !str_field(c, "name").empty())
                tparams.push_back(str_field(c, "name"));
        for (auto& c : inner(d)) {
            if (is_function_kind(kind(c)) || kind(c) == "CXXRecordDecl") {
                walk_decl(c, cx, tparams);
                break;  // the pattern; later children are specialisations
            }
        }
        tparams.resize(mark);
        return;
    }
    if (is_function_kind(k)) {
        analyze_function(d, cx, tparams);
        return;
    }
    if (k == "NamespaceDecl" || k == "LinkageSpecDecl" || k == "CXXRecordDecl" || k == "RecordDecl" ||
        k == "ExportDecl" || k == "ClassTemplateSpecializationDecl") {
        for (auto& c : inner(d)) walk_decl(c, cx, tparams);
    }
}

}  // namespace

FileResult analyze_unit(Unit& u, const FileSet& files) {
    FileResult r;
    Ctx cx{files, u, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    for (auto& d : u.decls) collect_enums(d, cx);
    if (u.backend != "libclang") {
        for (auto& d : u.decls) collect_defined(d, cx);
        for (auto& d : u.decls) collect_facts(d, cx, "");
    }
    std::set<std::string> checked;
    for (auto& d : u.decls) {
        if (flag(d, "_extra")) continue;
        auto p = begin_of(d);
        if (!p.file) p = name_of(d);
        if (!p.file || !files.wanted(*p.file)) continue;  // an enum from elsewhere
        if (*p.file != files.main) checked.insert(files.rel_of(*p.file));
        std::vector<std::string> tparams;
        walk_decl(d, cx, tparams);
    }
    sibling_report(cx);
    lock_order_report(cx);
    // One row per (file, line, class): lambda bodies appear twice in the dump,
    // and nested blocks are flow-checked with and without their outer block.
    std::set<std::tuple<std::string, int, std::string>> seen;
    for (auto& f : cx.out)
        if (seen.insert({f.file, f.line.value_or(0), f.cls}).second) r.findings.push_back(std::move(f));
    r.headers.assign(checked.begin(), checked.end());
    r.ran = true;
    return r;
}

}  // namespace prism::astlint::detail
