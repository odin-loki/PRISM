#include "prism/stages.hpp"
#include "prism/regex.hpp"

#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prism {
namespace {

const std::unordered_set<std::string> kKw = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "_Generic", "break", "continue",
    "goto", "struct", "union", "enum",
};

const char* kScalarTypes =
    "int|unsigned(?:\\s+int)?|long|short|char|"
    "uint32_t|int32_t|size_t|bool|_Bool";

using Index = std::map<std::pair<std::string, std::string>, const FunctionInfo*>;

// The tree the inliner sees: definitions by (file basename, name), and how
// many definitions each name has across the tree.
struct Tree {
    Index index;
    std::map<std::string, int> defs;
};

std::string strip(std::string s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lstrip_copy(std::string text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    return text.substr(i);
}

std::string squeeze_ws(std::string_view typ) {
    std::string out;
    bool sp = false;
    for (char c : typ) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!sp && !out.empty()) out.push_back(' ');
            sp = true;
        } else {
            out.push_back(c);
            sp = false;
        }
    }
    return out;
}

std::string basename_of(const std::string& file) {
    return std::filesystem::path(file).filename().string();
}

bool is_ident(std::string_view name) {
    static Regex re("[A-Za-z_]\\w*");
    auto m = re.search_match(name, 0);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           static_cast<std::size_t>(m->spans[0].second) == name.size();
}

// Calls the BMC encoder models (bmc_encoder.inc model_call): a callee that
// only calls these can still be inlined. __VERIFIER_nondet_* is matched by prefix.
const std::unordered_set<std::string> kModelledCalls = {
    "abs", "labs", "llabs", "__builtin_expect", "rand", "__VERIFIER_assume",
    "assume_abort_if_not", "abort", "exit", "_Exit", "quick_exit", "reach_error",
    "__assert_fail", "__VERIFIER_error", "printLine", "printWLine", "printIntLine", "printShortLine",
    "printLongLine", "printLongLongLine", "printSizeTLine", "printHexCharLine", "printUnsignedLine", "printHexUnsignedCharLine",
    // A call statement `__VERIFIER_assert(E);` is rewritten to `assert(...)`
    // before inlining (bmc_encoder.inc rewrite_verifier_assert); an assert
    // is a checked property. Output calls are modelled for literal formats
    // and scalar arguments only (model_output_call); any other form stays
    // unmodelled in the caller, so inlining never hides it.
    "assert", "__VERIFIER_assert", "printf", "puts", "putchar"};

bool has_calls(std::string_view body) {
    static Regex re("\\b([A-Za-z_]\\w*)\\s*\\(");
    for (auto& m : re.finditer(body)) {
        auto n = m.group(1);
        if (kKw.contains(n) || kModelledCalls.contains(n) || n.starts_with("__VERIFIER_nondet_")) continue;
        return true;
    }
    return false;
}

bool returns_value(const FunctionInfo& callee) {
    std::string rt = strip(callee.return_type.empty() ? "int" : callee.return_type);
    return rt != "void" && !rt.ends_with(" void");
}

// Integer type names the BMC encoder declares (bmc_encoder.inc CDECL_TYPE).
const char* kDeclType =
    "(?:(?:unsigned|signed)\\s+(?:long\\s+long|long|short|char|int)(?:\\s+int)?|"
    "long\\s+long(?:\\s+int)?|long(?:\\s+int)?|short(?:\\s+int)?|"
    "unsigned|signed|int|char|_Bool|bool|u?int(?:8|16|32|64)_t|"
    "size_t|ssize_t|ptrdiff_t|u?intptr_t|u?intmax_t)";

std::string regex_sub(const Regex& re, const std::string& src, const std::string& repl);

// The callee's full return type, from its signature (return_type keeps only
// the last word: `unsigned char` would read as `char`). nullopt: not a type
// the encoder can declare, so the callee is not inlined.
std::optional<std::string> callee_ret_type(const FunctionInfo& callee) {
    auto sig = callee.signature;
    auto at = sig.find(callee.name + "(");
    if (at == std::string::npos) at = sig.find(callee.name);
    if (at == std::string::npos) return std::nullopt;
    auto head = sig.substr(0, at);
    static Regex attrs("\\[\\[[^\\]]*\\]\\]|\\b(?:static|inline|__inline__|__inline|extern|constexpr)\\b");
    head = squeeze_ws(strip(regex_sub(attrs, head, " ")));
    if (head == "void") return head;
    static Regex full(std::string("^") + kDeclType + "$");
    auto m = full.search_match(head, 0);
    if (!m) return std::nullopt;
    return head;
}

