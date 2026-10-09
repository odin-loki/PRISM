#include "prism/cparse.hpp"
#include "prism/scope.hpp"

#include "prism/laws.hpp"
#include "prism/regex.hpp"

#include <algorithm>
#include <array>
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
// `operator "" _sr` is a user-defined literal (the `""` stays: strings are
// blanked inside, their quotes kept).
const std::string OPERATOR =
    R"(operator\s*(?:""\s*[A-Za-z_]\w*|\(\s*\)|\[\s*\]|new(?:\s*\[\s*\])?|delete(?:\s*\[\s*\])?)"
    R"(|[^\s\w(){};]{1,3}))";

// Words that are a type or qualifier themselves, never a storage macro.
const std::string C_TYPE_WORDS =
    R"((?:int|char|short|long|float|double|void|signed|unsigned|struct|union|enum)"
    R"(|const|volatile|bool|_Bool|static|inline|extern|return|else|case|goto|typedef))";
const std::string FUNC_HEAD_PAT =
    R"((?m)^[ \t]*)"
    R"((?P<head>)"
    R"((?P<tmpl>template[ \t]*)" + TMPL + R"([ \t]*)?)"
    R"((?P<mods>(?:(?:static|inline|extern|constexpr|consteval|virtual|)"
    R"(explicit|friend|unsigned|signed|const|volatile|restrict|)"
    R"(_Noreturn|__inline|__inline__|__forceinline|thread_local|)"
    R"(__extension__)\s+|)" + PRE_ATTR + R"([ \t]*)"
    // A language linkage: `extern "C" int` (the name may be on the next line).
    R"(|extern[ \t]*"[^"\n]*"\s+)"
    // A leading export macro before a lowercase type: `JSMN_API int f(`.
    R"(|[A-Z_][A-Z0-9_]*[ \t]+(?=[a-z]))"
    // A lowercase storage macro before a lowercase type and the name:
    // zlib's `local block_state deflate_stored(` (#define local static).
    R"(|(?!)" + C_TYPE_WORDS + R"(\b)[a-z_]\w*[ \t]+)"
    R"((?=(?:[a-z_]\w*[ \t*]+)+[A-Za-z_]\w*[ \t]*\())*))"
    R"((?P<ret>(?:(?:struct|enum|union|class|typename)\s+)?)"
    R"((?:long\s+long|long\s+(?:int|double)\b|short\s+int\b)"
    R"(|[A-Za-z_]\w*(?:\s*)" + TMPL + R"()?)"
    R"((?:\s*::\s*[A-Za-z_]\w*(?:\s*)" + TMPL + R"()?)*)))"
    R"((?P<stars>(?:\s*(?:[*&]|\b(?:const|volatile)\b))+\s*|\s+))"
    // Calling-convention / export / pointer-size macros, with the `*` that
    // may sit between them and the name: `Z3_ast Z3_API Z3_mk_add(`, zlib's
    // `const z_crc_t FAR * ZEXPORT get_crc_table(` and
    // `char ZLIB_INTERNAL *gz_strwinerror(`.
    // Two characters at least: a template parameter `T&` is a type.
    R"((?P<cc>(?:(?:[A-Z_][A-Z0-9_]+|__\w+)[ \t*&]+)*))"
    // A parenthesised name keeps a function-like macro off it: `int (min)()`.
    R"((?P<name>)" + QUAL + R"((?:)" + OPERATOR + R"(|[A-Za-z_]\w*|\([ \t]*[A-Za-z_]\w*[ \t]*\)))\s*)"
    R"(\((?P<params>)" + PARAMS + R"()\))"
    // K&R parameter declarations, a function pointer too: `void (*init)(void);`.
    R"((?P<knr>(?:\s*(?:register\s+)?[A-Za-z_][\w \t\n*,\[\]()]*;)*))"
    R"((?P<attrs>)" + ATTR + R"())"
    R"(\s*))"
    // A function-try-block: `void f(int &x) try { ... } catch (...) { ... }`.
    R"((?P<ftry>try\s*)?)"
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
    R"(|~?[A-Za-z_]\w*|\([ \t]*[A-Za-z_]\w*[ \t]*\)|)" + OPERATOR + R"()\s*)"
    R"(\((?P<params>)" + PARAMS + R"()\))" + ATTR +
    R"((?:\s*:(?!:)[^;{}]*)?\s*\Z)";

bool is_pointer_type(std::string_view typ) {
    return typ.find('*') != std::string_view::npos || typ.find('[') != std::string_view::npos;
}

