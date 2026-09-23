#include "prism/cparse.hpp"
#include "prism/scope.hpp"

#include "prism/laws.hpp"
#include "prism/regex.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

namespace prism {
namespace {

const std::unordered_set<std::string> SCALAR_WORDS = {
    "void", "bool", "_bool", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int_fast8_t", "int_fast16_t", "int_fast32_t", "int_fast64_t",
    "uint_fast8_t", "uint_fast16_t", "uint_fast32_t", "uint_fast64_t",
    "int_least8_t", "int_least16_t", "int_least32_t", "int_least64_t",
    "uint_least8_t", "uint_least16_t", "uint_least32_t", "uint_least64_t",
    "_bool", "wchar_t", "char16_t", "char32_t", "enum",
};

const std::unordered_set<std::string> KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "_Generic",
};

// Same patterns as prism/cparse.py; keep the two in step.
//
// Trailing declarator junk is not a parameter list. `throw()` must not be
// swallowed as params or `noexcept` functions are invisible. C++
// ref-qualifiers and a trailing return type (`-> int`) end here too.
const std::string ATTR =
    R"((?:)"
    R"(\s*__attribute__\s*\(\s*\([^;{}]*?\)\s*\))"
    R"(|\s*noexcept(?:\s*\([^;{}]*?\))?)"
    R"(|\s*throw\s*\([^;{}]*?\))"
    R"(|\s*(?:const|volatile|override|final))"
    R"(|\s*&&?)"
    R"()*)"
    R"((?:\s*->[^;{}]*)?)";
// `<...>` nested three deep: `std::map<int, std::vector<int>>`.
const std::string T0 = R"([^<>;{}()]*)";
const std::string T2 = "<" + T0 + "(?:<" + T0 + ">" + T0 + ")*>";
const std::string TMPL = "<" + T0 + "(?:" + T2 + T0 + ")*>";
// Qualifier chain: `std::`, `W::`, `W<T>::`.
const std::string QUAL = R"((?:[A-Za-z_]\w*\s*(?:)" + TMPL + R"(\s*)?::\s*)*)";
// A parameter list: no `)` escapes it, so `int m(void) BODY` never runs on
// into the next function's parameters. Two levels of nested parens cover
// `void (*cb)(int)`.
const std::string P0 = R"([^;{}()]*)";
const std::string P2 = R"(\()" + P0 + R"((?:\()" + P0 + R"(\))" + P0 + R"()*\))";
const std::string PARAMS = P0 + "(?:" + P2 + P0 + ")*";
// Leading attribute forms: GNU, C++11/C23, MSVC.
const std::string PRE_ATTR =
    R"((?:__attribute__\s*\(\s*\()" + PARAMS + R"(\)\s*\))"
    R"(|\[\[[^\];{}]*\]\])"
    R"(|__declspec\s*\([^;{}()]*\)))";
const std::string OPERATOR =
    R"(operator\s*(?:\(\s*\)|\[\s*\]|new(?:\s*\[\s*\])?|delete(?:\s*\[\s*\])?)"
    R"(|[^\s\w(){};]{1,3}))";

const std::string FUNC_HEAD_PAT =
    R"((?m)^[ \t]*)"
    R"((?P<head>)"
    R"((?P<tmpl>template[ \t]*)" + TMPL + R"([ \t]*)?)"
    R"((?P<mods>(?:(?:static|inline|extern|constexpr|consteval|virtual|)"
    R"(explicit|friend|unsigned|signed|const|volatile|restrict|)"
    R"(_Noreturn|__inline|__inline__|__forceinline|thread_local|)"
    R"(__extension__)\s+|)" + PRE_ATTR + R"([ \t]*)*))"
    R"((?P<ret>(?:(?:struct|enum|union|class|typename)\s+)?)"
    R"((?:long\s+long|long\s+(?:int|double)\b|short\s+int\b)"
    R"(|[A-Za-z_]\w*(?:\s*)" + TMPL + R"()?)"
    R"((?:\s*::\s*[A-Za-z_]\w*(?:\s*)" + TMPL + R"()?)*)))"
    R"((?P<stars>(?:\s*(?:[*&]|\b(?:const|volatile)\b))+\s*|\s+))"
    // A calling-convention / export macro: `Z3_ast Z3_API Z3_mk_add(`.
    R"((?P<cc>(?:[A-Z_][A-Z0-9_]*|__\w+)[ \t]+)?)"
    R"((?P<name>)" + QUAL + R"((?:)" + OPERATOR + R"(|[A-Za-z_]\w*))\s*)"
    R"(\((?P<params>)" + PARAMS + R"()\))"
    R"((?P<knr>(?:\s*(?:register\s+)?[A-Za-z_][\w \t\n*,\[\]]*;)*))"
    R"((?P<attrs>)" + ATTR + R"())"
    R"(\s*))"
    R"(\{)";
const std::string KNR_PARAMS_PAT = R"(\s*[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*\s*)";

// The declarators below are matched by the scope scan against the text
// before an unattributed `{` (from the previous `;`, `{` or `}`).

// `int (*get_handler(int sig))(int)`: a function returning a function pointer.
const std::string FUNC_PTR_DECL_PAT =
    R"(\s*(?P<mods>(?:(?:static|inline|extern|const|unsigned|signed)\s+)*))"
    R"((?P<ret>(?:(?:struct|enum|union)\s+)?[A-Za-z_]\w*)\s*(?P<stars>\**)\s*)"
    R"(\(\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*\((?P<params>)" + PARAMS + R"()\)\s*\))"
    R"(\s*\((?P<fparams>)" + PARAMS + R"()\)\s*\Z)";
// Out-of-line constructor / destructor: `W::W(int a) : v(a)`, `W::~W()`.
const std::string CTOR_DECL_PAT =
    R"(\s*(?:template\s*)" + TMPL + R"(\s*)?(?:(?:inline|constexpr)\s+)*)"
    R"((?P<name>(?:[A-Za-z_]\w*\s*(?:)" + TMPL + R"(\s*)?::\s*)+~?[A-Za-z_]\w*)\s*)"
    R"(\((?P<params>)" + PARAMS + R"()\))" + ATTR +
    R"((?:\s*:(?!:)[^;{}]*)?\s*\Z)";
// In-class definition FUNC_HEAD cannot see: a constructor, destructor,
// conversion operator, or a method not at the start of a line
// (`struct W { int go() { return 1; } };`).
const std::string MEMBER_DECL_PAT =
    R"(\s*(?:template\s*)" + TMPL + R"(\s*)?)"
    R"((?:(?:inline|constexpr|consteval|explicit|virtual|static|friend)\s+)"
    R"(|)" + PRE_ATTR + R"(\s*)*)"
    R"((?P<ret>[A-Za-z_][\w:<>,\s*&]*?[\s*&])??)"
    R"((?P<name>operator\s+(?:(?:const|volatile)\s+)*[A-Za-z_][\w:<>]*)"
    R"((?:\s*(?:[*&]|\b(?:const|volatile)\b))*)"
    R"(|~?[A-Za-z_]\w*|)" + OPERATOR + R"()\s*)"
    R"(\((?P<params>)" + PARAMS + R"()\))" + ATTR +
    R"((?:\s*:(?!:)[^;{}]*)?\s*\Z)";

bool is_pointer_type(std::string_view typ) {
    return typ.find('*') != std::string_view::npos || typ.find('[') != std::string_view::npos;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::pair<std::string, std::string>> split_params(std::string params) {
    while (!params.empty() && std::isspace(static_cast<unsigned char>(params.front()))) params.erase(params.begin());
    while (!params.empty() && std::isspace(static_cast<unsigned char>(params.back()))) params.pop_back();
    if (params.empty() || params == "void") return {};
    std::vector<std::pair<std::string, std::string>> out;
    std::string raw;
    std::stringstream ss(params);
    while (std::getline(ss, raw, ',')) {
        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
        if (raw.empty() || raw == "...") continue;
        if (auto eq = raw.find('='); eq != std::string::npos) {
            raw = raw.substr(0, eq);  // C++ default argument
            while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
        }
        raw = Regex("\\b(const|volatile|restrict|register)\\b").search(raw)
                  ? [&] {
                        std::string r;
                        std::size_t i = 0;
                        Regex re("\\b(const|volatile|restrict|register)\\b");
                        auto tmp = raw;
                        // simple word strip
                        std::string acc;
                        std::string word;
                        for (char c : (raw + " ")) {
                            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') word.push_back(c);
                            else {
                                if (word != "const" && word != "volatile" && word != "restrict" && word != "register") {
                                    if (!acc.empty() && !word.empty()) acc.push_back(' ');
                                    acc += word;
                                }
                                word.clear();
                                if (!std::isspace(static_cast<unsigned char>(c))) acc.push_back(c);
                            }
                        }
                        return acc;
                    }()
                  : raw;
        std::string spaced = raw;
        for (char& c : spaced)
            if (c == '*') { /* keep */ }
        auto m = re_search_match("([A-Za-z_]\\w*)\\s*$", spaced);
        if (!m) {
            out.emplace_back(raw, "");
            continue;
        }
        std::string name = m->group(1);
        std::string typ = spaced.substr(0, static_cast<std::size_t>(std::max(0, m->spans[1].first)));
        while (!typ.empty() && std::isspace(static_cast<unsigned char>(typ.back()))) typ.pop_back();
        if (typ.empty()) typ = raw;
        out.emplace_back(typ, name);
    }
    return out;
}

std::string kind_of(const std::string& ret, const std::string& stars,
                    const std::vector<std::pair<std::string, std::string>>& params) {
    (void)ret;
    (void)stars;
    if (params.empty()) return "VOID";
    auto param_ok = [](std::string t) -> std::string {
        for (char& c : t)
            if (c == '\t') c = ' ';
        if (is_pointer_type(t)) return "POINTER";
        std::vector<std::string> words;
        std::string w;
        for (char c : t + " ") {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') w.push_back(c);
            else if (!w.empty()) {
                words.push_back(w);
                w.clear();
            }
        }
        if (words.empty()) return "OTHER";
        for (auto& word : words) {
            if (!SCALAR_WORDS.contains(word)) {
                if (word == "struct" || word == "union") return "OTHER";
                return "OTHER";
            }
        }
        return "SCALAR";
    };
    bool pointer = false, other = false;
    for (auto& [t, _] : params) {
        auto k = param_ok(t);
        if (k == "POINTER") pointer = true;
        if (k == "OTHER") other = true;
    }
    if (pointer) return "POINTER";
    if (other) return "OTHER";
    return "SCALAR";
}

std::string collapse_ws(std::string_view s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            sp = true;
        } else {
            if (sp && !out.empty()) out.push_back(' ');
            sp = false;
            out.push_back(c);
        }
    }
    return out;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::string regex_sub(const Regex& re, std::string_view repl, std::string_view s) {
    std::string out;
    std::size_t off = 0;
    for (auto& m : re.finditer(s)) {
        auto a = static_cast<std::size_t>(m.spans[0].first);
        out.append(s.substr(off, a - off));
        out.append(repl);
        off = static_cast<std::size_t>(m.spans[0].second);
    }
    out.append(s.substr(std::min(off, s.size())));
    return out;
}

bool full_match(const Regex& re, std::string_view s) {
    auto m = re.search_match(s);
    return m && m->spans[0].first == 0 && m->spans[0].second == static_cast<int>(s.size());
}

// Anchored at 0 like Python re.match.
std::optional<Match> match_at_start(const Regex& re, std::string_view s) {
    // Anchored: an unanchored search retried every start offset, which was
    // super-linear on crafted input (docs/FUZZ_SELF.md F1).
    return re.match_prefix(s);
}

// `W :: go` -> `W::go`, `operator <<` -> `operator<<`; `operator bool` kept.
std::string norm_name(std::string_view s) {
    static Regex colons(R"(\s*::\s*)");
    static Regex op(R"(\boperator\s+(?=[^\w\s]))");
    return regex_sub(op, "operator", regex_sub(colons, "::", collapse_ws(s)));
}

// Character literal interiors as spaces, quotes kept (`'}'` -> `' '`).
std::string blank_char_literals(std::string text) {
    if (text.find('\'') == std::string::npos) return text;
    static Regex lit(R"('(?:\\.|[^'\\\n])*')");
    for (auto& m : lit.finditer(text))
        for (int k = m.spans[0].first + 1; k < m.spans[0].second - 1; ++k)
            text[static_cast<std::size_t>(k)] = ' ';
    return text;
}

// Preprocessor lines (with continuations) as spaces, newlines kept.
std::string blank_preprocessor(std::string text) {
    if (text.find('#') == std::string::npos) return text;
    static Regex pp(R"((?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*)", true);
    for (auto& m : pp.finditer(text))
        for (int k = m.spans[0].first; k < m.spans[0].second; ++k)
            if (text[static_cast<std::size_t>(k)] != '\n') text[static_cast<std::size_t>(k)] = ' ';
    return text;
}

// `head` with `operator=` names and parenthesized text collapsed: `=` left
// in the shape is an initializer, not a default argument.
std::string head_shape(const std::string& head) {
    static Regex op_sym(R"(\boperator\s*[^\s\w(]{1,3})");
    static Regex paren(R"(\([^()]*\))");
    std::string s = head.find("operator") != std::string::npos ? regex_sub(op_sym, "operator", head) : head;
    while (s.find('(') != std::string::npos) {
        auto t = regex_sub(paren, "", s);
        if (t == s) break;
        s = std::move(t);
    }
    std::string out;
    for (char c : s)
        if (c != ')') out.push_back(c);
    if (head.find('(') != std::string::npos) out.push_back('(');
    return out;
}

// `virtual ~W() {` / `explicit W(int) {` in a class: the scope scan names
// these; FUNC_HEAD must not take the keyword for a return type.
const std::set<std::string> NOT_A_TYPE = {
    "virtual", "explicit", "friend", "typedef", "using", "new", "delete", "operator",
};

struct Found {
    int head_start = 0;
    int close = 0;
    FunctionInfo fn;
};

struct Parser {
    std::string rel;
    std::string code;    // comments, strings and char literals blanked
    std::string bodies;  // comments blanked, literals kept
    std::vector<int> newlines;
    std::map<int, Found> found;  // keyed by the body's `{`