// A definition that is certain to be the one a call from `main` runs, though
// it is not static: the tree defines the name once in a C unit, and it is
// neither weak (a strong one elsewhere replaces it at link time) nor
// `inline` (C99 6.7.4p7: an inline definition that is not also external
// may be bypassed for an external definition in another unit; GNU89
// `extern inline` is not an external definition).
// C units only: in C++ a call may resolve to an overload, a member or a
// template the front end does not tell apart by name.
bool sole_external_definition(const FunctionInfo& callee, const std::map<std::string, int>& defs) {
    auto ext = std::filesystem::path(callee.file).extension().string();
    if (ext != ".c" && ext != ".i") return false;
    auto it = defs.find(callee.name);
    if (it == defs.end() || it->second != 1) return false;
    static Regex bad("\\b(?:inline|__inline|__inline__|weak|__weak__|weakref|alias)\\b");
    return !bad.search(callee.signature);
}

bool free_names_unshadowed(const FunctionInfo& callee, const FunctionInfo& caller);

bool inlineable_callee(const FunctionInfo& callee, const FunctionInfo& caller,
                       const std::map<std::string, int>& defs) {
    // A non-static callee only into `main`: the unit then is the program,
    // and its own definition of the callee is the one that runs.
    if (!callee.is_static && !(caller.name == "main" && sole_external_definition(callee, defs))) return false;
    // The SV-COMP error functions are properties of the call itself (bmc
    // model_call): a definition in the unit (`static void reach_error() {}`)
    // must not replace the call, or the violation would disappear.
    if (callee.name == "reach_error" || callee.name == "__VERIFIER_error") return false;
    // A non-static name the encoder models by name (the SV-COMP harness
    // functions, assume, exit, ...) keeps its model, as before.
    if (!callee.is_static && (kModelledCalls.contains(callee.name) || callee.name.starts_with("__VERIFIER_")))
        return false;
    if (callee.kind != "SCALAR" && callee.kind != "VOID") return false;
    if (has_calls(callee.body)) return false;
    if (!free_names_unshadowed(callee, caller)) return false;
    // A goto's label would be copied once per call site (labels must be
    // unique in a function for the bmc goto model).
    static Regex goto_kw("\\bgoto\\b");
    if (goto_kw.search(callee.body)) return false;
    if (returns_value(callee) && !callee_ret_type(callee)) return false;
    return true;
}

const FunctionInfo* lookup(const Index& index, const std::string& file, const std::string& name) {
    auto it = index.find({basename_of(file), name});
    return it == index.end() ? nullptr : it->second;
}