// C23 / C++14 digit separator: a `'` inside a pp-number (`1'000`,
// `0xFF'FF`), not the start of a character literal. The token before it
// starts with a digit (or `.digit`) and a digit or letter follows it;
// `u8'a'`, `L'a'` and `c=='0'` stay character literals.
bool is_digit_separator(std::string_view text, std::size_t i) {
    if (i == 0 || i + 1 >= text.size() || text[i] != '\'') return false;
    auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    if (!alnum(text[i - 1]) || !alnum(text[i + 1])) return false;
    std::size_t j = i;
    while (j > 0 && (alnum(text[j - 1]) || text[j - 1] == '_' || text[j - 1] == '.' || text[j - 1] == '\''))
        --j;
    if (std::isdigit(static_cast<unsigned char>(text[j]))) return true;
    return text[j] == '.' && j + 1 < i && std::isdigit(static_cast<unsigned char>(text[j + 1]));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return scrub_utf8(ss.str());
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trim(std::string_view s);

// The one spelling of a parameter type every consumer reads: const,
// volatile, restrict and register dropped as words; no whitespace except one
// space between two words and one before a run of `*`/`&` (`const char*s`
// -> `char *`, `std::vector< int > &v` -> `std::vector<int> &`,
// `unsigned  long` -> `unsigned long`). A run the source binds to the type
// (`char* p`, `const std::vector<int>& v`) keeps that spelling: `char*`,
// `std::vector<int>&`. Template spelling stays whole, so a
// lint's `\bvector\s*<\w` still reads `vector<int>`.
std::string canonical_type(std::string_view raw) {
    std::string out;
    std::size_t i = 0, n = raw.size();
    char prev = 0;  // last token emitted: 'w' word, 'p' `*`/`&`, 'o' other
    while (i < n) {
        char c = raw[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (is_word_char(c)) {
            std::size_t j = i;
            while (j < n && is_word_char(raw[j])) ++j;
            std::string_view w = raw.substr(i, j - i);
            i = j;
            if (w == "const" || w == "volatile" || w == "restrict" || w == "register" ||
                w == "__restrict" || w == "__restrict__")
                continue;
            if (prev == 'w') out.push_back(' ');
            out.append(w);
            prev = 'w';
            continue;
        }
        if (c == '*' || c == '&') {
            // A run of `*`/`&` after a type gets one space before it, unless
            // the source binds it to the type and not the name (`char* p`,
            // `std::vector<int>& v` keep `char*`, `std::vector<int>&`).
            bool after_type = prev == 'w' || (prev == 'o' && (out.back() == '>' || out.back() == ')' || out.back() == ']'));
            if (after_type) {
                bool space_before = i > 0 && std::isspace(static_cast<unsigned char>(raw[i - 1]));
                std::size_t k = i, last = i;
                while (k < n && (raw[k] == '*' || raw[k] == '&' || std::isspace(static_cast<unsigned char>(raw[k])))) {
                    if (raw[k] == '*' || raw[k] == '&') last = k;
                    ++k;
                }
                bool space_after = last + 1 < n && std::isspace(static_cast<unsigned char>(raw[last + 1]));
                if (space_before || !space_after) out.push_back(' ');
            }
            out.push_back(c);
            prev = 'p';
            ++i;
            continue;
        }
        out.push_back(c);
        prev = 'o';
        ++i;
    }
    return out;
}

// Split at top-level commas: a comma inside <>, (), [] or {} belongs to one
// parameter (`std::map<int, int> m`, `void (*cb)(int, int)`). After a
// default argument's `=`, `<` and `>` are operators (`int n = a < b`).
std::vector<std::string> split_top_level(std::string_view params) {
    std::vector<std::string> out;
    int angle = 0, other = 0;
    bool in_default = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        char c = params[i];
        if (c == '(' || c == '[' || c == '{') ++other;
        else if ((c == ')' || c == ']' || c == '}') && other > 0) --other;
        else if (c == '<' && !in_default) ++angle;
        else if (c == '>' && !in_default && angle > 0) --angle;
        else if (c == '=' && angle == 0 && other == 0) in_default = true;
        else if (c == ',' && other == 0 && (angle == 0 || in_default)) {
            out.emplace_back(params.substr(start, i - start));
            start = i + 1;
            angle = 0;
            in_default = false;
        }
    }
    out.emplace_back(params.substr(start));
    return out;
}

std::vector<std::pair<std::string, std::string>> split_params(std::string_view params_in) {
    auto params = trim(params_in);
    if (params.empty() || params == "void") return {};
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& piece : split_top_level(params)) {
        auto raw = trim(piece);
        if (raw.empty() || raw == "...") continue;
        // C++ default argument: the first `=` outside brackets.
        int depth = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '(' || c == '[' || c == '{' || c == '<') ++depth;
            else if ((c == ')' || c == ']' || c == '}' || c == '>') && depth > 0) --depth;
            else if (c == '=' && depth == 0) {
                raw = trim(std::string_view(raw).substr(0, i));
                break;
            }
        }
        auto canon = canonical_type(raw);
        // The name is the last word. Text that ends in a bracket or a paren
        // (`int a[4]`, `void (*cb)(int)`) has none. A lone word keeps the
        // old reading, name and type both the word: a K&R identifier list
        // (`f(s, flush)`) names its parameters that way.
        std::size_t b = canon.size();
        while (b > 0 && is_word_char(canon[b - 1])) --b;
        if (b == canon.size() || std::isdigit(static_cast<unsigned char>(canon[b]))) {
            out.emplace_back(canon, "");
            continue;
        }
        auto typ = trim(std::string_view(canon).substr(0, b));
        out.emplace_back(typ.empty() ? canon : typ, canon.substr(b));
    }
    return out;
}

