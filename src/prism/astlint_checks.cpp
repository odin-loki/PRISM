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

std::string record_name(std::string t) {
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

const json& null_json() {
    static const json j;
    return j;
}

// ---------------------------------------------------------------- node helpers

const std::string& str_field(const json& n, const char* key) {
    static const std::string empty;
    if (!n.is_object()) return empty;
    auto it = n.find(key);
    if (it == n.end() || !it->is_string()) return empty;
    return it->get_ref<const std::string&>();
}

const std::string& kind(const json& n) { return str_field(n, "kind"); }

bool flag(const json& n, const char* key) {
    if (!n.is_object()) return false;
    auto it = n.find(key);
    return it != n.end() && it->is_boolean() && it->get<bool>();
}

const json& inner(const json& n) {
    if (!n.is_object()) return null_json();
    auto it = n.find("inner");
    return it == n.end() ? null_json() : *it;
}

std::size_t n_children(const json& n) {
    const auto& in = inner(n);
    return in.is_array() ? in.size() : 0;
}

const json& child(const json& n, std::size_t i) {
    const auto& in = inner(n);
    if (!in.is_array() || i >= in.size()) return null_json();
    return in[i];
}

const json& last_child(const json& n) {
    auto c = n_children(n);
    return c ? child(n, c - 1) : null_json();
}

const json& ref_decl(const json& n) {
    if (!n.is_object()) return null_json();
    auto it = n.find("referencedDecl");
    return it == n.end() ? null_json() : *it;
}

const std::string& qual_type(const json& n) {
    static const std::string empty;
    if (!n.is_object()) return empty;
    auto it = n.find("type");
    if (it == n.end() || !it->is_object()) return empty;
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

bool is_implicit_wrapper(const std::string& k) {
    return k == "ImplicitCastExpr" || k == "ExprWithCleanups" || k == "ConstantExpr" ||
           k == "MaterializeTemporaryExpr" || k == "CXXBindTemporaryExpr" || k == "FullExpr";
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

bool is_explicit_cast(const std::string& k) {
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

bool one_of(const std::string& s, std::initializer_list<std::string_view> xs) {
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
    if (kind(s) == "DeclRefExpr") return str_field(ref_decl(s), "name");
    if (kind(s) == "MemberExpr") return str_field(s, "name");
    return {};
}

const std::string& ref_id(const json& n) {
    const auto& s = strip_all(n);
    static const std::string empty;
    if (kind(s) != "DeclRefExpr") return empty;
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
        return std::stod(v);
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
    std::string v = str_field(n, "value");
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return std::nullopt;  // L"..", u8"..": skip
    std::string out;
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
        char c = v[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i + 1 > v.size()) break;
        char e = v[i];
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
                while (cnt < 3 && i + 1 < v.size() && v[i] >= '0' && v[i] <= '7') {
                    val = val * 8 + (v[i] - '0');
                    ++i;
                    ++cnt;
                }
                --i;
                out += static_cast<char>(val);
                break;
            }
            case 'x': {
                int val = 0;
                while (i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1]))) {
                    ++i;
                    val = val * 16 + (std::isdigit(static_cast<unsigned char>(v[i])) ? v[i] - '0'
                                                                                      : (std::tolower(v[i]) - 'a' + 10));
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
std::string result_type(const std::string& fnt) {
    if (auto arrow = fnt.find(") -> "); arrow != std::string::npos) return fnt.substr(arrow + 5);
    auto p = fnt.find('(');
    if (p == std::string::npos) return {};
    std::string r = fnt.substr(0, p);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

bool fn_noexcept(const std::string& fnt) {
    auto p = fnt.rfind(')');
    if (p == std::string::npos) return false;
    auto tail = fnt.substr(p);
    if (tail.find("noexcept(false)") != std::string::npos) return false;
    if (tail.find("noexcept") != std::string::npos) return true;
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
    return {&f->get_ref<const std::string&>(), o->get<long long>(), false};
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

bool dependent_kind(const std::string& k) {
    return k == "CXXDependentScopeMemberExpr" || k == "UnresolvedLookupExpr" || k == "UnresolvedMemberExpr" ||
           k == "CXXUnresolvedConstructExpr" || k == "DependentScopeDeclRefExpr" || k == "ParenListExpr" ||
           k == "PackExpansionExpr" || k == "SizeOfPackExpr" || k == "CXXFoldExpr" ||
           k == "DependentCoawaitExpr";
}

bool has_word(const std::string& s, const std::string& w) {
    for (auto p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) {
        bool l = p == 0 || !(std::isalnum(static_cast<unsigned char>(s[p - 1])) || s[p - 1] == '_');
        auto e = p + w.size();
        bool r = e >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_');
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
            if (has_word(t, p)) return true;
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
        tmp[0].extra["engine"] = std::string(ENGINE);
        out.push_back(std::move(tmp[0]));
    }

    const DeclFacts* facts(const std::string& id) const {
        auto it = unit.facts.find(id);
        return it == unit.facts.end() ? nullptr : &it->second;
    }
};

// ---------------------------------------------------------------- enums

void collect_enums(const json& n, Ctx& cx) {
    if (!n.is_object()) {
        if (n.is_array())
            for (auto& c : n) collect_enums(c, cx);
        return;
    }
    if (kind(n) == "EnumDecl") {
        const auto& id = str_field(n, "id");
        if (cx.enums.contains(id)) return;
        EnumInfo info;
        info.name = str_field(n, "name");
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
                        v = std::stoll(val);
                    } catch (...) {
                        info.values_known = false;
                    }
                } else {
                    info.values_known = false;
                }
            }
            info.consts.emplace_back(str_field(c, "name"), v);
            cx.enum_of[str_field(c, "id")] = id;
            cx.const_info[str_field(c, "id")] = {str_field(c, "name"), v};
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
            if (kind(c) == "CompoundStmt") cx.defined.insert(str_field(n, "name"));
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
            f.user = cx.defined.contains(str_field(n, "name")) && k == "FunctionDecl";
            for (auto& c : inner(n)) {
                const auto& ck = kind(c);
                if (ck == "WarnUnusedResultAttr") f.nodiscard = true;
                if (one_of(ck, {"NoReturnAttr", "CXX11NoReturnAttr", "C11NoReturnAttr"})) f.noreturn = true;
            }
            auto rt = result_type(qual_type(n));
            f.result = rt.ends_with("&&") ? "xvalue" : rt.ends_with("&") ? "lvalue" : "prvalue";
            cx.unit.facts[id] = f;
        }
    }
    std::string sub = scope;
    if (k == "NamespaceDecl" && !str_field(n, "name").empty()) sub = scope + str_field(n, "name") + "::";
    if ((k == "CXXRecordDecl" || k == "RecordDecl") && !flag(n, "isImplicit") && n.contains("inner")) {
        auto name = scope + str_field(n, "name");
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

// ---------------------------------------------------------------- simple checks

struct Checker {
    Ctx& cx;
    Fn& fn;

    bool dep(const json& n) const { return is_dependent(n, fn.tparams); }

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

    const json* sizeof_operand(const json& n) {
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
        if (user) return;  // the project's own function of that name: not the libc one
        if (name == "memset" && args.size() >= 3 && !in_macro(c) && is_zero_literal(*args[2]) &&
            !begin_of(strip_all(*args[2])).macro && !is_zero_literal(*args[1])) {
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
        // Strings into local char arrays.
        if (one_of(name, {"strcpy", "strcat"}) && args.size() >= 2) {
            auto sz = local_array_size(*args[0]);
            auto lit = string_lit(*args[1]);
            if (sz && lit && static_cast<long long>(lit->size()) + 1 > *sz) {
                add(c, "STR-OFF-BY-ONE",
                    name + "() of a " + std::to_string(lit->size()) + "-character literal (+ NUL) into " +
                        decl_name(*args[0]) + "[" + std::to_string(*sz) + "]: the copy overflows the array");
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
        format_call(c, name, args);
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
            for (auto& c : inner(n)) rec(c);
        };
        if (fn.node) rec(*fn.node);
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
        const json& farg = *args[static_cast<std::size_t>(f.fmt)];
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
            const json& a = *args[static_cast<std::size_t>(f.fmt) + 1 + i];
            auto t = type_of(strip_implicit(a));
            if (t.empty() || dep(a)) continue;
            const char cv = convs[i].c;
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
                const auto& len = convs[i].len;
                const bool wide = one_of(len, {"l", "ll", "q", "j", "z", "t"});
                if (w == 64 && !wide && len != "L") want = "an int (use %l" + std::string(1, cv) + ")";
                else if (w > 0 && w <= 32 && wide) want = "a 64-bit integer";
            }
            if (want.empty()) continue;
            add(a, "FMT-ARGS",
                name + "() conversion %" + convs[i].len + std::string(1, cv) + " expects " + want + " but argument " +
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

    void delete_expr(const json& d) {
        if (n_children(d) == 0) return;
        const auto& op = strip_all(child(d, 0));
        if (kind(op) == "CXXThisExpr") {
            add(d, "CXX-DELETE-THIS", "delete this: the object is destroyed while its member function still runs");
            return;
        }
        auto name = record_name(type_of(op));
        if (name.empty()) {
            // Dump path: records are known by their spelled name.
            name = record_name(qual_type(op));
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
        if (qt.find('&') != std::string::npos || is_pointer_type(t) || is_builtin_arith(t) || t.starts_with("enum "))
            return;
        if (in_macro(v) || dep(v)) return;
        add(v, "CXX-CATCH-BY-VALUE",
            "exception caught by value (" + qt + "): a derived exception is sliced and copied (catch by const reference)");
    }
};

// ---------------------------------------------------------------- flow (straight-line)

enum class St { None, Freed, Nulled, Moved, Uninit, Malloc, New, NewArr };

struct VState {
    St st = St::None;
    int line = 0;
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
            if (f && f->user) return St::None;
            auto nm = callee_name(c);
            if (one_of(nm, {"malloc", "calloc", "realloc", "strdup", "strndup", "aligned_alloc", "reallocarray"}))
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
            st[id] = {St::Nulled, cx.line_of(begin_of(*rhs))};
            return;
        }
        if (v && is_pointer_type(type_of(*v)) && kind(strip_casts(*rhs)) == "CXXNullPtrLiteralExpr") {
            st[id] = {St::Nulled, cx.line_of(begin_of(*rhs))};
            return;
        }
        auto a = alloc_state(*rhs);
        if (a != St::None) st[id] = {a, cx.line_of(begin_of(*rhs))};
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
                const json& p = *parents[i - 1];
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
        st[id] = {St::Freed, cx.line_of(begin_of(at))};
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
                        st[id] = {St::Moved, cx.line_of(begin_of(n))};
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
            if (pk == "MemberExpr" && flag(*p, "isArrow")) return;
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
                    st[id] = {St::Uninit, cx.line_of(begin_of(v))};
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
            eval(child(s, i), Ctx2::Branch);
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
    const json* catch_var = nullptr;
    std::vector<std::vector<std::string>> scopes;  // names of locals, for CTRL-SHADOW

    Walker(Ctx& c, Fn& f) : cx(c), fn(f), ck{c, f} {}

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
#include "astlint_discard.inc"
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
    bool terminates(const json& s) {
        const auto& k = kind(s);
        if (one_of(k, {"BreakStmt", "ReturnStmt", "ContinueStmt", "GotoStmt", "IndirectGotoStmt", "CXXThrowExpr"}))
            return true;
        if (k == "CompoundStmt") return n_children(s) && terminates(last_child(s));
        if (k == "AttributedStmt") {
            for (auto& c : inner(s))
                if (kind(c) == "FallThroughAttr") return true;
            return n_children(s) && terminates(last_child(s));
        }
        if (k == "LabelStmt") return n_children(s) && terminates(last_child(s));
        if (k == "IfStmt") {
            std::size_t cond = (flag(s, "hasInit") ? 1 : 0) + (flag(s, "hasVar") ? 1 : 0);
            if (n_children(s) < cond + 3) return false;
            return terminates(child(s, cond + 1)) && terminates(child(s, cond + 2));
        }
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
            auto& gr = groups[g];
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
            auto e = begin_of(*groups[g + 1].label);
            if (b.macro || e.macro) continue;
            auto t = std::string(cx.text(b, e));
            std::string low;
            for (char ch : t) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            static const std::regex ann(R"(fall[ \t-]*thr(ough|u)|no\s*break)");
            if (std::regex_search(low, ann)) continue;
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

    const json* decayed_local_array(const json& e0) {
        const auto& e = strip_parens(e0);
        if (kind(e) == "ImplicitCastExpr" && str_field(e, "castKind") == "ArrayToPointerDecay") {
            const auto& d = strip_parens(child(e, 0));
            if (kind(d) == "DeclRefExpr") {
                auto* v = ck.local_var(d);
                if (ck.automatic(v) && kind(*v) == "VarDecl") return v;
            }
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
                       "throw " + str_field(*catch_var, "name") + "; throws a copy of the caught exception (sliced to " +
                           qual_type(*catch_var) + "): use throw; to rethrow it");
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
            for (auto& c : inner(xs)) leaves(c);
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
        const json* cur = parents[i];
        while (i > 0) {
            const json* up = parents[i - 1];
            const auto& uk = kind(*up);
            if (uk == "ExprWithCleanups" || uk == "ParenExpr") {
                cur = up;
                --i;
                continue;
            }
            if (uk == "BinaryOperator" && str_field(*up, "opcode") == ",")
                return &child(*up, 0) != cur && assign_value_used_at(parents, i - 1);
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
        } else if (k == "CXXMemberCallExpr") {
            virtual_in_ctor(n);
        } else if (k == "ImplicitCastExpr") {
            int_div(n);
        } else if (k == "UnaryExprOrTypeTraitExpr") {
            ck.sizeof_array_param(n);
        } else if (k == "CXXDeleteExpr") {
            if (!lambda_depth) ck.delete_expr(n);
        } else if (k == "CXXThrowExpr") {
            throw_expr(n);
        } else if (k == "ReturnStmt") {
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
        fl.st[id] = {St::Nulled, cx.line_of(begin_of(n))};
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

void analyze_function(const json& f, Ctx& cx, const std::vector<std::string>& tparams) {
    Fn fn;
    fn.node = &f;
    fn.name = str_field(f, "name");
    fn.kind = kind(f);
    fn.ret = result_type(qual_type(f));
    fn.noexcept_ = fn_noexcept(qual_type(f)) || fn.kind == "CXXDestructorDecl";
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
    Ctx cx{files, u, {}, {}, {}, {}, {}, {}};
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
