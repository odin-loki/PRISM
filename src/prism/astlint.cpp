// Roadmap 2.8: C/C++ lints on the Clang AST (include/prism/astlint.hpp).
//
// clang -fsyntax-only -Xclang -ast-dump=json prints the whole translation
// unit, headers included (a C++ unit that includes <vector> and <map> is
// ~200 MB of JSON). The dump is therefore split without a full parse: each
// top-level declaration is bracket-matched in the text, and only those whose
// location is in the scanned file (plus enum declarations from anywhere, for
// the switch check) are handed to nlohmann::json.
//
// The dumper elides a location's "file" (and "line") when it equals the one
// printed before it, so the current file is state carried in document order;
// every location object gets the resolved file in "_f", and lines come from
// "offset" (always printed) against the file's own bytes.

#include "prism/astlint.hpp"

#include "prism/checkers.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/threads.hpp"

#include "proc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace prism::astlint {
namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

constexpr std::size_t npos = std::string_view::npos;
constexpr const char* kInstall = "install clang (apt install clang-18) for the AST lints";

const json& null_json() {
    static const json j;
    return j;
}

// ---------------------------------------------------------------- dump split

bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

std::size_t skip_ws(std::string_view s, std::size_t i) {
    while (i < s.size() && is_ws(s[i])) ++i;
    return i;
}

// One past the end of the JSON value starting at s[i]; npos when malformed.
std::size_t skip_value(std::string_view s, std::size_t i) {
    if (i >= s.size()) return npos;
    const char c = s[i];
    if (c == '"') {
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\') ++i;
            else if (s[i] == '"') return i + 1;
        }
        return npos;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        while (i < s.size()) {
            const char d = s[i];
            if (d == '"') {
                i = skip_value(s, i);
                if (i == npos) return npos;
                continue;
            }
            if (d == '{' || d == '[') ++depth;
            else if ((d == '}' || d == ']') && --depth == 0) return i + 1;
            ++i;
        }
        return npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !is_ws(s[i])) ++i;
    return i;
}