std::vector<std::string> split_args(std::string args) {
    args = strip(std::move(args));
    if (args.empty()) return {};
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        char ch = args[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == ',' && depth == 0) {
            out.push_back(strip(args.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(strip(args.substr(start)));
    return out;
}

std::optional<std::tuple<std::string, std::string, std::size_t>> parse_call(const std::string& text,
                                                                          std::size_t start) {
    static Regex ident("[A-Za-z_]\\w*");
    if (start > text.size()) return std::nullopt;
    auto m = ident.search_match(std::string_view(text).substr(start), 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    std::string name = m->text;
    if (kKw.contains(name) || !is_ident(name)) return std::nullopt;
    std::size_t pos = start + name.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n')) ++pos;
    if (pos >= text.size() || text[pos] != '(') return std::nullopt;
    int depth = 0;
    for (std::size_t i = pos; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') {
            --depth;
            if (depth == 0) {
                return std::make_tuple(name, text.substr(pos + 1, i - (pos + 1)), i + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> match_balanced(const std::string& text, char open_ch,
                                                                 char close_ch) {
    if (text.empty() || text.front() != open_ch) return std::nullopt;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == open_ch) ++depth;
        else if (text[i] == close_ch) {
            --depth;
            if (depth == 0) {
                return std::make_pair(text.substr(1, i - 1), text.substr(i + 1));
            }
        }
    }
    return std::nullopt;
}

std::string regex_sub(const Regex& re, const std::string& src, const std::string& repl) {
    std::string out;
    std::size_t i = 0;
    for (auto& m : re.finditer(src)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(m.spans[0].first < 0 ? 0 : m.spans[0].first);
        auto b = static_cast<std::size_t>(m.spans[0].second < 0 ? 0 : m.spans[0].second);
        if (a < i) continue;
        out.append(src, i, a - i);
        for (std::size_t k = 0; k < repl.size(); ++k) {
            if ((repl[k] == '$' || repl[k] == '\\') && k + 1 < repl.size() &&
                std::isdigit(static_cast<unsigned char>(repl[k + 1]))) {
                out += m.group(static_cast<std::size_t>(repl[k + 1] - '0'));
                ++k;
            } else {
                out += repl[k];
            }
        }
        i = b;
    }
    out.append(src, i, std::string::npos);
    return out;
}

std::string rename_params(const std::string& body, const std::map<std::string, std::string>& rename) {
    static Regex ident("[A-Za-z_]\\w*");
    std::string out;
    std::size_t i = 0;
    for (auto& m : ident.finditer(body)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(m.spans[0].first < 0 ? 0 : m.spans[0].first);
        auto b = static_cast<std::size_t>(m.spans[0].second < 0 ? 0 : m.spans[0].second);
        if (a < i) continue;
        out.append(body, i, a - i);
        auto it = rename.find(m.text);
        out += (it != rename.end()) ? it->second : m.text;
        i = b;
    }
    out.append(body, i, std::string::npos);
    return out;
}

// A `return` must leave the inlined body: every return becomes
// `{ ret = X; break; }` inside `do { ... } while (0)`. A callee with its own
// loop or switch (where that break would bind to the wrong statement) is
// inlined only when its single return is its last top-level statement.
std::optional<std::string> adapt_callee_body(std::string body, const std::map<std::string, std::string>& rename,
                                             const std::optional<std::string>& ret_var) {
    body = strip(rename_params(body, rename));
    static Regex ret_any("\\breturn\\b");
    static Regex ret_val("\\breturn\\s+([^;]+);");
    static Regex ret_bare("\\breturn\\s*;");
    static Regex loops("\\b(?:for|while|do|switch)\\b");
    auto rets = ret_any.finditer(body);
    if (rets.empty()) return body;
    bool single_final = false;
    if (rets.size() == 1) {
        auto at = static_cast<std::size_t>(rets[0].spans[0].first);
        int depth = 0;
        for (std::size_t i = 0; i < at; ++i) {
            if (body[i] == '{') ++depth;
            else if (body[i] == '}') --depth;
        }
        auto semi = body.find(';', at);
        single_final = depth == 0 && semi != std::string::npos && strip(body.substr(semi + 1)).empty();
    }
    if (single_final) {
        if (ret_var) body = regex_sub(ret_val, body, *ret_var + " = $1;");
        return strip(regex_sub(ret_bare, body, ""));
    }
    if (!loops.finditer(body).empty()) return std::nullopt;
    if (ret_var) body = regex_sub(ret_val, body, "{ " + *ret_var + " = $1; break; }");
    body = regex_sub(ret_bare, body, "break;");
    return "do {\n" + body + "\n} while (0);";
}

// Locals the callee declares, renamed with the call-site prefix so they do
// not collide with the caller's names.
void add_callee_locals(const std::string& body, const std::string& prefix,
                       std::map<std::string, std::string>& rename) {
    static Regex decl(std::string("\\b") + kDeclType + "\\s+([A-Za-z_]\\w*)\\s*(?=[=;,\\[)])");
    static const std::unordered_set<std::string> type_words = {
        "int", "unsigned", "signed", "long", "short", "char", "_Bool", "bool"};
    for (auto& m : decl.finditer(body)) {
        auto name = m.group(1);
        if (type_words.contains(name) || rename.count(name)) continue;
        rename[name] = prefix + "_l_" + name;
    }
}

// The identifiers of code (not in string or character literals, not
// numbers, not member names after `.`/`->`), each with whether a `(`
// follows it (a call).
std::vector<std::pair<std::string, bool>> code_idents(std::string_view b) {
    std::vector<std::pair<std::string, bool>> out;
    auto word = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
    for (std::size_t i = 0; i < b.size();) {
        char ch = b[i];
        if (ch == '"' || ch == '\'') {
            for (++i; i < b.size() && b[i] != ch; ++i)
                if (b[i] == '\\') ++i;
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            while (i < b.size() && (word(b[i]) || b[i] == '.')) ++i;
            continue;
        }
        if (!word(ch)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < b.size() && word(b[j])) ++j;
        std::size_t p = i;
        while (p > 0 && std::isspace(static_cast<unsigned char>(b[p - 1]))) --p;
        bool member = (p > 0 && b[p - 1] == '.') || (p > 1 && b[p - 1] == '>' && b[p - 2] == '-');
        std::size_t k = j;
        while (k < b.size() && std::isspace(static_cast<unsigned char>(b[k]))) ++k;
        if (!member) out.emplace_back(std::string(b.substr(i, j - i)), k < b.size() && b[k] == '(');
        i = j;
    }
    return out;
}

// A name the callee uses but does not declare (a global, an enumerator, a
// macro) must mean the same after inlining: if the caller declares or
// uses the same name, the inlined text would bind it to the caller's
// variable instead (a global the callee writes would become the caller's
// local). Such a callee is not inlined.
bool free_names_unshadowed(const FunctionInfo& callee, const FunctionInfo& caller) {
    static const std::unordered_set<std::string> words = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
        "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "_Bool", "bool", "true", "false", "size_t", "ssize_t", "ptrdiff_t", "intptr_t",
        "uintptr_t", "intmax_t", "uintmax_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
        "uint16_t", "uint32_t", "uint64_t"};
    std::unordered_set<std::string> bound;
    for (auto& [_, n] : callee.params) bound.insert(n);
    std::map<std::string, std::string> locals;
    add_callee_locals(callee.body, "", locals);
    for (auto& [n, _] : locals) bound.insert(n);
    std::unordered_set<std::string> caller_names;
    for (auto& [_, n] : caller.params) caller_names.insert(n);
    for (auto& [n, call] : code_idents(caller.body))
        if (!call) caller_names.insert(n);
    for (auto& [n, call] : code_idents(callee.body)) {
        if (call || bound.contains(n) || words.contains(n)) continue;
        if (caller_names.contains(n)) return false;
    }
    return true;
}

std::string param_type(const std::string& typ) {
    auto t = squeeze_ws(typ.empty() ? "int" : typ);
    return t.empty() ? "int" : t;
}

std::optional<std::string> build_inline_block(const FunctionInfo& callee,
                                              const std::vector<std::string>& args,
                                              const std::string& prefix,
                                              const std::string& tail) {
    if (args.size() != callee.params.size()) return std::nullopt;
    std::vector<std::string> decls;
    std::map<std::string, std::string> rename;
    for (std::size_t i = 0; i < callee.params.size(); ++i) {
        const auto& [typ, pname] = callee.params[i];
        std::string temp = prefix + "_i" + std::to_string(i);
        rename[pname] = temp;
        decls.push_back(param_type(typ) + " " + temp + " = " + args[i] + ";");
    }
    std::optional<std::string> ret_var;
    if (returns_value(callee)) {
        auto rt = callee_ret_type(callee);
        if (!rt) return std::nullopt;
        ret_var = prefix + "_ret";
        decls.push_back(*rt + " " + *ret_var + ";");
    }
    add_callee_locals(callee.body, prefix, rename);
    auto adapted = adapt_callee_body(callee.body, rename, ret_var);
    if (!adapted) return std::nullopt;
    std::vector<std::string> parts = std::move(decls);
    if (!adapted->empty()) parts.push_back(std::move(*adapted));
    if (!tail.empty()) {
        std::string t = tail;
        while (!t.empty() && t.back() == ';') t.pop_back();
        parts.push_back(t + ";");
    }
    std::string inner;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) inner += "\n";
        inner += parts[i];
    }
    return "{\n" + inner + "\n}";
}

std::string try_inline_stmt(const std::string& stmt, const FunctionInfo& fn, const Tree& tree,
                            int site) {
    const std::string& raw = stmt;
    std::string s = strip(stmt);
    if (s.empty() || s.back() != ';') return raw;

    std::string prefix = "_h" + std::to_string(site);

    static Regex ret_re("return\\s+");
    if (auto m = ret_re.search_match(s, 0); m && !m->spans.empty() && m->spans[0].first == 0) {
        auto end = static_cast<std::size_t>(m->spans[0].second);
        auto call = parse_call(s, end);
        if (!call) return raw;
        auto [name, args_src, call_end] = *call;
        auto rest_src = s.substr(end);
        std::size_t k = 0;
        while (k < rest_src.size() && std::isspace(static_cast<unsigned char>(rest_src[k]))) ++k;
        if (!rest_src.substr(k).starts_with(name)) return raw;
        auto* callee = lookup(tree.index, fn.file, name);
        if (!callee || callee->name == fn.name || !inlineable_callee(*callee, fn, tree.defs)) return raw;
        if (strip(s.substr(call_end)) != ";") return raw;
        auto args = split_args(args_src);
        std::string tail = "return " + std::string(returns_value(*callee) ? prefix + "_ret" : "0");
        auto block = build_inline_block(*callee, args, prefix, tail);
        return block ? *block : raw;
    }

    static Regex assign_re("([A-Za-z_]\\w*)\\s*=\\s*");
    if (auto m = assign_re.search_match(s, 0); m && !m->spans.empty() && m->spans[0].first == 0) {
        std::string lhs = m->group(1);
        auto end = static_cast<std::size_t>(m->spans[0].second);
        auto call = parse_call(s, end);
        if (!call) return raw;
        auto [name, args_src, call_end] = *call;
        auto* callee = lookup(tree.index, fn.file, name);
        if (!callee || callee->name == fn.name || !inlineable_callee(*callee, fn, tree.defs)) return raw;
        if (!returns_value(*callee)) return raw;
        if (strip(s.substr(call_end)) != ";") return raw;
        auto args = split_args(args_src);
        auto block = build_inline_block(*callee, args, prefix, lhs + " = " + prefix + "_ret");
        return block ? *block : raw;
    }

    static Regex decl_re(std::string("(?P<typ>") + kScalarTypes +
                         ")\\s+(?P<name>[A-Za-z_]\\w*)\\s*=\\s*");
    if (auto m = decl_re.search_match(s, 0); m && !m->spans.empty() && m->spans[0].first == 0) {
        std::string lhs = m->named("name");
        auto end = static_cast<std::size_t>(m->spans[0].second);
        auto call = parse_call(s, end);
        if (!call) return raw;
        auto [name, args_src, call_end] = *call;
        auto* callee = lookup(tree.index, fn.file, name);
        if (!callee || callee->name == fn.name || !inlineable_callee(*callee, fn, tree.defs)) return raw;
        if (!returns_value(*callee)) return raw;
        if (strip(s.substr(call_end)) != ";") return raw;
        auto args = split_args(args_src);
        auto block = build_inline_block(*callee, args, prefix, lhs + " = " + prefix + "_ret");
        // The declaration stays in the caller's scope; the block assigns it.
        return block ? m->named("typ") + " " + lhs + ";\n" + *block : raw;
    }

    auto call = parse_call(s, 0);
    if (!call) return raw;
    auto [name, args_src, call_end] = *call;
    if (strip(s.substr(call_end)) != ";") return raw;
    auto* callee = lookup(tree.index, fn.file, name);
    if (!callee || callee->name == fn.name || !inlineable_callee(*callee, fn, tree.defs)) return raw;
    if (returns_value(*callee)) return raw;
    auto args = split_args(args_src);
    auto block = build_inline_block(*callee, args, prefix, "");
    return block ? *block : raw;
}

// One statement from the start of text, up to its `;` at depth 0 (string
// and character literals skipped): (statement, rest). A brace at depth 0
// before the `;` (an initialiser list, a labelled block) is part of it.
std::pair<std::string, std::string> take_stmt(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '"' || ch == '\'') {
            for (++i; i < text.size() && text[i] != ch; ++i)
                if (text[i] == '\\') ++i;
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') ++depth;
        else if (ch == ')' || ch == ']' || ch == '}') --depth;
        else if (ch == ';' && depth == 0) return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    return {text, {}};
}

// `kw` as a whole word at the start of text.
bool starts_word(const std::string& text, std::string_view kw) {
    if (!text.starts_with(kw)) return false;
    return text.size() == kw.size() ||
           !(std::isalnum(static_cast<unsigned char>(text[kw.size()])) || text[kw.size()] == '_');
}

std::string transform_body(std::string text, const FunctionInfo& fn, const Tree& tree, int& counter);

// One statement, recursing into the bodies of if/else/while/for/do/switch
// (a call in a branch or loop body is inlined like one at the top level;
// each site keeps its own prefix). A changed statement that is not a block
// becomes one, so it stays a single statement under `if (c)`.
std::string transform_stmt(std::string& text, const FunctionInfo& fn, const Tree& tree, int& counter) {
    text = lstrip_copy(std::move(text));
    if (text.empty()) return {};
    if (text.front() == '{') {
        auto matched = match_balanced(text, '{', '}');
        if (!matched) {
            std::string all = std::move(text);
            text.clear();
            return all;
        }
        text = matched->second;
        return "{" + transform_body(matched->first, fn, tree, counter) + "}";
    }
    auto sub = [&](const std::string& head) {
        int before = counter;
        auto body = transform_stmt(text, fn, tree, counter);
        if (counter != before && !body.starts_with("{")) body = "{" + body + "}";
        return head + " " + body;
    };
    for (std::string_view kw : {"if", "while", "for", "switch"}) {
        if (!starts_word(text, kw)) continue;
        auto after = lstrip_copy(text.substr(kw.size()));
        auto cond = match_balanced(after, '(', ')');
        if (!cond) break;  // not a statement shape this pass knows: taken as is below
        text = cond->second;
        auto out = sub(std::string(kw) + " (" + cond->first + ")");
        if (kw == "if") {
            auto rest = lstrip_copy(text);
            if (starts_word(rest, "else")) {
                text = rest.substr(4);
                out += " " + sub("else");
            }
        }
        return out;
    }
    if (starts_word(text, "do")) {
        text = text.substr(2);
        auto out = sub("do");
        auto [tail, rest] = take_stmt(lstrip_copy(text));  // while (...);
        text = rest;
        return out + " " + tail;
    }
    auto [stmt, rest] = take_stmt(text);
    text = rest;
    if (strip(stmt).empty()) return stmt;
    auto new_stmt = try_inline_stmt(stmt, fn, tree, counter);
    if (new_stmt != stmt) ++counter;
    return new_stmt;
}

std::string transform_body(std::string text, const FunctionInfo& fn, const Tree& tree, int& counter) {
    std::string out;
    while (!(text = lstrip_copy(std::move(text))).empty()) {
        std::size_t before = text.size();
        out += transform_stmt(text, fn, tree, counter);
        out += '\n';
        if (text.size() >= before) break;  // no progress: never loop
    }
    return out;
}

FunctionInfo inline_function(FunctionInfo fn, const Tree& tree) {
    if (fn.kind == "POINTER") return fn;
    int counter = 0;
    auto new_body = transform_body(fn.body, fn, tree, counter);
    if (counter == 0) return fn;
    fn.body = std::move(new_body);
    // The inlined body no longer maps offset for offset onto the source.
    fn.body_line = fn.body_col = 0;
    fn.col_shifts.clear();
    return fn;
}

}  // namespace

std::vector<FunctionInfo> inline_static(const std::vector<FunctionInfo>& functions) {
    Tree tree;
    for (auto& f : functions) {
        tree.index[{basename_of(f.file), f.name}] = &f;
        ++tree.defs[f.name];
    }
    std::vector<FunctionInfo> out;
    out.reserve(functions.size());
    for (auto& fn : functions) out.push_back(inline_function(fn, tree));
    return out;
}

}  // namespace prism