std::string kind_of(const std::string& ret, const std::string& stars,
                    const std::vector<std::pair<std::string, std::string>>& params) {
    (void)ret;
    (void)stars;
    if (params.empty()) return "VOID";
    auto param_ok = [](std::string t) -> std::string {
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
        // Qualifiers are words, not substrings: `constant_t` stays a word.
        std::erase_if(words, [](const std::string& word) { return word == "const" || word == "volatile"; });
        if (words.empty()) return "OTHER";
        for (auto& word : words)
            if (!SCALAR_WORDS.contains(word)) return "OTHER";  // struct by value, unknown typedef
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
// `(min)` -> `min`, `operator "" _sr` -> `operator""_sr`.
std::string norm_name(std::string_view s) {
    static Regex colons(R"(\s*::\s*)");
    static Regex op(R"(\boperator\s+(?=[^\w\s]))");
    static Regex udl(R"(""\s+)");
    static Regex paren_name(R"(\(\s*([A-Za-z_]\w*)\s*\)$)");
    auto out = regex_sub(op, "operator", regex_sub(colons, "::", collapse_ws(s)));
    if (out.find("\"\"") != std::string::npos) out = regex_sub(udl, "\"\"", out);
    if (!out.empty() && out.back() == ')')
        if (auto m = paren_name.search_match(out))
            out = out.substr(0, static_cast<std::size_t>(m->spans[0].first)) + m->group(1);
    return out;
}

// Character literal interiors as spaces, quotes kept (`'}'` -> `' '`).
// A digit separator (`1'000`) is not a literal.
std::string blank_char_literals(std::string text) {
    if (text.find('\'') == std::string::npos) return text;
    const auto n = text.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (text[i] != '\'' || is_digit_separator(text, i)) continue;
        std::size_t k = i + 1;
        while (k < n && text[k] != '\'' && text[k] != '\n') k += text[k] == '\\' && k + 1 < n && text[k + 1] != '\n' ? 2 : 1;
        if (k >= n || text[k] != '\'') continue;  // unterminated on its line: left as it is
        for (auto q = i + 1; q < k; ++q) text[q] = ' ';
        i = k;
    }
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

// Leading lines that are a whole ALL_CAPS macro use with no `;`
// (`CXXOPTS_DIAGNOSTIC_PUSH`, `CXXOPTS_IGNORE_WARNING("...")`) before a
// declaration on the lines after: the length of that prefix. A macro line
// with nothing after it (`TEST(a, b)` right before `{`) is the head itself.
std::size_t macro_line_prefix(std::string_view head) {
    static Regex line(R"([ \t]*[A-Z_][A-Z0-9_]*[ \t]*(?:\([^()\n]*\))?[ \t]*)");
    std::size_t cut = 0;
    for (;;) {
        std::size_t p = cut;
        while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) {
            if (head[p] == '\n') cut = p + 1;
            ++p;
        }
        auto nl = head.find('\n', p);
        if (nl == std::string_view::npos) return cut;
        auto ln = head.substr(cut, nl - cut);
        if (!full_match(line, ln)) return cut;
        auto rest = head.substr(nl + 1);
        if (rest.find_first_not_of(" \t\r\n") == std::string_view::npos) return cut;
        cut = nl + 1;
    }
}

// `decltype(...)`, `sizeof(...)`, `alignof(...)`, `noexcept(...)` groups
// removed: a class head's base or template argument, not a parameter list
// (`struct is_range<T, decltype(begin(x))> : std::true_type {`).
std::string drop_type_operators(std::string head) {
    static Regex op(R"(\b(?:decltype|sizeof|alignof|noexcept)\s*\()");
    for (;;) {
        auto m = op.search_match(head);
        if (!m) return head;
        auto a = static_cast<std::size_t>(m->spans[0].first);
        auto k = static_cast<std::size_t>(m->spans[0].second) - 1;  // the `(`
        int depth = 0;
        std::size_t e = head.size();
        for (auto i = k; i < head.size(); ++i) {
            if (head[i] == '(') ++depth;
            else if (head[i] == ')' && --depth == 0) {
                e = i + 1;
                break;
            }
        }
        head.replace(a, e - a, " ");
    }
}

// `Bin(int r) : Base{ true, r }, m_lhs(r) {}`: the `{` after a
// mem-initializer's name is its braced initializer, not the body. When the
// head ends in such a name (a `:` initializer list after the parameters,
// its last item with no parentheses), the body is the first `{` after the
// remaining initializers; `brace` itself otherwise, and whenever the list
// does not read cleanly.
int ctor_body_brace(std::string_view text, std::string_view head, int brace) {
    int depth = 0;
    bool seen_close = false;
    std::size_t colon = std::string_view::npos;
    for (std::size_t i = 0; i < head.size(); ++i) {
        char c = head[i];
        if (c == '(') ++depth;
        else if (c == ')') {
            if (--depth == 0) seen_close = true;
        } else if (c == ':' && depth == 0 && seen_close) {
            bool dbl = (i + 1 < head.size() && head[i + 1] == ':') || (i > 0 && head[i - 1] == ':');
            if (!dbl) {
                colon = i;
                break;
            }
        }
    }
    if (colon == std::string_view::npos) return brace;
    auto init = head.substr(colon + 1);
    int d = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < init.size(); ++i) {
        char c = init[i];
        if (c == '(' || c == '<' || c == '[') ++d;
        else if ((c == ')' || c == '>' || c == ']') && d > 0) --d;
        else if (c == ',' && d == 0) last = i + 1;
    }
    static Regex item(R"(\s*(?:[A-Za-z_]\w*\s*(?:<[^(){};]*>)?\s*::\s*)*[A-Za-z_]\w*\s*(?:<[^(){};]*>)?\s*)");
    if (!full_match(item, init.substr(last))) return brace;
    const auto n = static_cast<int>(text.size());
    auto skip_ws = [&](int k) {
        while (k < n && std::isspace(static_cast<unsigned char>(text[static_cast<std::size_t>(k)]))) ++k;
        return k;
    };
    int close = match_brace(text, brace);
    for (int guard = 0; close >= 0 && guard < 256; ++guard) {
        int k = skip_ws(close + 1);
        if (k >= n) return brace;
        char c = text[static_cast<std::size_t>(k)];
        if (c == '{') return k;
        if (c != ',') return brace;
        // The next initializer: a (qualified, templated) name, then `(` or `{`.
        int ad = 0;
        for (++k; k < n; ++k) {
            char q = text[static_cast<std::size_t>(k)];
            if (q == '<') ++ad;
            else if (q == '>' && ad > 0) --ad;
            else if (ad == 0 && (q == '(' || q == '{')) break;
            else if (q == ';' || q == '}' || q == ')') return brace;
        }
        if (k >= n) return brace;
        if (text[static_cast<std::size_t>(k)] == '{') {
            close = match_brace(text, k);
        } else {
            int pd = 0;
            close = -1;
            for (int i = k; i < n; ++i) {
                char q = text[static_cast<std::size_t>(i)];
                if (q == '(') ++pd;
                else if (q == ')' && --pd == 0) {
                    close = i;
                    break;
                }
            }
        }
    }
    return brace;
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
        f.fn.body_line = line_of(brace + 1);
        f.fn.body_col = brace + 1 - (f.fn.body_line > 1 ? newlines[static_cast<std::size_t>(f.fn.body_line - 2)] : -1);
        return &found.emplace(brace, std::move(f)).first->second;
    }

    // A function-try-block (`) try {`): the body is the try block and every
    // handler after it, `try { ... } catch (...) { ... }`, as written.
    void extend_try(Found& f, int try_pos) {
        auto n = static_cast<int>(code.size());
        auto skip_ws = [&](int k) {
            while (k < n && std::isspace(static_cast<unsigned char>(code[static_cast<std::size_t>(k)]))) ++k;
            return k;
        };
        int close = f.close;
        for (;;) {
            int k = skip_ws(close + 1);
            if (code.compare(static_cast<std::size_t>(k), 5, "catch") != 0) break;
            k = skip_ws(k + 5);
            if (k >= n || code[static_cast<std::size_t>(k)] != '(') break;
            int depth = 0, pc = -1;
            for (int i = k; i < n; ++i) {
                char c = code[static_cast<std::size_t>(i)];
                if (c == '(') ++depth;
                else if (c == ')' && --depth == 0) { pc = i; break; }
            }
            if (pc < 0) break;
            k = skip_ws(pc + 1);
            if (k >= n || code[static_cast<std::size_t>(k)] != '{') break;
            int bc = match_brace(code, k);
            if (bc < 0) break;
            close = bc;
        }
        f.close = close;
        f.fn.body = bodies.substr(static_cast<std::size_t>(try_pos), static_cast<std::size_t>(close + 1 - try_pos));
        f.fn.span.second = line_of(close);
        f.fn.body_line = line_of(try_pos);
        f.fn.body_col = try_pos - (f.fn.body_line > 1 ? newlines[static_cast<std::size_t>(f.fn.body_line - 2)] : -1);
    }

    // `template <typename T>` alone on the line(s) before `pos`.
    bool template_line_before(int pos) const {
        static Regex tmpl(R"(\btemplate\s*<[^;{}]*>\s*\Z)");
        int k = pos;
        while (k > 0 && std::isspace(static_cast<unsigned char>(code[static_cast<std::size_t>(k - 1)]))) --k;
        if (k == 0 || code[static_cast<std::size_t>(k - 1)] != '>') return false;
        auto from = code.find_last_of(";{}", static_cast<std::size_t>(k - 1));
        from = from == std::string::npos ? 0 : from + 1;
        return tmpl.search(std::string_view(code).substr(from, static_cast<std::size_t>(k) - from));
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
            for (char c : m->named("cc"))
                if (c == '*' || c == '&') stars.push_back(c);
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
            if (kind != "OTHER" && template_line_before(head_start)) kind = "OTHER";  // a template
            int brace = m->spans[0].second - 1;
            auto ftry = m->named("ftry");
            int try_pos = ftry.empty() ? -1 : brace - static_cast<int>(ftry.size());
            auto sig = code.substr(static_cast<std::size_t>(head_start),
                                   static_cast<std::size_t>((try_pos >= 0 ? try_pos : brace) - head_start));
            auto* hit = add(head_start, brace, name, kind, sig, std::move(params), trim(trim(stars) + " " + ret),
                            mods.find("static") != std::string::npos);
            if (hit && try_pos >= 0) {
                extend_try(*hit, try_pos);
                pos = static_cast<std::size_t>(hit->close) + 1;
                continue;
            }
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
            return nullptr;
        }
        // `TEST(Suite, Name) {` (Unity fixture, GoogleTest) and other ALL_CAPS
        // macros that define a function: the body is code, checked by the
        // lints; OTHER, so no stage models it as a plain C function.
        static Regex macro_def(R"(\s*(?P<mac>[A-Z_][A-Z0-9_]*)\s*\((?P<args>[^()]*)\)\s*\Z)");
        if (auto m = match_at_start(macro_def, head)) {
            std::string name = m->named("mac"), word;
            auto args = m->named("args") + " ";
            for (char c : args) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                    word.push_back(c);
                } else if (!word.empty()) {
                    name += "_" + word;
                    word.clear();
                }
            }
            return add(start, brace, name, "OTHER", head, {}, "void", false);
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
        std::size_t decl_start = 0;  // after the last `{` / `}` at this level (`;` does not move it)
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
                head_start = decl_start = pos;
                continue;
            }
            bool in_class = !stack.empty() && stack.back().first;
            std::string qual = stack.empty() ? std::string() : stack.back().second;
            const bool after_semicolon = head_start > decl_start && text[head_start - 1] == ';';
            std::string head = text.substr(head_start, k - head_start);
            if (auto lab = match_at_start(access_label, head))
                head = head.substr(static_cast<std::size_t>(lab->spans[0].second));
            // The head before macro lines are cut: the gap decision falls back
            // to it when the cut head is neither a definition nor a type.
            const std::string full_head = head;
            const std::size_t macro_cut = macro_line_prefix(head);
            if (macro_cut) head = head.substr(macro_cut);
            std::size_t lead = 0;
            while (lead < head.size() && std::isspace(static_cast<unsigned char>(head[lead]))) ++lead;
            int start = brace - static_cast<int>(head.size()) + static_cast<int>(lead);
            head_start = pos;
            auto shape = head_shape(head);
            Found* hit = nullptr;
            if (auto it = found.find(brace); it != found.end()) hit = &it->second;
            else if (shape.find('(') != std::string::npos && shape.find('=') == std::string::npos) {
                int body = ctor_body_brace(text, head, brace);
                if (body != brace) {
                    if (auto it = found.find(body); it != found.end()) hit = &it->second;
                    else hit = scan_definition(head, start, body, qual, in_class);
                } else {
                    hit = scan_definition(head, start, brace, qual, in_class);
                }
            }
            if (hit) {
                if (in_class) {
                    if (!qual.empty() && hit->fn.name.find("::") == std::string::npos)
                        hit->fn.name = qual + "::" + hit->fn.name;
                    hit->fn.kind = "OTHER";
                }
                pos = head_start = decl_start = static_cast<std::size_t>(hit->close) + 1;
                continue;
            }
            if (match_at_start(extern_head, head) ||
                (namespace_head.search(head) && head.find('(') == std::string::npos)) {
                stack.emplace_back(false, qual);
                decl_start = pos;
                continue;
            }
            auto tm = match_at_start(type_head, head);
            bool is_type = tm && shape.find('=') == std::string::npos &&
                           (!tm->named("enum").empty() ||
                            drop_type_operators(regex_sub(pre_attr, "", head)).find('(') == std::string::npos);
            if (is_type && tm->named("enum").empty()) {
                auto cm = match_at_start(class_name, head);
                std::string cname = cm ? cm->named("name") : std::string();
                if (!cname.empty() && !qual.empty()) cname = qual + "::" + cname;
                stack.emplace_back(true, cname);
                decl_start = pos;
                continue;
            }
            // `std::vector<int> m{...}` / `static int n{0}`: a type, then the
            // declared name, then a brace initializer (a member's default or
            // a variable's), not a function head.
            static Regex braced_init(R"(\s*[A-Za-z_][\w:<>,*&\s]*[\s*&>][A-Za-z_]\w*\s*)");
            const bool var_init = !is_type && full_match(braced_init, head);
            std::string gap_head = head;
            int gap_start = start;
            if (macro_cut && !is_type && !var_init && shape.find('(') == std::string::npos) {
                // `DEFINE_X(a)\nWITH_LOCK {`: cut to a paren-less head that is
                // nothing it reads, so the uncut head decides (Law 7).
                gap_head = full_head;
                std::size_t fl = 0;
                while (fl < gap_head.size() && std::isspace(static_cast<unsigned char>(gap_head[fl]))) ++fl;
                gap_start = brace - static_cast<int>(gap_head.size()) + static_cast<int>(fl);
            }
            const auto gap_shape = gap_head.size() == head.size() ? shape : head_shape(gap_head);
            if (!is_type && !var_init && gap_shape.find('(') != std::string::npos &&
                gap_shape.find('=') == std::string::npos) {
                auto t = trim(gap_head);
                auto first = trim(t.substr(0, t.find('\n')));
                gaps.emplace_back(line_of(gap_start), first.substr(0, 80));
            } else if (!is_type && !var_init && shape.find('(') == std::string::npos &&
                       shape.find('=') == std::string::npos && after_semicolon) {
                // `local void once(state, init) once_t *state; void (*init)(void); {`:
                // a K&R head the patterns did not read ends at a `;`. The
                // definition starts at the last `;`-segment since the previous
                // `{`/`}` that has one: a gap, never a body skipped without a
                // word (Law 7). A brace initializer after a declaration
                // (var_init) is not this.
                std::vector<std::pair<std::size_t, std::string>> segs;  // (offset, text)
                std::size_t s0 = decl_start;
                for (std::size_t q = decl_start; q <= k; ++q) {
                    if (q == k || text[q] == ';') {
                        segs.emplace_back(s0, text.substr(s0, q - s0));
                        s0 = q + 1;
                    }
                }
                for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
                    if (it->second.find('(') == std::string::npos ||
                        head_shape(it->second).find('=') != std::string::npos)
                        continue;
                    std::size_t lw = 0;
                    while (lw < it->second.size() && std::isspace(static_cast<unsigned char>(it->second[lw]))) ++lw;
                    auto t = trim(it->second);
                    auto first = trim(t.substr(0, t.find('\n')));
                    gaps.emplace_back(line_of(static_cast<int>(it->first + lw)), first.substr(0, 80));
                    break;
                }
            }
            // Initializer, enum or brace-init body, or a gap: skip it whole.
            int close = match_brace(text, brace);
            if (close < 0) break;
            pos = head_start = decl_start = static_cast<std::size_t>(close) + 1;
        }
        return gaps;
    }
};