    int line_of(int pos) const {
        return static_cast<int>(std::lower_bound(newlines.begin(), newlines.end(), pos) - newlines.begin()) + 1;
    }

    Found* add(int head_start, int brace, const std::string& name, const std::string& kind,
               std::string_view signature, std::vector<std::pair<std::string, std::string>> params,
               const std::string& return_type, bool is_static) {
        if (auto it = found.find(brace); it != found.end()) return &it->second;
        int close = match_brace(code, brace);
        if (close < 0) return nullptr;
        Found f;
        f.head_start = head_start;
        f.close = close;
        f.fn.file = rel;
        f.fn.name = name;
        f.fn.kind = kind;
        f.fn.line = line_of(head_start);
        f.fn.signature = collapse_ws(signature);
        f.fn.params = std::move(params);
        f.fn.return_type = return_type;
        f.fn.is_static = is_static;
        // Discovery blanks strings so `"int foo("` is not a function. Bodies
        // keep literals so strcpy/snprintf oracles see the bytes.
        f.fn.body = bodies.substr(static_cast<std::size_t>(brace + 1),
                                  static_cast<std::size_t>(close - brace - 1));
        f.fn.span = {f.fn.line, line_of(close)};
        return &found.emplace(brace, std::move(f)).first->second;
    }