std::optional<std::string> json_string_at(std::string_view s, std::size_t i) {
    auto e = skip_value(s, i);
    if (e == npos || s[i] != '"') return std::nullopt;
    try {
        return json::parse(s.substr(i, e - i)).get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

// Position of the value of `"key":` searching forward from `from`, or npos.
std::size_t find_key(std::string_view s, std::string_view key, std::size_t from, std::size_t to) {
    std::string pat = "\"" + std::string(key) + "\"";
    for (auto p = s.find(pat, from); p != npos && p < to; p = s.find(pat, p + 1)) {
        auto q = skip_ws(s, p + pat.size());
        if (q < to && s[q] == ':') return skip_ws(s, q + 1);
    }
    return npos;
}

// The last location "file" value in s[b, e) — not an "includedFrom" file.
std::optional<std::string> last_loc_file(std::string_view s, std::size_t b, std::size_t e) {
    constexpr std::string_view pat = "\"file\"";
    std::size_t hi = e;
    while (hi > b) {
        auto p = s.rfind(pat, hi - 1);
        if (p == npos || p < b) return std::nullopt;
        hi = p;
        auto q = skip_ws(s, p + pat.size());
        if (q >= e || s[q] != ':') continue;
        auto v = skip_ws(s, q + 1);
        // "includedFrom": { "file": ... } names the includer, not a location.
        std::size_t k = p;
        while (k > b && is_ws(s[k - 1])) --k;
        if (k > b && s[k - 1] == '{') {
            --k;
            while (k > b && is_ws(s[k - 1])) --k;
            if (k > b && s[k - 1] == ':') {
                --k;
                while (k > b && is_ws(s[k - 1])) --k;
                constexpr std::string_view inc = "\"includedFrom\"";
                if (k >= b + inc.size() && s.substr(k - inc.size(), inc.size()) == inc) continue;
            }
        }
        if (auto str = json_string_at(s, v)) return str;
    }
    return std::nullopt;
}

// Resolve every location object's file ("_f") in document order.
void annotate(json& o, std::string& cur) {
    if (o.is_object()) {
        if (o.contains("offset")) {
            auto it = o.find("file");
            if (it != o.end() && it->is_string()) cur = it->get<std::string>();
            o["_f"] = cur;
        }
        for (auto& el : o.items()) {
            if (el.key() == "includedFrom" || el.key() == "_f") continue;
            annotate(el.value(), cur);
        }
    } else if (o.is_array()) {
        for (auto& v : o) annotate(v, cur);
    }
}

// Top-level declarations of the main file (and every EnumDecl).
std::optional<std::vector<json>> split_dump(std::string_view s, const std::string& main_file,
                                            std::string& err) {
    auto root = s.find('{');
    if (root == npos) {
        err = "no JSON in clang output";
        return std::nullopt;
    }
    auto root_end = skip_value(s, root);
    if (root_end == npos) {
        err = "truncated AST dump";
        return std::nullopt;
    }
    auto kind_at = find_key(s, "kind", root, root_end);
    auto kind = kind_at == npos ? std::nullopt : json_string_at(s, kind_at);
    if (!kind || *kind != "TranslationUnitDecl") {
        err = "AST dump does not start with a TranslationUnitDecl";
        return std::nullopt;
    }
    std::vector<json> out;
    auto arr = find_key(s, "inner", root, root_end);
    if (arr == npos) return out;  // empty unit
    if (s[arr] != '[') {
        err = "malformed AST dump";
        return std::nullopt;
    }
    std::string cur;
    std::size_t i = skip_ws(s, arr + 1);
    while (i < root_end && s[i] != ']') {
        if (s[i] != '{') {
            err = "malformed AST dump";
            return std::nullopt;
        }
        auto e = skip_value(s, i);
        if (e == npos || e > root_end) {
            err = "truncated AST dump";
            return std::nullopt;
        }
        std::string at_start = cur;
        std::optional<std::string> file;
        bool implicit_loc = true;
        if (auto l = find_key(s, "loc", i, e); l != npos && s[l] == '{') {
            auto le = skip_value(s, l);
            if (le != npos && skip_ws(s, l + 1) != le - 1) {
                implicit_loc = false;
                file = last_loc_file(s, l, le);
                if (!file) file = cur;
            }
        }
        auto k = find_key(s, "kind", i, e);
        auto kname = k == npos ? std::nullopt : json_string_at(s, k);
        const bool keep = !implicit_loc && ((file && *file == main_file) ||
                                            (kname && *kname == "EnumDecl"));
        if (auto last = last_loc_file(s, i, e)) cur = *last;
        if (keep) {
            try {
                auto j = json::parse(s.substr(i, e - i));
                annotate(j, at_start);
                out.push_back(std::move(j));
            } catch (const std::exception& ex) {
                err = std::string("AST dump does not parse: ") + ex.what();
                return std::nullopt;
            }
        }
        i = skip_ws(s, e);
        if (i < root_end && s[i] == ',') i = skip_ws(s, i + 1);
    }
    return out;
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

const json& ref_decl(const json& n) {
    if (!n.is_object()) return null_json();
    auto it = n.find("referencedDecl");
    return it == n.end() ? null_json() : *it;
}

std::string type_of(const json& n) {
    if (!n.is_object()) return {};
    auto it = n.find("type");
    if (it == n.end() || !it->is_object()) return {};
    auto d = it->find("desugaredQualType");
    std::string t = d != it->end() && d->is_string() ? d->get<std::string>()
                                                     : str_field(*it, "qualType");
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
    while ((is_implicit_wrapper(kind(*p)) || kind(*p) == "ParenExpr") && n_children(*p) >= 1)
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
    return one_of(t, {"int", "long", "long long", "short", "signed char", "char", "long int",
                      "short int", "long long int", "signed", "signed int", "signed long",
                      "__int128", "__int128_t"});
}

bool is_unsigned_int(const std::string& t) {
    if (t.find('*') != std::string::npos || t.find('[') != std::string::npos) return false;
    if (t.starts_with("enum ")) return false;
    return t.starts_with("unsigned") || t == "__uint128_t";
}

bool is_builtin_arith(const std::string& t) {
    return is_signed_int(t) || is_unsigned_int(t) ||
           one_of(t, {"float", "double", "long double", "bool", "_Bool", "wchar_t", "char8_t",
                      "char16_t", "char32_t", "_Float16", "__fp16", "__float128"});
}

bool is_pointer_type(const std::string& t) {
    auto star = t.rfind('*');
    if (star == std::string::npos) return false;
    std::string rest = t.substr(star + 1);
    std::istringstream ss(rest);
    std::string w;
    while (ss >> w)
        if (!one_of(w, {"const", "volatile", "restrict", "__restrict", "__restrict__"}))
            return false;
    return true;
}

bool is_int_type(const std::string& t) { return is_signed_int(t) || is_unsigned_int(t); }

std::string decl_name(const json& n) {
    const auto& s = strip_all(n);
    if (kind(s) == "DeclRefExpr") return str_field(ref_decl(s), "name");
    if (kind(s) == "MemberExpr") return str_field(s, "name");
    return {};
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

// ---------------------------------------------------------------- the checks

struct EnumInfo {
    std::string name;
    std::vector<std::pair<std::string, long long>> consts;
    bool values_known = true;
};

struct Ctx {
    const std::string& main;
    std::string_view rel;
    std::vector<std::size_t> starts;  // byte offset of each line
    std::vector<std::string> lines;   // comment-stripped, for evidence
    std::map<std::string, EnumInfo> enums;             // EnumDecl id -> info
    std::unordered_map<std::string, std::string> enum_of;  // EnumConstantDecl id -> EnumDecl id
    std::unordered_map<std::string, std::pair<std::string, long long>> const_info;  // id -> name, value
    std::vector<Finding> out;

    int line_of(const Pos& p) const {
        if (!p.file || *p.file != main || p.offset < 0) return 0;
        auto it = std::upper_bound(starts.begin(), starts.end(), static_cast<std::size_t>(p.offset));
        return static_cast<int>(it - starts.begin());
    }

    void add(const json& at, const std::string& fn, std::string_view cls, const std::string& msg,
             bool allow_macro = false) {
        auto p = begin_of(at);
        if (p.macro && !allow_macro) return;
        int line = line_of(p);
        if (line <= 0) return;
        std::vector<Finding> tmp;
        lint_add(tmp, rel, fn.empty() ? std::nullopt : std::optional<std::string>(fn), line, cls, msg,
                 lines);
        tmp[0].extra["engine"] = std::string(ENGINE);
        out.push_back(std::move(tmp[0]));
    }
};

void collect_enums(const json& n, Ctx& cx) {
    if (!n.is_object()) {
        if (n.is_array())
            for (auto& c : n) collect_enums(c, cx);
        return;
    }
    if (kind(n) == "EnumDecl") {
        const auto& id = str_field(n, "id");
        EnumInfo info;
        info.name = str_field(n, "name");
        long long next = 0;
        for (auto& c : inner(n)) {
            if (kind(c) != "EnumConstantDecl") continue;
            long long v = next;
            if (n_children(c) > 0) {
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

struct Var {
    std::string name;
    int reads = 0;
    const json* first_write = nullptr;
};

struct Fn {
    std::string name;
    std::unordered_set<std::string> ptr_params;  // ParmVarDecl ids of pointer type
    std::map<std::string, Var> vars;             // eligible locals, by VarDecl id
    std::vector<std::string> order;
    bool opaque = false;  // inline asm / setjmp: data flow is not visible
};

std::string callee_name(const json& call) {
    if (n_children(call) == 0) return {};
    const auto& c = strip_all(child(call, 0));
    if (kind(c) != "DeclRefExpr") return {};
    std::string n = str_field(ref_decl(c), "name");
    if (n.starts_with("__builtin___") && n.ends_with("_chk")) n = n.substr(12, n.size() - 16);
    else if (n.starts_with("__builtin_")) n = n.substr(10);
    return n;
}

bool is_zero_literal(const json& n) {
    const auto& s = strip_all(n);
    return (kind(s) == "IntegerLiteral" || kind(s) == "CharacterLiteral") &&
           (s.contains("value") && ((s["value"].is_string() && s["value"].get<std::string>() == "0") ||
                                    (s["value"].is_number_integer() && s["value"].get<long long>() == 0)));
}

void check_assign_cond(const json& cond, Ctx& cx, const Fn& fn) {
    const auto& n = strip_implicit(cond);
    if (kind(n) != "BinaryOperator" || str_field(n, "opcode") != "=") return;
    auto lhs = decl_name(child(n, 0));
    cx.add(n, fn.name, "CTRL-ASSIGN-COND",
           "assignment " + (lhs.empty() ? std::string("") : "to " + lhs + " ") +
               "used as a condition; == was probably meant (extra parentheses mark it deliberate)");
}

void check_loop_sign(const json& cond, Ctx& cx, const Fn& fn) {
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
        // A non-negative constant converts exactly.
        if (kind(core) == "IntegerLiteral" || kind(core) == "CharacterLiteral") continue;
        if (kind(core) == "DeclRefExpr" && str_field(ref_decl(core), "kind") == "EnumConstantDecl")
            continue;
        if (begin_of(core).macro) continue;
        auto sn = decl_name(src);
        auto un = decl_name(other);
        cx.add(n, fn.name, "INT-SIGN-CONV",
               "loop condition compares signed " + (sn.empty() ? std::string("value") : sn) +
                   " with unsigned " + (un.empty() ? std::string("value") : un) +
                   ": the signed side is converted to unsigned, so a negative value compares huge");
        return;
    }
}

bool label_enum_consts(const json& n, std::vector<std::string>& ids, bool& has_default,
                       bool& non_enum) {
    if (!n.is_object()) return true;
    const auto& k = kind(n);
    if (k == "SwitchStmt") return true;  // nested switch: its own labels
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
    return true;
}

bool has_user_conversion(const json& n) {
    if (!n.is_object()) return false;
    if (str_field(n, "castKind") == "UserDefinedConversion") return true;
    if (kind(n) == "CXXMemberCallExpr") return true;
    for (auto& c : inner(n))
        if (has_user_conversion(c)) return true;
    return false;
}

void check_switch(const json& sw, Ctx& cx, const Fn& fn) {
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
        if (it == cx.enum_of.end()) return;  // an enumerator this dump did not show
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
    cx.add(sw, fn.name, "INT-ENUM-HOLE",
           "switch (" + (var.empty() ? std::string("...") : var) + ") on enum " + ename +
               " misses " + miss + " and has no default");
}

bool same_expr(const json& a0, const json& b0) {
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
        return str_field(a, "opcode") == "*" && str_field(b, "opcode") == "*" &&
               same_expr(child(a, 0), child(b, 0));
    if (k == "ArraySubscriptExpr")
        return same_expr(child(a, 0), child(b, 0)) && same_expr(child(a, 1), child(b, 1));
    if (k == "CXXThisExpr") return true;
    return false;
}

void check_self_assign(const json& n, Ctx& cx, const Fn& fn) {
    if (str_field(n, "opcode") != "=" || n_children(n) != 2) return;
    const auto& lhs = child(n, 0);
    for (const json* x : {&n, &lhs, &strip_all(lhs)}) {
        auto t = x->find("type");
        if (t == x->end()) return;
        if (str_field(*t, "qualType").find("volatile") != std::string::npos) return;  // device access
    }
    if (!same_expr(lhs, child(n, 1))) return;
    auto name = decl_name(lhs);
    cx.add(n, fn.name, "CTRL-SELF-ASSIGN",
           (name.empty() ? std::string("value") : name) +
               " is assigned to itself; the statement has no effect (another operand was probably meant)");
}

struct SizeUse {
    std::vector<std::size_t> bufs;
    std::size_t size;
};

void check_call(const json& call, Ctx& cx, Fn& fn) {
    auto name = callee_name(call);
    if (name.empty()) return;
    if (one_of(name, {"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp", "longjmp", "vfork"}))
        fn.opaque = true;
    std::vector<const json*> args;
    for (std::size_t i = 1; i < n_children(call); ++i) args.push_back(&child(call, i));
    if (name == "memset" && args.size() >= 3 && !begin_of(call).macro && is_zero_literal(*args[2]) &&
        !begin_of(strip_all(*args[2])).macro && !is_zero_literal(*args[1])) {
        cx.add(call, fn.name, "MEM-MEMSET-SWAP",
               "memset() length is 0 and the fill byte is not: size and fill-byte arguments are swapped");
    }
    static const std::map<std::string, SizeUse, std::less<>> kSized{
        {"memset", {{0}, 2}},    {"memcpy", {{0, 1}, 2}}, {"memmove", {{0, 1}, 2}},
        {"memcmp", {{0, 1}, 2}}, {"bzero", {{0}, 1}},     {"explicit_bzero", {{0}, 1}},
    };
    auto it = kSized.find(name);
    if (it == kSized.end() || args.size() <= it->second.size || fn.ptr_params.empty()) return;
    const auto& sz = strip_all(*args[it->second.size]);
    if (kind(sz) != "UnaryExprOrTypeTraitExpr" || str_field(sz, "name") != "sizeof" || n_children(sz) == 0)
        return;
    const auto& e = strip_all(child(sz, 0));
    if (kind(e) != "DeclRefExpr") return;
    const auto& id = str_field(ref_decl(e), "id");
    if (!fn.ptr_params.contains(id)) return;
    bool is_buf = false;
    for (auto b : it->second.bufs) {
        const auto& a = strip_all(*args[b]);
        if (kind(a) == "DeclRefExpr" && str_field(ref_decl(a), "id") == id) is_buf = true;
    }
    if (!is_buf) return;
    auto pname = str_field(ref_decl(e), "name");
    cx.add(call, fn.name, "MEM-SIZEOF-PTR",
           name + "() size is sizeof(" + pname + "), the size of a pointer: " + pname +
               " is a pointer parameter, not the caller's buffer (pass the length)");
}

void check_int_div(const json& cast, Ctx& cx, const Fn& fn) {
    if (str_field(cast, "castKind") != "IntegralToFloating") return;
    const auto& d = strip_parens(child(cast, 0));
    if (kind(d) != "BinaryOperator" || str_field(d, "opcode") != "/") return;
    if (!is_int_type(type_of(d))) return;
    const auto& l = strip_all(child(d, 0));
    const auto& r = strip_all(child(d, 1));
    auto lit = [](const json& x) -> std::optional<long long> {
        if (kind(x) != "IntegerLiteral") return std::nullopt;
        try {
            return std::stoll(str_field(x, "value"));
        } catch (...) {
            return std::nullopt;
        }
    };
    auto lv = lit(l), rv = lit(r);
    if (rv && *rv == 1) return;
    if (lv && rv && *rv != 0 && *lv % *rv == 0) return;  // exact constant quotient
    auto to = type_of(cast);
    auto ln = decl_name(l), rn = decl_name(r);
    std::string what = (ln.empty() ? std::string("...") : ln) + " / " + (rn.empty() ? std::string("...") : rn);
    cx.add(d, fn.name, "INT-DIV-TO-FLOAT",
           "integer division " + what + " truncates before the result is converted to " +
               (to.empty() ? std::string("floating point") : to));
}

bool eligible_local(const json& v) {
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

bool is_function_kind(const std::string& k) {
    return k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl" ||
           k == "CXXDestructorDecl" || k == "CXXConversionDecl";
}

bool is_template_kind(const std::string& k) {
    return k == "FunctionTemplateDecl" || k == "ClassTemplateDecl" ||
           k == "ClassTemplatePartialSpecializationDecl" || k == "VarTemplateDecl" ||
           k == "TypeAliasTemplateDecl" || k == "VarTemplatePartialSpecializationDecl";
}

void walk(const json& n, const json* parent, std::size_t idx, Ctx& cx, Fn& fn) {
    if (!n.is_object()) return;
    const auto& k = kind(n);
    if (is_template_kind(k)) return;  // dependent code: only instantiated types are exact
    if (k == "GCCAsmStmt" || k == "MSAsmStmt") fn.opaque = true;
    if (k == "VarDecl" && eligible_local(n)) {
        const auto& id = str_field(n, "id");
        if (!fn.vars.contains(id)) {
            fn.vars[id] = Var{str_field(n, "name"), 0, nullptr};
            fn.order.push_back(id);
        }
    } else if (k == "DeclRefExpr") {
        const auto& id = str_field(ref_decl(n), "id");
        if (auto it = fn.vars.find(id); it != fn.vars.end()) {
            const bool store = parent && kind(*parent) == "BinaryOperator" &&
                               str_field(*parent, "opcode") == "=" && idx == 0;
            if (!store) ++it->second.reads;
            else if (!it->second.first_write && !begin_of(*parent).macro)
                it->second.first_write = parent;
        }
    } else if (k == "IfStmt") {
        check_assign_cond(child(n, (flag(n, "hasInit") ? 1 : 0) + (flag(n, "hasVar") ? 1 : 0)), cx, fn);
    } else if (k == "WhileStmt") {
        const auto& c = child(n, flag(n, "hasVar") ? 1 : 0);
        check_assign_cond(c, cx, fn);
        check_loop_sign(c, cx, fn);
    } else if (k == "DoStmt") {
        const auto& c = child(n, 1);
        check_assign_cond(c, cx, fn);
        check_loop_sign(c, cx, fn);
    } else if (k == "ForStmt" && n_children(n) == 5) {
        check_assign_cond(child(n, 2), cx, fn);
        check_loop_sign(child(n, 2), cx, fn);
    } else if (k == "SwitchStmt") {
        check_switch(n, cx, fn);
    } else if (k == "BinaryOperator") {
        check_self_assign(n, cx, fn);
    } else if (k == "CallExpr") {
        check_call(n, cx, fn);
    } else if (k == "ImplicitCastExpr") {
        check_int_div(n, cx, fn);
    }
    const auto& in = inner(n);
    if (in.is_array())
        for (std::size_t i = 0; i < in.size(); ++i) walk(in[i], &n, i, cx, fn);
}

void analyze_function(const json& f, Ctx& cx) {
    Fn fn;
    fn.name = str_field(f, "name");
    const json* body = nullptr;
    for (auto& c : inner(f)) {
        const auto& k = kind(c);
        if (k == "ParmVarDecl" && is_pointer_type(type_of(c))) fn.ptr_params.insert(str_field(c, "id"));
        if (k == "CompoundStmt" || k == "CXXTryStmt") body = &c;
    }
    if (!body) return;
    walk(*body, nullptr, 0, cx, fn);
    if (fn.opaque) return;
    for (auto& id : fn.order) {
        auto& v = fn.vars[id];
        if (v.reads > 0 || !v.first_write) continue;
        cx.add(*v.first_write, fn.name, "CTRL-DEAD-STORE",
               "value stored to " + v.name + " is never read (" + v.name +
                   " is not read anywhere in the function)");
    }
}

void walk_decl(const json& d, Ctx& cx) {
    if (!d.is_object() || flag(d, "isImplicit")) return;
    const auto& k = kind(d);
    if (is_template_kind(k)) return;
    if (is_function_kind(k)) {
        analyze_function(d, cx);
        return;
    }
    if (k == "NamespaceDecl" || k == "LinkageSpecDecl" || k == "CXXRecordDecl" || k == "ExportDecl" ||
        k == "ClassTemplateSpecializationDecl") {
        for (auto& c : inner(d)) walk_decl(c, cx);
    }
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower_ext(const fs::path& p) {
    auto e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_cxx_unit(const fs::path& p) {
    auto e = lower_ext(p);
    return e == ".cc" || e == ".cpp" || e == ".cxx" || e == ".ii" || e == ".c++";
}

// ---------------------------------------------------------------- compile db

std::vector<std::string> shell_split(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    char q = 0;
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < cmd.size()) cur += cmd[++i];
            else cur += c;
        } else if (c == '"' || c == '\'') {
            q = c;
            have = true;
        } else if (c == '\\' && i + 1 < cmd.size()) {
            cur += cmd[++i];
            have = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (have || !cur.empty()) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
        }
    }
    if (have || !cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> keep_flags(const std::vector<std::string>& argv, const fs::path& dir) {
    std::vector<std::string> out;
    auto abs = [&](const std::string& p) {
        fs::path q(p);
        return (q.is_relative() ? dir / q : q).lexically_normal().string();
    };
    static const std::vector<std::string> kPathFlags{"-I", "-isystem", "-iquote", "-idirafter", "-include"};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const auto& a = argv[i];
        bool done = false;
        for (auto& pf : kPathFlags) {
            if (a == pf && i + 1 < argv.size()) {
                out.push_back(pf);
                out.push_back(abs(argv[++i]));
                done = true;
                break;
            }
            if (pf == "-I" && a.size() > 2 && a.starts_with("-I")) {
                out.push_back("-I" + abs(a.substr(2)));
                done = true;
                break;
            }
        }
        if (done) continue;
        if ((a == "-D" || a == "-U") && i + 1 < argv.size()) {
            out.push_back(a);
            out.push_back(argv[++i]);
        } else if ((a.starts_with("-D") || a.starts_with("-U")) && a.size() > 2) {
            out.push_back(a);
        } else if (a.starts_with("-std=")) {
            out.push_back(a);
        }
    }
    return out;
}

std::string canon(const fs::path& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : c).generic_string();
}

using Db = std::map<std::string, std::vector<std::string>>;

std::shared_ptr<const Db> load_db(const fs::path& db) {
    static std::mutex mu;
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const Db>>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto key = canon(db);
    std::error_code ec;
    auto mtime = fs::last_write_time(db, ec);
    if (auto it = cache.find(key); it != cache.end() && it->second.first == mtime) return it->second.second;
    auto m = std::make_shared<Db>();
    try {
        auto j = nlohmann::json::parse(read_file(db));
        for (auto& e : j) {
            if (!e.is_object() || !e.contains("file") || !e["file"].is_string()) continue;
            fs::path dir = e.value("directory", std::string());
            fs::path file = e["file"].get<std::string>();
            if (file.is_relative()) file = dir / file;
            std::vector<std::string> argv;
            if (e.contains("arguments") && e["arguments"].is_array()) {
                for (auto& a : e["arguments"])
                    if (a.is_string()) argv.push_back(a.get<std::string>());
            } else if (e.contains("command") && e["command"].is_string()) {
                argv = shell_split(e["command"].get<std::string>());
            }
            (*m)[canon(file)] = keep_flags(argv, dir);
        }
    } catch (...) {
        m->clear();
    }
    cache[key] = {mtime, m};
    return m;
}

}  // namespace

// ---------------------------------------------------------------- public API

FileResult analyze_dump(std::string_view dump, const std::string& main_file, std::string_view source,
                        std::string_view rel) {
    FileResult r;
    std::string err;
    auto decls = split_dump(dump, main_file, err);
    if (!decls) {
        r.error = err;
        return r;
    }
    Ctx cx{main_file, rel, {}, {}, {}, {}, {}, {}};
    cx.starts.push_back(0);
    for (std::size_t i = 0; i < source.size(); ++i)
        if (source[i] == '\n') cx.starts.push_back(i + 1);
    {
        std::istringstream ls(strip_comments_keep_lines(source, true));
        std::string line;
        while (std::getline(ls, line)) cx.lines.push_back(line);
    }
    for (auto& d : *decls) collect_enums(d, cx);
    for (auto& d : *decls) {
        auto p = begin_of(d);
        if (!p.file || *p.file != main_file) continue;  // an enum from a header
        walk_decl(d, cx);
    }
    // A lambda body is dumped under its closure type and again under the
    // LambdaExpr: one row per (line, class).
    std::set<std::pair<int, std::string>> seen;
    for (auto& f : cx.out)
        if (seen.insert({f.line.value_or(0), f.cls}).second) r.findings.push_back(std::move(f));
    r.ran = true;
    return r;
}

std::vector<std::string> compile_db_flags(const fs::path& root, const fs::path& src) {
    std::error_code ec;
    fs::path base = fs::is_directory(root, ec) ? root : root.parent_path();
    for (auto& cand : {base / "compile_commands.json", base / "build" / "compile_commands.json"}) {
        if (!fs::is_regular_file(cand, ec)) continue;
        auto db = load_db(cand);
        if (auto it = db->find(canon(src)); it != db->end()) return it->second;
    }
    return {};
}

FileResult run_file(const std::optional<fs::path>& clang, const fs::path& src, const fs::path& root,
                    std::string_view rel, double timeout_s) {
    FileResult r;
    if (!clang) {
        r.error = "clang not found";
        return r;
    }
    const bool cxx = is_cxx_unit(src);
    // -fsyntax-only + the JSON dump is a parse, not a check: -w keeps clang's
    // warnings (the `warnings` stage reports them) out of the merged
    // stdout/stderr so the dump stays valid JSON. Errors still fail the parse.
    std::vector<std::string> argv{clang->string(), "-fsyntax-only", "-w", "-Xclang", "-ast-dump=json"};
    auto db = compile_db_flags(root, src);
    const bool has_std = std::any_of(db.begin(), db.end(), [](auto& f) { return f.starts_with("-std="); });
    if (!has_std) argv.push_back(cxx ? "-std=gnu++23" : "-std=gnu17");
    if (!cxx) {
        // Front-end leniency as in the pir stage: old C still parses.
        for (auto* w : {"-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
                        "-Wno-error=int-conversion", "-Wno-error=incompatible-pointer-types"})
            argv.push_back(w);
    }
    if (db.empty()) {
        std::error_code ec;
        fs::path base = fs::is_directory(root, ec) ? root : root.parent_path();
        argv.push_back("-I" + src.parent_path().string());
        if (!base.empty()) argv.push_back("-I" + base.string());
        if (fs::is_directory(base / "include", ec)) argv.push_back("-I" + (base / "include").string());
    }
    argv.insert(argv.end(), db.begin(), db.end());
    argv.push_back(src.string());
    auto pr = detail::run_process(argv, timeout_s);
    if (pr.failed) {
        r.error = "could not start " + clang->string();
        return r;
    }
    if (pr.timed_out) {
        r.error = "clang timed out after " + std::to_string(static_cast<int>(timeout_s)) + " s";
        return r;
    }
    if (pr.rc != 0) {
        std::string first;
        std::size_t from = pr.text.size() > (1u << 20) ? pr.text.size() - (1u << 20) : 0;
        std::istringstream ls(pr.text.substr(from));
        std::string line;
        while (std::getline(ls, line)) {
            if (line.find("error:") != std::string::npos) {
                first = line;
                break;
            }
        }
        // Name the file as the report does, not by the absolute path clang saw.
        if (auto at = first.find(src.string()); at != std::string::npos)
            first.replace(at, src.string().size(), std::string(rel));
        if (first.size() > 240) first = first.substr(0, 240) + "...";
        if (first.empty() && pr.rc == 127) first = "could not start " + clang->string();
        r.error = "does not parse" + (first.empty() ? std::string() : ": " + first) +
                  (db.empty() ? " (no compile_commands.json entry)" : "");
        return r;
    }
    return analyze_dump(pr.text, src.string(), read_file(src), rel);
}

void supersede(std::vector<Finding>& regex, std::vector<Finding>& ast) {
    std::map<std::tuple<std::string, int, std::string>, Finding*> idx;
    for (auto& f : ast)
        if (f.line) idx[{f.file, *f.line, f.cls}] = &f;
    std::vector<Finding> kept;
    kept.reserve(regex.size());
    for (auto& f : regex) {
        if (f.status == laws::FAILED && f.line) {
            if (auto it = idx.find({f.file, *f.line, f.cls}); it != idx.end()) {
                auto& a = *it->second;
                if (!a.extra.contains("supersedes")) {
                    a.extra["supersedes"] = "regex";
                    if (f.function) a.function = f.function;
                }
                continue;
            }
        }
        kept.push_back(std::move(f));
    }
    regex = std::move(kept);
}

bool is_layer_row(const Finding& f) {
    return f.status == laws::NOTRUN && f.extra.contains(std::string(LAYER_KEY));
}

std::vector<Finding> run_lints_ast(const std::vector<fs::path>& paths, const fs::path& root,
                                   const Config& cfg) {
    auto fe = pir::find_frontend(cfg);
    return run_lints_ast(paths, root, cfg, fe.clang ? fe.clang : fe.clangxx);
}

std::vector<Finding> run_lints_ast(const std::vector<fs::path>& paths, const fs::path& root,
                                   const Config& cfg, const std::optional<fs::path>& clang) {
    auto out = run_lints(paths, root, cfg.jobs);
    std::vector<fs::path> units;
    int headers = 0;
    for (auto& p : paths) {
        auto e = lower_ext(p);
        if (is_tu_ext(e)) units.push_back(p);
        else if (is_c_ext(e)) ++headers;
    }
    auto rel_of = [&](const fs::path& p) {
        std::error_code ec;
        if (fs::is_directory(root, ec)) {
            auto r = fs::relative(p, root, ec);
            return ec ? p.filename().string() : r.generic_string();
        }
        return p.filename().string();
    };
    auto layer_row = [&](std::string file, std::string msg) {
        Finding f;
        f.stage = "lints";
        f.status = std::string(laws::NOTRUN);
        f.file = std::move(file);
        f.message = std::move(msg);
        f.strength = std::string(laws::STRENGTH_FINDS);
        f.extra[std::string(LAYER_KEY)] = std::string(ENGINE);
        return f;
    };
    std::vector<Finding> ast, notes;
    if (!units.empty() && !clang) {
        auto f = layer_row("", "clang not found: the Clang-AST lints did not run on " +
                                   std::to_string(units.size()) + " translation unit(s); regex lints only");
        f.extra["install"] = kInstall;
        notes.push_back(std::move(f));
    } else if (!units.empty()) {
        std::vector<FileResult> res(units.size());
        const double timeout = std::max(30.0, cfg.timeout);
        parallel_for(cfg.jobs, units, [&](std::size_t i, const fs::path& p) {
            try {
                res[i] = run_file(clang, p, root, rel_of(p), timeout);
            } catch (const std::exception& ex) {
                res[i] = FileResult{};
                res[i].error = std::string("internal error: ") + ex.what();
            }
        });
        for (std::size_t i = 0; i < units.size(); ++i) {
            if (!res[i].ran) {
                auto f = layer_row(rel_of(units[i]),
                                   "Clang-AST lints did not run: " + res[i].error + "; regex lints only");
                f.extra["install"] = kInstall;
                notes.push_back(std::move(f));
                continue;
            }
            for (auto& f : res[i].findings) ast.push_back(std::move(f));
        }
    }
    if (headers > 0) {
        auto f = layer_row("", std::to_string(headers) +
                                   " header file(s) not parsed on their own: the Clang-AST lints check "
                                   "each translation unit's own code; headers get regex lints only");
        f.extra["files"] = std::to_string(headers);
        notes.push_back(std::move(f));
    }
    supersede(out, ast);
    out.insert(out.end(), ast.begin(), ast.end());
    out.insert(out.end(), notes.begin(), notes.end());
    return out;
}

}  // namespace prism::astlint