// `CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *v) {`: an ALL_CAPS
// function-like export macro wrapping the return type (cJSON, libpng,
// zlib-style APIs). Discovery reads it as `cJSON * cJSON_Parse(...)`; the
// macro name and its parentheses become spaces, so offsets and lines hold.
std::string unwrap_export_macros(std::string text) {
    static Regex re(
        R"(^([ \t]*(?:(?:static|extern|inline)[ \t]+)*)([A-Z_][A-Z0-9_]*[ \t]*\()([^();{}\n]*)\))"
        R"((?=[ \t]*\**[ \t]*[A-Za-z_]\w*[ \t]*\())",
        true);
    if (text.find('(') == std::string::npos) return text;
    for (auto& m : re.finditer(text)) {
        auto [a, b] = m.spans[2];
        for (int k = a; k < b; ++k) text[static_cast<std::size_t>(k)] = ' ';
        // the closing `)` right after the wrapped type
        text[static_cast<std::size_t>(m.spans[0].second - 1)] = ' ';
    }
    return text;
}

// Unbalanced #elif / #else arms as spaces (newlines kept), for discovery
// only (Python engine _blank_else_branches). zlib trees.c opens one `{` in
// each arm of `#ifdef FORCE_STORED if (..) { #else if (..) { #endif`:
// counting both unbalances every brace after it and the function (and the
// rest of the file) was lost without a gap. When an arm of a conditional
// does not balance its own braces, only the first arm is kept (the ctags
// rule); balanced conditionals are left as they are. Bodies still hold
// every arm for the lints.
std::string blank_else_branches(std::string text) {
    if (text.find('#') == std::string::npos) return text;
    static Regex cond(R"(^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b)");
    std::vector<std::pair<std::size_t, std::size_t>> lines;  // [start, end) of each line
    for (std::size_t pos = 0;;) {
        auto nl = text.find('\n', pos);
        lines.emplace_back(pos, nl == std::string::npos ? text.size() : nl);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    auto delta = [&](std::size_t k) {
        long d = 0;
        for (auto i = lines[k].first; i < lines[k].second; ++i) d += text[i] == '{' ? 1 : text[i] == '}' ? -1 : 0;
        return d;
    };
    std::vector<std::vector<std::size_t>> stack;  // per open #if: the start line of each arm
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view ln(text.data() + lines[i].first, lines[i].second - lines[i].first);
        auto m = match_at_start(cond, ln);
        if (!m) continue;
        auto d = m->group(1);
        if (d.starts_with("if")) {
            stack.push_back({i});
        } else if ((d == "elif" || d == "else") && !stack.empty()) {
            stack.back().push_back(i);
        } else if (d == "endif" && !stack.empty()) {
            auto arms = std::move(stack.back());
            stack.pop_back();
            if (arms.size() < 2) continue;
            arms.push_back(i);
            bool unbalanced = false;
            for (std::size_t a = 0; a + 1 < arms.size(); ++a) {
                long sum = 0;
                for (auto k = arms[a] + 1; k < arms[a + 1]; ++k) sum += delta(k);
                if (sum != 0) unbalanced = true;
            }
            if (!unbalanced) continue;
            for (std::size_t a = 1; a + 1 < arms.size(); ++a)
                for (auto k = arms[a] + 1; k < arms[a + 1]; ++k)
                    for (auto c = lines[k].first; c < lines[k].second; ++c) text[c] = ' ';
        }
    }
    return text;
}