    void heads() {
        static Regex head(FUNC_HEAD_PAT, true);
        static Regex knr_params(KNR_PARAMS_PAT);
        static Regex cxx_mods(R"(\b(?:virtual|explicit|friend|consteval)\b)");
        std::size_t pos = 0;
        while (pos <= code.size()) {
            auto m = head.search_match(code, pos);
            if (!m) break;
            auto name = norm_name(m->named("name"));
            auto ret = trim(m->named("ret"));
            if (ret.empty()) ret = "int";
            auto knr = trim(m->named("knr"));
            auto params_text = m->named("params");
            auto last = name.rfind("::") == std::string::npos ? name : name.substr(name.rfind("::") + 2);
            if (KW.contains(last) || KW.contains(ret) || NOT_A_TYPE.contains(ret) ||
                (!knr.empty() && (!full_match(knr_params, params_text) || trim(params_text) == "void"))) {
                // Not a definition. Resume on the next line: this match may
                // have run over a real head.
                auto nl = code.find('\n', static_cast<std::size_t>(m->spans[0].first) + 1);
                if (nl == std::string::npos) break;
                pos = nl + 1;
                continue;
            }
            auto params = split_params(params_text);
            auto stars = m->named("stars");
            auto mods = m->named("mods");
            auto attrs = m->named("attrs");
            auto kind = kind_of(ret, stars, params);
            if (name.find("::") != std::string::npos || ret.find("::") != std::string::npos ||
                ret.find('<') != std::string::npos || ret == "auto" || name.starts_with("operator") ||
                !m->named("tmpl").empty() || stars.find('&') != std::string::npos ||
                attrs.find('&') != std::string::npos || attrs.find("->") != std::string::npos ||
                cxx_mods.search(mods) || !knr.empty() || !m->named("cc").empty()) {
                // A method, template, K&R, calling-convention or C++-typed
                // definition: BMC must not model it as a plain C function.
                kind = "OTHER";
            }
            int head_start = m->spans[0].first;
            while (head_start < m->spans[0].second &&
                   (code[static_cast<std::size_t>(head_start)] == ' ' ||
                    code[static_cast<std::size_t>(head_start)] == '\t'))
                ++head_start;
            int brace = m->spans[0].second - 1;
            auto sig = code.substr(static_cast<std::size_t>(head_start),
                                   static_cast<std::size_t>(brace - head_start));
            add(head_start, brace, name, kind, sig, std::move(params), trim(trim(stars) + " " + ret),
                mods.find("static") != std::string::npos);
            pos = static_cast<std::size_t>(m->spans[0].second);
        }
    }