std::pair<std::vector<FunctionInfo>, std::vector<std::pair<int, std::string>>> parse_text(
    std::string_view text, const std::string& rel) {
    Parser p;
    p.rel = rel;
    p.code = blank_else_branches(unwrap_export_macros(blank_char_literals(strip_comments_keep_lines(text, true))));
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
    // The stripped text is length-preserving, so columns of body offsets are
    // source columns: col_shifts stays empty (source_col is the identity).
    // Every `main` token of the unit (comments and strings blanked) beyond
    // one per definition head of main is a mention bmc must see.
    {
        static Regex main_word("\\bmain\\b");
        auto plain = strip_comments_keep_lines(text, true);
        std::size_t mentions = main_word.finditer(plain).size();
        std::size_t defs = 0;
        for (auto& fn : out) defs += fn.name == "main";
        if (mentions > defs)
            for (auto& fn : out) fn.unit_names_main = true;
    }
    {
        // Strings kept: `_Pragma("weak f")` spells the pragma in one.
        static Regex weak_word("\\b(?:__)?(?:weak|weakref|alias)(?:__)?\\b");
        if (weak_word.search(strip_comments_keep_lines(text, false)))
            for (auto& fn : out) fn.unit_has_weak = true;
    }
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

namespace {

// strip_comments_keep_lines, and optionally where it dropped the `/*` and `*/`
// delimiters: (line, column in the output of the next kept character, number
// of characters dropped there).
std::string strip_comments_impl(std::string_view text, bool blank_strings,
                                std::vector<std::array<int, 3>>* shifts) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0, n = text.size();
    bool in_block = false;
    int out_line = 1;
    std::size_t line_start = 0, scanned = 0;  // out offsets: current line, newlines counted
    auto drop2 = [&] {
        if (!shifts) return;
        for (std::size_t k = scanned; k < out.size(); ++k)
            if (out[k] == '\n') {
                ++out_line;
                line_start = k + 1;
            }
        scanned = out.size();
        int col = static_cast<int>(out.size() - line_start) + 1;
        if (!shifts->empty() && (*shifts).back()[0] == out_line && (*shifts).back()[1] == col)
            (*shifts).back()[2] += 2;
        else
            shifts->push_back({out_line, col, 2});
    };
    while (i < n) {
        if (in_block) {
            if (i + 1 < n && text[i] == '*' && text[i + 1] == '/') {
                in_block = false;
                // a comment is whitespace: `int/**/x` is not `intx`. The
                // shift report describes the delimiter-dropped spelling.
                if (shifts) drop2();
                else out.append("  ");
                i += 2;
            } else {
                out.push_back(text[i] == '\n' ? '\n' : ' ');
                ++i;
            }
            continue;
        }
        if (i + 1 < n && text[i] == '/' && text[i + 1] == '*') {
            in_block = true;
            if (shifts) drop2();
            else out.append("  ");  // length-preserving: columns after the comment hold
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
        if (c == '\'' && is_digit_separator(text, i)) {
            out.push_back(c);
            ++i;
            continue;
        }
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

}  // namespace

std::string strip_comments_keep_lines(std::string_view text, bool blank_strings) {
    return strip_comments_impl(text, blank_strings, nullptr);
}

std::vector<std::array<int, 3>> comment_col_shifts(std::string_view text) {
    std::vector<std::array<int, 3>> shifts;
    strip_comments_impl(text, false, &shifts);
    return shifts;
}

int match_brace(std::string_view text, int open_idx) {
    // A negative start is not a position in the text (it used to index
    // before text.data()).
    if (open_idx < 0) return -1;
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

std::vector<FunctionInfo> extract_functions_from_text(std::string_view text, const std::string& rel) {
    return parse_text(scrub_utf8(text), rel).first;
}

std::vector<std::pair<int, std::string>> parse_gaps(const std::filesystem::path& path) {
    auto text = read_file(path);
    return parse_text(text, path.string()).second;
}

std::vector<std::pair<int, std::string>> parse_gaps_from_text(std::string_view text) {
    return parse_text(scrub_utf8(text), "").second;
}

std::string scrub_utf8(std::string_view text) {
    std::string out(text);
    const auto n = out.size();
    auto cont = [&](std::size_t k) {
        return k < n && (static_cast<unsigned char>(out[k]) & 0xC0) == 0x80;
    };
    for (std::size_t i = 0; i < n;) {
        auto c = static_cast<unsigned char>(out[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = cont(i + 1) ? 2 : 0;
        } else if (c >= 0xE0 && c <= 0xEF) {
            auto c1 = i + 1 < n ? static_cast<unsigned char>(out[i + 1]) : 0;
            bool ok = cont(i + 1) && cont(i + 2) && !(c == 0xE0 && c1 < 0xA0)  // overlong
                      && !(c == 0xED && c1 >= 0xA0);                            // surrogate
            len = ok ? 3 : 0;
        } else if (c >= 0xF0 && c <= 0xF4) {
            auto c1 = i + 1 < n ? static_cast<unsigned char>(out[i + 1]) : 0;
            bool ok = cont(i + 1) && cont(i + 2) && cont(i + 3) && !(c == 0xF0 && c1 < 0x90) &&
                      !(c == 0xF4 && c1 >= 0x90);  // overlong, above U+10FFFF
            len = ok ? 4 : 0;
        }
        if (len == 0) {
            out[i++] = SCRUBBED_BYTE;
            continue;
        }
        i += len;
    }
    return out;
}

std::optional<std::string> scrubbed_byte_reason(std::string_view body, std::string_view engine) {
    if (body.find(SCRUBBED_BYTE) == std::string_view::npos) return std::nullopt;
    return "UNENCODED: byte that is not UTF-8 text in body (read as 0x7F; not modelled by " +
           std::string(engine) + "): not a proof";
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
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && scope::skip_dir(it->path().filename().string())) {
            it.disable_recursion_pending();
            continue;
        }
        if (scope::skipped_path(it->path(), root)) continue;
        if (it->is_regular_file(e2) && is_c_ext(it->path().extension().string()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace prism