    // A definition FUNC_HEAD missed, recognized from its declarator.
    Found* scan_definition(const std::string& head, int start, int brace, const std::string& qual,
                           bool in_class) {
        static Regex ptr_decl(FUNC_PTR_DECL_PAT);
        static Regex ctor_decl(CTOR_DECL_PAT);
        static Regex member_decl(MEMBER_DECL_PAT);
        static Regex tmpl_tail(R"(<.*$)");
        if (auto m = match_at_start(ptr_decl, head);
            m && !KW.contains(m->named("name")) && !KW.contains(m->named("ret"))) {
            auto ret = trim(m->named("ret"));
            return add(start, brace, m->named("name"), "OTHER", head, split_params(m->named("params")),
                       ret + " " + m->named("stars") + "(*)(" + collapse_ws(m->named("fparams")) + ")",
                       m->named("mods").find("static") != std::string::npos);
        }
        if (auto m = match_at_start(ctor_decl, head)) {
            auto name = norm_name(m->named("name"));
            std::vector<std::string> parts;
            std::size_t a = 0;
            while (true) {
                auto b = name.find("::", a);
                parts.push_back(regex_sub(tmpl_tail, "", name.substr(a, b == std::string::npos ? b : b - a)));
                if (b == std::string::npos) break;
                a = b + 2;
            }
            if (parts.size() >= 2 && (parts.back().starts_with("~") || parts.back() == parts[parts.size() - 2]))
                return add(start, brace, name, "OTHER", head, split_params(m->named("params")), "", false);
        }
        if (in_class) {
            if (auto m = match_at_start(member_decl, head); m && !KW.contains(m->named("name"))) {
                auto name = norm_name(m->named("name"));
                return add(start, brace, qual.empty() ? name : qual + "::" + name, "OTHER", head,
                           split_params(m->named("params")), collapse_ws(m->named("ret")), false);
            }
        }
        return nullptr;
    }

    // Walks file scope and namespace, extern "C" and class bodies. Every `{`
    // there is a function body FUNC_HEAD found, a container, an initializer
    // or type body, a definition only the declarator regexes recognize, or a
    // gap: code no per-function stage sees (Law 7).
    std::vector<std::pair<int, std::string>> scope_scan() {
        static Regex access_label(
            R"(\s*(?:(?:public|private|protected)(?:\s+(?:slots|Q_SLOTS))?|signals|Q_SIGNALS)\s*:(?!:))");
        static Regex namespace_head(R"((?:^|\s)namespace\b)");
        static Regex extern_head(R"(\s*extern\s*"[^"]*"\s*\Z)");
        static Regex type_head(R"(\s*(?:template\s*)" + TMPL +
                               R"(\s*)?(?:typedef\s+)?(?:(?P<enum>enum)|class|struct|union)\b)");
        static Regex class_name(R"(\s*(?:template\s*)" + TMPL +
                                R"(\s*)?(?:typedef\s+)?(?:class|struct|union)\s+(?:)" + PRE_ATTR +
                                R"(\s*|alignas\s*\([^;{}()]*\)\s*)*(?P<name>[A-Za-z_]\w*))");
        static Regex pre_attr(PRE_ATTR + R"(|alignas\s*\([^;{}()]*\))");
        auto text = blank_preprocessor(code);
        std::vector<std::pair<int, std::string>> gaps;
        std::vector<std::pair<bool, std::string>> stack;  // (is_class, qualified class name)
        std::size_t head_start = 0;
        std::size_t pos = 0;
        while (pos < text.size()) {
            auto k = text.find_first_of("{};", pos);
            if (k == std::string::npos) break;
            char c = text[k];
            int brace = static_cast<int>(k);
            pos = k + 1;
            if (c == ';') {
                head_start = pos;
                continue;
            }
            if (c == '}') {
                if (!stack.empty()) stack.pop_back();
                head_start = pos;
                continue;
            }
            bool in_class = !stack.empty() && stack.back().first;
            std::string qual = stack.empty() ? std::string() : stack.back().second;
            std::string head = text.substr(head_start, k - head_start);
            if (auto lab = match_at_start(access_label, head))
                head = head.substr(static_cast<std::size_t>(lab->spans[0].second));
            std::size_t lead = 0;
            while (lead < head.size() && std::isspace(static_cast<unsigned char>(head[lead]))) ++lead;
            int start = brace - static_cast<int>(head.size()) + static_cast<int>(lead);
            head_start = pos;
            auto shape = head_shape(head);
            Found* hit = nullptr;
            if (auto it = found.find(brace); it != found.end()) hit = &it->second;
            else if (shape.find('(') != std::string::npos && shape.find('=') == std::string::npos)
                hit = scan_definition(head, start, brace, qual, in_class);
            if (hit) {
                if (in_class) {
                    if (!qual.empty() && hit->fn.name.find("::") == std::string::npos)
                        hit->fn.name = qual + "::" + hit->fn.name;
                    hit->fn.kind = "OTHER";
                }
                pos = head_start = static_cast<std::size_t>(hit->close) + 1;
                continue;
            }
            if (match_at_start(extern_head, head) ||
                (namespace_head.search(head) && head.find('(') == std::string::npos)) {
                stack.emplace_back(false, qual);
                continue;
            }
            auto tm = match_at_start(type_head, head);
            bool is_type = tm && shape.find('=') == std::string::npos &&
                           (!tm->named("enum").empty() ||
                            regex_sub(pre_attr, "", head).find('(') == std::string::npos);
            if (is_type && tm->named("enum").empty()) {
                auto cm = match_at_start(class_name, head);
                std::string cname = cm ? cm->named("name") : std::string();
                if (!cname.empty() && !qual.empty()) cname = qual + "::" + cname;
                stack.emplace_back(true, cname);
                continue;
            }
            if (!is_type && shape.find('(') != std::string::npos && shape.find('=') == std::string::npos) {
                auto t = trim(head);
                auto first = trim(t.substr(0, t.find('\n')));
                gaps.emplace_back(line_of(start), first.substr(0, 80));
            }
            // Initializer, enum or brace-init body, or a gap: skip it whole.
            int close = match_brace(text, brace);
            if (close < 0) break;
            pos = head_start = static_cast<std::size_t>(close) + 1;
        }
        return gaps;
    }
};

std::pair<std::vector<FunctionInfo>, std::vector<std::pair<int, std::string>>> parse_text(
    std::string_view text, const std::string& rel) {
    Parser p;
    p.rel = rel;
    p.code = blank_char_literals(strip_comments_keep_lines(text, true));
    p.bodies = strip_comments_keep_lines(text, false);
    for (std::size_t i = 0; i < p.code.size(); ++i)
        if (p.code[i] == '\n') p.newlines.push_back(static_cast<int>(i));
    p.heads();
    auto gaps = p.scope_scan();
    std::vector<std::pair<std::pair<int, int>, FunctionInfo*>> order;
    for (auto& [brace, f] : p.found) order.push_back({{f.head_start, brace}, &f.fn});
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<FunctionInfo> out;
    out.reserve(order.size());
    for (auto& [_, fn] : order) out.push_back(std::move(*fn));
    return {std::move(out), std::move(gaps)};
}

}  // namespace

bool is_c_ext(std::string_view ext) {
    auto e = to_lower(std::string(ext));
    for (auto* p = C_EXTS; *p; ++p)
        if (e == *p) return true;
    return false;
}

bool is_tu_ext(std::string_view ext) {
    auto e = to_lower(std::string(ext));
    for (auto* p = TU_EXTS; *p; ++p)
        if (e == *p) return true;
    return false;
}

std::string strip_comments_keep_lines(std::string_view text, bool blank_strings) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0, n = text.size();
    bool in_block = false;
    while (i < n) {
        if (in_block) {
            if (i + 1 < n && text[i] == '*' && text[i + 1] == '/') {
                in_block = false;
                i += 2;
            } else {
                out.push_back(text[i] == '\n' ? '\n' : ' ');
                ++i;
            }
            continue;
        }
        if (i + 1 < n && text[i] == '/' && text[i + 1] == '*') {
            in_block = true;
            i += 2;
            continue;
        }
        if (i + 1 < n && text[i] == '/' && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') {
                out.push_back(' ');
                ++i;
            }
            continue;
        }
        char c = text[i];
        if (c == '\'') {
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '\'') {
                if (text[i] == '\\') {
                    out.push_back(text[i]);
                    ++i;
                    if (i < n) {
                        out.push_back(text[i]);
                        ++i;
                    }
                    continue;
                }
                out.push_back(text[i]);
                ++i;
            }
            if (i < n) {
                out.push_back(text[i]);
                ++i;
            }
            continue;
        }
        if (c == '"') {
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '"') {
                if (text[i] == '\\') {
                    if (blank_strings) {
                        out.push_back(' ');
                        ++i;
                        if (i < n) {
                            out.push_back(' ');
                            ++i;
                        }
                    } else {
                        out.push_back(text[i]);
                        ++i;
                        if (i < n) {
                            out.push_back(text[i]);
                            ++i;
                        }
                    }
                    continue;
                }
                out.push_back(blank_strings ? (text[i] == '\n' ? '\n' : ' ') : text[i]);
                ++i;
            }
            if (i < n) {
                out.push_back(text[i]);
                ++i;
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

int match_brace(std::string_view text, int open_idx) {
    int depth = 0;
    int n = static_cast<int>(text.size());
    for (int i = open_idx; i < n; ++i) {
        if (text[static_cast<std::size_t>(i)] == '{') ++depth;
        else if (text[static_cast<std::size_t>(i)] == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return -1;
}

bool body_returns_local_array(std::string_view body) {
    if (body.empty()) return false;
    static Regex local_arr(
        "(?m)^[ \\t]*(?P<static>static\\s+)?"
        "(?:const\\s+|volatile\\s+)*"
        "(?:unsigned\\s+|signed\\s+|long\\s+|short\\s+)*"
        "(?:struct\\s+\\w+|union\\s+\\w+|enum\\s+\\w+|"
        "char|int|short|long|float|double|void|"
        "size_t|ssize_t|ptrdiff_t|uint\\w*|int\\w*|wchar_t|_Bool|bool)\\s+"
        "(?P<name>[A-Za-z_]\\w*)\\s*\\[",
        true);
    static Regex ret_decay("\\breturn\\s+\\(*\\s*([A-Za-z_]\\w*)\\s*\\)*\\s*(?:;|[+\\-])");
    static Regex ret_decay_rhs("\\breturn\\s+[^;]*[+\\-]\\s*\\(*\\s*([A-Za-z_]\\w*)\\s*\\)*\\s*;");
    std::unordered_set<std::string> arrays;
    for (auto& m : local_arr.finditer(body)) {
        if (m.named("static").empty()) arrays.insert(m.named("name"));
    }
    if (arrays.empty()) return false;
    for (auto& m : ret_decay.finditer(body))
        if (arrays.contains(m.group(1))) return true;
    for (auto& m : ret_decay_rhs.finditer(body))
        if (arrays.contains(m.group(1))) return true;
    return false;
}

bool body_needs_pointer_harness(std::string_view body) {
    if (body.empty()) return false;
    static Regex local_ptr(
        "\\b(?:struct\\s+\\w+|union\\s+\\w+|void|char|int|short|long|unsigned|signed"
        "|size_t|uint\\w*|int\\w*|FILE|DIR)\\s+\\*\\s*[A-Za-z_]"
        "|\\b(?:malloc|calloc|realloc|reallocarray|strdup|getenv|fopen"
        "|popen|alloca|__builtin_alloca)\\s*\\(");
    if (local_ptr.search(body)) return true;
    if (re_search("return\\s*\\(*\\s*&", body)) return true;
    return body_returns_local_array(body);
}

std::vector<FunctionInfo> extract_functions(const std::filesystem::path& path, std::string rel) {
    auto text = read_file(path);
    if (rel.empty()) rel = path.string();
    return parse_text(text, rel).first;
}

std::vector<std::pair<int, std::string>> parse_gaps(const std::filesystem::path& path) {
    auto text = read_file(path);
    return parse_text(text, path.string()).second;
}

std::vector<Finding> parse_gap_findings(const std::filesystem::path& path, const std::string& rel) {
    std::vector<Finding> out;
    for (auto& [line, head] : parse_gaps(path)) {
        Finding f;
        f.stage = "inventory";
        f.status = std::string(laws::NOTRUN);
        f.file = rel;
        f.line = line;
        f.cls = "PARSE-GAP";
        f.message = "line " + std::to_string(line) +
                    ": code in braces not attributed to any function; not checked: " + head;
        f.strength = std::string(laws::STRENGTH_FINDS);
        out.push_back(std::move(f));
    }
    return out;
}

bool tu_is_empty(const std::filesystem::path& path) {
    auto ext = to_lower(path.extension().string());
    if (!is_tu_ext(ext)) return false;
    return extract_functions(path).empty();
}

std::vector<std::filesystem::path> iter_sources(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_regular_file(root)) return {root};
    if (!std::filesystem::exists(root)) return files;
    for (auto it = std::filesystem::recursive_directory_iterator(root);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory() && scope::skip_dir(it->path().filename().string())) {
            it.disable_recursion_pending();
            continue;
        }
        if (scope::skipped_path(it->path(), root)) continue;
        if (it->is_regular_file() && is_c_ext(it->path().extension().string()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace prism
