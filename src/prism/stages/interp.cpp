// The concrete C interpreter (the Python engine prism/concrete.py):
// execute, eval_src, eval_cond and the public concrete_execute.
#include "interp.hpp"

#include <charconv>

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
const std::unordered_set<std::string> kCastWords = {
    "char", "short", "int", "long", "unsigned", "signed", "const", "volatile", "void",
    "_Bool", "bool", "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
    "int8_t", "uint8_t", "int16_t", "uint16_t", "ssize_t", "ptrdiff_t", "intptr_t",
    "uintptr_t", "intmax_t", "uintmax_t",
};

// Type words that start a declaration the interpreter models.
const std::unordered_set<std::string> kDeclKws = {
    "int", "unsigned", "signed", "long", "short", "char", "uint32_t", "int32_t", "uint64_t", "int64_t",
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
};

const std::unordered_set<std::string> kStmtStartWords = {
    "if",     "for",        "while",    "switch",   "return",   "sizeof",   "typeof",
    "else",   "do",         "case",     "default",  "goto",     "break",    "continue",
    "assert", "throw",      "try",      "catch",    "asm",      "__asm__",  "__asm",
    "typedef","static",     "extern",   "auto",     "register", "int",      "unsigned",
    "signed", "long",       "short",    "char",     "uint32_t", "int32_t",  "uint64_t",
    "int64_t","size_t",     "void",     "float",    "double",   "_Bool",    "bool",
    "const",  "volatile",   "_Atomic",  "struct",   "union",    "enum",     "ssize_t",
    "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
};

struct UB : std::runtime_error {
    std::string cls;
    explicit UB(std::string c) : std::runtime_error(c), cls(std::move(c)) {}
};

struct BreakEx : std::exception {};

struct ContinueEx : std::exception {};

bool is_ident(std::string_view t) {
    if (t.empty() || !(std::isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_'))
        return false;
    for (std::size_t i = 1; i < t.size(); ++i)
        if (!(std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_')) return false;
    return true;
}

bool is_computed_goto(std::string_view text) {
    if (!starts_kw(text, "goto")) return false;
    return lstrip(std::string(text.substr(4))).starts_with("*");
}

bool is_nested_function(std::string_view text) {
    static Regex re(
        "(?:void|int|unsigned(?:\\s+int)?|long(?:\\s+int)?|short|char|"
        "float|double|_Bool|bool)\\s+[A-Za-z_]\\w*\\s*\\([^)]*\\)\\s*\\{");
    auto s = lstrip(std::string(text));
    auto m = re.search_match(s, 0);
    return m && !m->spans.empty() && m->spans[0].first == 0;
}

bool looks_like_decl(std::string_view stmt) {
    auto s = lstrip(std::string(stmt));
    for (auto& kw : kDeclKws)
        if (starts_kw(s, kw)) return true;
    return false;
}

}  // namespace

namespace stages_detail {
bool type_is_unsigned(std::string_view typ) {
    static Regex re("(?i)\\bunsigned\\b|\\bsize_t\\b|\\buint\\d*_t\\b|\\bu_int\\b|\\bu_long\\b");
    return re.search(typ);
}

int type_width(std::string_view typ) {
    // LP64, as the bmc encoder: long, long long, size_t and the 64-bit
    // typedefs are 64 bits wide.
    std::string t = lower_copy(std::string(typ));
    if (re_search("\\blong\\b|\\b[iu]nt64_t\\b|\\bs?size_t\\b|\\bu?intptr_t\\b|\\bu?intmax_t\\b|\\bptrdiff_t\\b", t))
        return 64;
    return WIDTH;
}

std::optional<CT> scalar_ctype(std::string_view typ) {
    std::string t(typ);
    if (t.find_first_of("*[&(") != std::string::npos) return std::nullopt;
    static const std::unordered_set<std::string> kQual = {
        "const", "volatile", "register", "auto", "static", "extern", "inline", "restrict", "__restrict",
        "__restrict__",
    };
    std::vector<std::string> words;
    {
        std::istringstream ss(t);
        std::string wd;
        while (ss >> wd)
            if (!kQual.contains(wd)) words.push_back(wd);
    }
    if (words.empty()) return std::nullopt;
    static const std::map<std::string, CT> kNamed = {
        {"_Bool", {1, true}},     {"bool", {1, true}},       {"int8_t", {8, false}},
        {"uint8_t", {8, true}},   {"int16_t", {16, false}},  {"uint16_t", {16, true}},
        {"int32_t", {32, false}}, {"uint32_t", {32, true}},  {"int64_t", {64, false}},
        {"uint64_t", {64, true}}, {"size_t", {64, true}},    {"ssize_t", {64, false}},
        {"ptrdiff_t", {64, false}}, {"intptr_t", {64, false}}, {"uintptr_t", {64, true}},
        {"intmax_t", {64, false}}, {"uintmax_t", {64, true}},
    };
    if (words.size() == 1) {
        auto it = kNamed.find(words[0]);
        if (it != kNamed.end()) return it->second;
    }
    bool is_u = false, is_s = false;
    int n_long = 0, n_short = 0, n_char = 0, n_int = 0;
    for (auto& wd : words) {
        if (wd == "unsigned") is_u = true;
        else if (wd == "signed") is_s = true;
        else if (wd == "long") ++n_long;
        else if (wd == "short") ++n_short;
        else if (wd == "char") ++n_char;
        else if (wd == "int") ++n_int;
        else return std::nullopt;
    }
    if ((is_u && is_s) || n_long > 2 || n_short > 1 || n_char > 1 || n_int > 1) return std::nullopt;
    if (n_char && (n_long || n_short || n_int)) return std::nullopt;
    if (n_short && n_long) return std::nullopt;
    if (n_char) return CT{8, is_u};
    if (n_short) return CT{16, is_u};
    if (n_long) return CT{64, is_u};
    return CT{32, is_u};
}

int64_t norm(int64_t v, CT t) {
    if (t.w == 1) return v != 0;
    if (t.w >= 64) return v;
    auto bits = static_cast<uint64_t>(v) & ((uint64_t{1} << t.w) - 1);
    if (!t.u && (bits >> (t.w - 1)) & 1) return static_cast<int64_t>(bits) - (int64_t{1} << t.w);
    return static_cast<int64_t>(bits);
}

}  // namespace stages_detail

namespace {
std::vector<std::string> split_semi(const std::string& s) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        if (ch == ';' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::pair<std::string, std::string> take_block(std::string text) {
    text = lstrip(std::move(text));
    if (text.starts_with("{")) return brace(text);
    return stmt(text);
}

std::pair<std::string, std::string> upto_colon(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') --depth;
        else if (text[i] == ':' && depth == 0) return {text.substr(0, i), text.substr(i + 1)};
    }
    throw ParseFail("expected :");
}

std::vector<std::string> tok_uncached(const std::string& src);

// Loop conditions and bodies are re-read on every iteration: the tokens of a
// source text are cached per thread (a pure function of the text).
std::vector<std::string> tok(const std::string& src) {
    thread_local std::unordered_map<std::string, std::vector<std::string>> cache;
    if (auto it = cache.find(src); it != cache.end()) return it->second;
    auto toks = tok_uncached(src);
    if (cache.size() >= 8192) cache.clear();
    cache.emplace(src, toks);
    return toks;
}

std::vector<std::string> tok_uncached(const std::string& src) {
    // A scrubbed byte (cparse.hpp) is not the source's value; the regex
    // below would read it inside a literal.
    if (src.find(SCRUBBED_BYTE) != std::string::npos)
        throw ParseFail("UNENCODED: byte that is not UTF-8 text (read as 0x7F)");
    static Regex rx(
        R"(0[xX][0-9a-fA-F](?:'?[0-9a-fA-F])*[uUlL]*|0[bB][01](?:'?[01])*[uUlL]*|\d(?:'?\d)*[uUlL]*|'(?:\\.|[^\\'])'|"(?:\\.|[^\\"])*"|[A-Za-z_]\w*|&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--|[+\-*/%<>=!&|^~()[\],?:{}])");
    std::vector<std::string> out;
    std::size_t at = 0;
    // Text between tokens must be blank: an unknown character (a float's
    // '.', '->', '@') is a parse failure, not something to skip.
    auto gap = [&](std::size_t to) {
        for (; at < to; ++at)
            if (!std::isspace(static_cast<unsigned char>(src[at])))
                throw ParseFail(std::string("unexpected character '") + src[at] + "'");
    };
    for (auto& m : rx.finditer(src)) {
        if (m.spans.empty() || m.spans[0].first < 0) continue;
        gap(static_cast<std::size_t>(m.spans[0].first));
        at = static_cast<std::size_t>(m.spans[0].second);
        out.push_back(m.text);
        // Digit separators (`1'000`) are not part of the value.
        if (std::isdigit(static_cast<unsigned char>(m.text[0])) && m.text.find('\'') != std::string::npos)
            std::erase(out.back(), '\'');
    }
    gap(src.size());
    return out;
}

int char_lit_value(const std::string& t) {
    if (t.size() < 2) throw ParseFail("empty character literal");
    std::string inner = t.substr(1, t.size() - 2);
    if (inner.empty()) throw ParseFail("empty character literal");
    if (inner[0] == '\\' && inner.size() >= 2) {
        switch (inner[1]) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case '0': return 0;
        case '\\': return 92;
        case '\'': return 39;
        case '"': return 34;
        default: return static_cast<unsigned char>(inner[1]);
        }
    }
    return static_cast<unsigned char>(inner[0]);
}

std::string string_lit_value(const std::string& t) {
    if (t.size() < 2) return {};
    std::string inner = t.substr(1, t.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\\' && i + 1 < inner.size()) {
            char esc = inner[++i];
            switch (esc) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            default: out.push_back(esc); break;
            }
        } else {
            out.push_back(inner[i]);
        }
    }
    return out;
}

std::optional<std::string> unencoded_layout_prefix(std::string_view text);

std::optional<std::string> unencoded_layout_stmt(std::string_view stmt_s);


std::optional<std::string> unencoded_layout_prefix(std::string_view text) {
    auto s = lstrip(std::string(text));
    static Regex su(R"((?:struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{)");
    static Regex en(R"(enum\s*\{)");
    static Regex se(R"((?:static|extern)\b)");
    static Regex al(R"((?:_Alignas|alignas)\s*\()");
    static Regex at(R"(__auto_type\b)");
    static Regex tl(R"((?:_Thread_local|thread_local)\b)");
    static Regex cx(R"((?:_Complex|_Imaginary)\b)");
    static Regex cl(
        R"((?:(?:unsigned|signed|long|short|int|char|uint32_t|int32_t|uint64_t|int64_t|size_t)\s+)+\w+\s*=\s*\([^)]*\)\s*\{)");
    if (match_at(su, s)) return "struct unencoded";
    if (match_at(en, s)) return "anon enum unencoded";
    if (match_at(se, s)) return "storage-duration unencoded";
    if (match_at(al, s)) return "alignas unencoded";
    if (match_at(at, s)) return "storage-class unencoded";
    if (match_at(tl, s)) return "thread-local unencoded";
    if (match_at(cx, s)) return "complex unencoded";
    if (match_at(cl, s)) return "compound-lit unencoded";
    return std::nullopt;
}

std::optional<std::string> unencoded_layout_stmt(std::string_view stmt_s) {
    auto s = strip(std::string(stmt_s));
    if (s.empty()) return std::nullopt;
    if (re_search(R"(\b(?:__int128(?:_t)?|_BitInt)\b)", s)) return "128-bit unencoded";
    if (starts_kw(s, "constexpr")) return "constexpr unencoded";
    if (starts_kw(s, "const")) return "const unencoded";
    if (starts_kw(s, "register") || starts_kw(s, "auto")) return "storage-class unencoded";
    if (starts_kw(s, "static") || starts_kw(s, "extern")) return "storage-duration unencoded";
    if (starts_kw(s, "struct") || starts_kw(s, "union")) return "struct unencoded";
    if (is_nested_function(s)) return "nested function unencoded";
    // `T name`, and `T *name` / `T * const name` (a pointer to a typedef type).
    static Regex td(R"(([A-Za-z_]\w*)(?:\s+|\s*\*+\s*(?:const\s+)?)[A-Za-z_]\w*\s*(?:[=;\[]|$))");
    auto m = match_at(td, s);
    if (!m) return std::nullopt;
    if (kStmtStartWords.contains(m->group(1))) return std::nullopt;
    return "typedef local unencoded";
}

// Enumerator values of every plain `enum { ... }` in the file. An
// enumerator whose value cannot be computed here (e.g. `A = 1 << 3`) is left
// out together with the implicit enumerators that follow it, and a name
// defined with two different values is left out: an unknown enumerator is
// UNENCODED at its use, never a wrong constant.
std::map<std::string, int> extract_enums(std::string text) {
    // Same semantics as bmc.cpp / prism/bmc.py extract_enums (the Python
    // concrete oracle imports that one).
    text = strip_comments_keep_lines(text);
    std::map<std::string, int> out;
    std::set<std::string> ambiguous;
    auto drop = [&](const std::string& name) {
        ambiguous.insert(name);
        out.erase(name);
    };
    static Regex en("\\benum\\b(?:\\s+[A-Za-z_]\\w*)?\\s*\\{([^{}]*)\\}");
    for (auto& m : en.finditer(text)) {
        std::optional<int64_t> nxt = 0;
        auto inner = m.group(1);
        std::string part;
        std::stringstream ss(inner);
        while (std::getline(ss, part, ',')) {
            {
                std::istringstream ws(part);
                std::string w, sq;
                while (ws >> w) sq += (sq.empty() ? "" : " ") + w;
                part = sq;
            }
            if (part.empty()) continue;
            std::string name = part;
            auto eq = part.find('=');
            if (eq != std::string::npos) {
                name = strip(part.substr(0, eq));
                auto val = strip(part.substr(eq + 1));
                while (!val.empty() && (val.back() == 'u' || val.back() == 'U' ||
                                        val.back() == 'l' || val.back() == 'L'))
                    val.pop_back();
                nxt = std::nullopt;
                try {
                    size_t used = 0;
                    auto v = std::stoll(val, &used, 0);
                    if (used == val.size() && !val.empty()) nxt = v;
                } catch (...) {}
                if (!nxt && is_ident(val) && !ambiguous.count(val)) {
                    auto it = out.find(val);
                    if (it != out.end()) nxt = it->second;
                }
            }
            if (!is_ident(name)) {
                nxt = std::nullopt;
                continue;
            }
            if (!nxt || *nxt < INT32_MIN || *nxt > INT32_MAX) {
                drop(name);
                nxt = std::nullopt;
                continue;
            }
            if (ambiguous.count(name)) {
                nxt = *nxt + 1;
                continue;
            }
            auto it = out.find(name);
            if (it != out.end() && it->second != *nxt) drop(name);
            else out[name] = static_cast<int>(*nxt);
            nxt = *nxt + 1;
        }
    }
    return out;
}

std::map<std::string, int> enums_for(const FunctionInfo& fn) {
    auto p = locate_source(fn);
    if (!p) return {};
    try {
        return extract_enums(read_text_file(*p));
    } catch (...) {
        return {};
    }
}

// The promoted type of an operand (C11 6.3.1.1): narrower than int is int.
CT promote(CT t) { return t.w < 32 ? kInt : t; }

// The usual arithmetic conversions (C11 6.3.1.8) of two promoted types.
CT common_type(CT a, CT b) {
    a = promote(a);
    b = promote(b);
    if (a.w != b.w) return a.w > b.w ? a : b;
    return CT{a.w, a.u || b.u};
}

int64_t type_min(CT t) { return t.u ? 0 : (t.w >= 64 ? INT64_MIN_V : -(int64_t{1} << (t.w - 1))); }

int64_t type_max(CT t) { return t.w >= 64 ? INT64_MAX_V : (t.u ? (int64_t{1} << t.w) - 1 : (int64_t{1} << (t.w - 1)) - 1); }

// An integer constant and its type (C11 6.4.4.1, LP64): digit separators
// ('), 0x / 0b / leading-0 octal, and the u / l / ll suffixes.
std::pair<int64_t, CT> int_literal(const std::string& tok0) {
    std::string t;
    for (char c : tok0)
        if (c != '\'') t.push_back(c);
    bool has_u = false;
    int n_l = 0;
    while (!t.empty()) {
        char c = t.back();
        if (c == 'u' || c == 'U') has_u = true;
        else if (c == 'l' || c == 'L') ++n_l;
        else break;
        t.pop_back();
    }
    int base = 10;
    std::string digits = t;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        base = 16;
        digits = t.substr(2);
    } else if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B')) {
        base = 2;
        digits = t.substr(2);
    } else if (t.size() > 1 && t[0] == '0') {
        base = 8;
        digits = t.substr(1);
    }
    if (digits.empty()) throw ParseFail("bad num " + tok0);
    uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
    if (ec == std::errc::result_out_of_range) throw ParseFail("literal overflow " + tok0);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) throw ParseFail("bad num " + tok0);
    const bool dec = base == 10;
    // The first type of the list that can represent the value.
    std::vector<CT> order;
    if (!has_u && n_l == 0) {
        order = dec ? std::vector<CT>{{32, false}, {64, false}}
                    : std::vector<CT>{{32, false}, {32, true}, {64, false}, {64, true}};
    } else if (has_u && n_l == 0) {
        order = {{32, true}, {64, true}};
    } else if (!has_u) {
        order = dec ? std::vector<CT>{{64, false}} : std::vector<CT>{{64, false}, {64, true}};
    } else {
        order = {{64, true}};
    }
    for (auto ct : order) {
        if (ct.u && ct.w >= 64) return {static_cast<int64_t>(v), ct};
        if (v <= static_cast<uint64_t>(type_max(ct))) return {static_cast<int64_t>(v), ct};
    }
    // A decimal constant above LLONG_MAX has no type (C11 6.4.4.1p6).
    throw ParseFail("64-bit literal unencoded " + tok0);
}

enum class Nk { Num, Id, Idx, Un, Pre, Post, Str, Call, Tern, Comma, Bin, Cast };

struct Node {
    Nk k{};
    std::string a;
    int64_t n = 0;
    std::string s;
    std::vector<Node> ch;
    CT t{};  // Num and Cast: the type
};

CT tree_type(const Node& t, const St& st);

int sizeof_concrete(const std::vector<std::string>& inner, const St& st) {
    if (inner.empty()) return 4;
    for (auto& t : inner)
        if (t == "*") return 8;  // LP64 pointer
    if (inner.size() == 1 && is_ident(inner[0])) {
        auto a = st.arrays.find(inner[0]);
        if (a != st.arrays.end()) {
            auto et = st.arr_t.find(inner[0]);
            int esz = et == st.arr_t.end() ? 4 : std::max(1, et->second.w / 8);
            return static_cast<int>(a->second.size()) * esz;
        }
        auto v = st.types.find(inner[0]);
        if (v != st.types.end()) return std::max(1, v->second.w / 8);
    }
    if (auto ct = scalar_ctype(join_sv(inner, " "))) return std::max(1, ct->w / 8);
    return 4;
}

struct EParser {
    St& st;
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    const std::unordered_map<std::string, int> prec{
        {"||", 10}, {"&&", 20}, {"|", 30},  {"^", 40},  {"&", 50},  {"==", 60}, {"!=", 60},
        {"<", 70},  {">", 70},  {"<=", 70}, {">=", 70}, {"<<", 80}, {">>", 80}, {"+", 90},
        {"-", 90},  {"*", 100}, {"/", 100}, {"%", 100},
    };
    std::string peek() const { return pos < tokens.size() ? tokens[pos] : std::string{}; }
    std::string eat(const std::string* expect = nullptr) {
        if (pos >= tokens.size()) throw ParseFail("unexpected end");
        auto got = tokens[pos];
        if (expect && got != *expect) throw ParseFail("expected " + *expect + " got " + got);
        ++pos;
        return got;
    }
    std::string eat_s(std::string t) { return eat(&t); }
    Node num(int64_t v, CT t) {
        Node n{Nk::Num, {}, v};
        n.t = t;
        return n;
    }
    Node nud() {
        auto t = eat();
        if (t == "sizeof") {
            // sizeof yields a size_t (unsigned long in LP64).
            if (peek() == "(") {
                eat_s("(");
                std::vector<std::string> inner;
                int depth = 1;
                while (depth) {
                    auto ntok = eat();
                    if (ntok == "(") {
                        ++depth;
                        inner.push_back(ntok);
                    } else if (ntok == ")") {
                        --depth;
                        if (depth) inner.push_back(ntok);
                    } else {
                        inner.push_back(ntok);
                    }
                }
                return num(sizeof_concrete(inner, st), CT{64, true});
            }
            auto name = eat();
            return num(sizeof_concrete({name}, st), CT{64, true});
        }
        if (t == "(") {
            if (peek() == "{") throw ParseFail("statement-expr unencoded");
            if (kCastWords.contains(peek())) {
                std::vector<std::string> words;
                bool ptr = false;
                while (!peek().empty() && peek() != ")") {
                    if (!kCastWords.contains(peek()) && peek() != "*") break;
                    if (peek() == "*") ptr = true;
                    words.push_back(eat());
                }
                eat_s(")");
                auto operand = parse(110);
                auto joined = join_sv(words, " ");
                auto ct = ptr ? std::nullopt : scalar_ctype(joined);
                if (!ct) {
                    // (void)x evaluates x for its effects; a pointer cast
                    // keeps the operand's value.
                    if (!ptr && joined != "void" && !joined.ends_with(" void") && !joined.starts_with("void "))
                        throw ParseFail("cast unencoded (" + joined + ")");
                    return operand;
                }
                Node n{Nk::Cast};
                n.t = *ct;
                n.ch.push_back(std::move(operand));
                return n;
            }
            auto v = parse(0);
            eat_s(")");
            return v;
        }
        if (t == "-" || t == "!" || t == "~") {
            Node n{Nk::Un, t};
            n.ch.push_back(parse(110));
            return n;
        }
        if (t == "+") return parse(110);
        if (t == "++" || t == "--") {
            auto name = eat();
            if (!is_ident(name)) throw ParseFail("bad token " + name);
            return Node{Nk::Pre, t, 0, name};
        }
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'') return num(char_lit_value(t), kInt);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
            return Node{Nk::Str, {}, 0, string_lit_value(t)};
        if (std::isdigit(static_cast<unsigned char>(t[0]))) {
            auto [v, ct] = int_literal(t);
            return num(v, ct);
        }
        if (is_ident(t)) {
            if (t == "_Generic" || t == "offsetof") throw ParseFail(t + " unencoded");
            if (peek() == "[") {
                eat_s("[");
                auto idx = parse(0);
                eat_s("]");
                Node n{Nk::Idx, t};
                n.ch.push_back(std::move(idx));
                return n;
            }
            if (peek() == "(") {
                eat_s("(");
                Node n{Nk::Call, t};
                if (peek() != ")") {
                    n.ch.push_back(parse(2));
                    while (peek() == ",") {
                        eat_s(",");
                        n.ch.push_back(parse(2));
                    }
                }
                eat_s(")");
                return n;
            }
            if (peek() == "++" || peek() == "--") {
                auto op = eat();
                return Node{Nk::Post, op, 0, t};
            }
            return Node{Nk::Id, t};
        }
        throw ParseFail("bad token " + t);
    }
    Node parse(int minp) {
        auto left = nud();
        while (prec.contains(peek()) && prec.at(peek()) >= minp) {
            auto op = eat();
            auto right = parse(prec.at(op) + 1);
            Node n{Nk::Bin, op};
            n.ch.push_back(std::move(left));
            n.ch.push_back(std::move(right));
            left = std::move(n);
        }
        if (minp <= 5 && peek() == "?") {
            eat_s("?");
            auto th = parse(0);
            eat_s(":");
            auto el = parse(5);
            Node n{Nk::Tern};
            n.ch.push_back(std::move(left));
            n.ch.push_back(std::move(th));
            n.ch.push_back(std::move(el));
            left = std::move(n);
        }
        if (minp <= 1 && peek() == ",") {
            eat_s(",");
            auto right = parse(0);
            Node n{Nk::Comma};
            n.ch.push_back(std::move(left));
            n.ch.push_back(std::move(right));
            left = std::move(n);
        }
        return left;
    }
};

bool is_compare(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" || op == "&&" ||
           op == "||";
}

CT var_type(const St& st, const std::string& name) {
    auto it = st.types.find(name);
    return it == st.types.end() ? kInt : it->second;
}

CT tree_type(const Node& t, const St& st) {
    switch (t.k) {
    case Nk::Num:
    case Nk::Cast: return t.t;
    case Nk::Id: return var_type(st, t.a);
    case Nk::Idx: {
        auto it = st.arr_t.find(t.a);
        return it == st.arr_t.end() ? kInt : it->second;
    }
    case Nk::Un: return t.a == "!" ? kInt : promote(tree_type(t.ch[0], st));
    case Nk::Pre:
    case Nk::Post: return var_type(st, t.s);
    case Nk::Str:
    case Nk::Call: return kInt;
    case Nk::Tern: return common_type(tree_type(t.ch[1], st), tree_type(t.ch[2], st));
    case Nk::Comma: return tree_type(t.ch[1], st);
    case Nk::Bin:
        if (is_compare(t.a)) return kInt;
        if (t.a == "<<" || t.a == ">>") return promote(tree_type(t.ch[0], st));
        return common_type(tree_type(t.ch[0], st), tree_type(t.ch[1], st));
    }
    return kInt;
}

// a op b for a shift: T is the promoted type of the left operand, b the
// count's value (an unsigned 64-bit count held as its bit pattern reads as
// negative, i.e. out of range).
int64_t shift(int64_t a, const std::string& op, int64_t b, CT t) {
    if (b < 0 || b >= t.w) throw UB("INT-SHIFT-UB");
    if (op == "<<") {
        if (!t.u) {
            // Negative operand or unrepresentable result (C11 6.5.7p4).
            if (a < 0 || a > (type_max(t) >> b)) throw UB("INT-SHIFT-UB");
            return a << b;
        }
        return norm(static_cast<int64_t>(static_cast<uint64_t>(a) << b), t);
    }
    if (t.u && t.w >= 64) return static_cast<int64_t>(static_cast<uint64_t>(a) >> b);
    return a >> b;
}

// a op b in type T (both already converted to T, T promoted).
int64_t binop(int64_t a, const std::string& op, int64_t b, CT t) {
    if (op == "<<" || op == ">>") return shift(a, op, b, t);
    const bool u64 = t.u && t.w >= 64;
    auto ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<") return u64 ? ua < ub : a < b;
    if (op == ">") return u64 ? ua > ub : a > b;
    if (op == "<=") return u64 ? ua <= ub : a <= b;
    if (op == ">=") return u64 ? ua >= ub : a >= b;
    if (op == "&") return norm(a & b, t);
    if (op == "|") return norm(a | b, t);
    if (op == "^") return norm(a ^ b, t);
    if (op == "/" || op == "%") {
        if (b == 0) throw UB("INT-DIV-ZERO");
        if (u64) return static_cast<int64_t>(op == "/" ? ua / ub : ua % ub);
        // INT_MIN / -1 and INT_MIN % -1 (C11 6.5.5p6).
        if (!t.u && a == type_min(t) && b == -1) throw UB("INT-SIGNED-OVF");
        return op == "/" ? a / b : a % b;
    }
    if (t.u) {
        uint64_t r = op == "+" ? ua + ub : op == "-" ? ua - ub : op == "*" ? ua * ub : 0;
        if (op != "+" && op != "-" && op != "*") throw ParseFail("op " + op);
        return norm(static_cast<int64_t>(r), t);
    }
    int64_t r = 0;
    bool ovf = false;
    if (op == "+") ovf = __builtin_add_overflow(a, b, &r);
    else if (op == "-") ovf = __builtin_sub_overflow(a, b, &r);
    else if (op == "*") ovf = __builtin_mul_overflow(a, b, &r);
    else throw ParseFail("op " + op);
    if (ovf || r < type_min(t) || r > type_max(t)) throw UB("INT-SIGNED-OVF");
    return r;
}

int64_t eval_tree(const Node& t, St& st);

void eval_cstr_copy(St& st, const Node& dest, const Node& src, std::optional<int> n, bool cat) {
    if (dest.k != Nk::Id) {
        eval_tree(dest, st);
        eval_tree(src, st);
        return;
    }
    auto it = st.arrays.find(dest.a);
    if (it == st.arrays.end()) {
        eval_tree(src, st);
        return;
    }
    if (src.k != Nk::Str) {
        eval_tree(src, st);
        return;
    }
    auto& arr = it->second;
    std::size_t start = 0;
    if (cat) {
        while (start < arr.size() && arr[start]) ++start;
        if (start >= arr.size()) throw UB("MEM-OOB-WRITE");
    }
    std::vector<int64_t> payload;
    if (!n) {
        for (unsigned char c : src.s) payload.push_back(c);
        payload.push_back(0);
    } else {
        int nn = std::max(0, *n);
        std::string chars = src.s.substr(0, static_cast<std::size_t>(nn));
        for (unsigned char c : chars) payload.push_back(c);
        while (static_cast<int>(payload.size()) < nn) payload.push_back(0);
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        auto idx = start + i;
        if (idx >= arr.size()) throw UB("MEM-OOB-WRITE");
        arr[idx] = payload[i];
    }
}

// The element of `arr` at index value i (of type it): a negative index, or
// an unsigned 64-bit index held as a negative bit pattern, is out of bounds.
std::size_t checked_index(int64_t i, std::size_t size, const char* cls) {
    if (i < 0 || static_cast<uint64_t>(i) >= size) throw UB(cls);
    return static_cast<std::size_t>(i);
}

int64_t eval_tree(const Node& t, St& st) {
    switch (t.k) {
    case Nk::Num: return t.n;
    case Nk::Cast: return norm(eval_tree(t.ch[0], st), t.t);
    case Nk::Id: {
        auto it = st.vars.find(t.a);
        if (it != st.vars.end()) return norm(it->second, var_type(st, t.a));
        if (st.enums.contains(t.a)) return st.enums[t.a];
        st.vars[t.a] = 0;
        return 0;
    }
    case Nk::Idx: {
        auto it = st.arrays.find(t.a);
        if (it == st.arrays.end()) throw ParseFail("unknown array " + t.a);
        auto idx = eval_tree(t.ch[0], st);
        auto& arr = st.arrays.at(t.a);
        return norm(arr[checked_index(idx, arr.size(), "MEM-OOB-READ")], tree_type(t, st));
    }
    case Nk::Un: {
        if (t.a == "!") return eval_tree(t.ch[0], st) == 0 ? 1 : 0;
        CT ty = promote(tree_type(t.ch[0], st));
        auto v = norm(eval_tree(t.ch[0], st), ty);
        if (t.a == "-") {
            if (!ty.u && v == type_min(ty)) throw UB("INT-SIGNED-OVF");
            return norm(static_cast<int64_t>(0 - static_cast<uint64_t>(v)), ty);
        }
        if (t.a == "~") return norm(~v, ty);
        throw ParseFail("unop " + t.a);
    }
    case Nk::Post:
    case Nk::Pre: {
        CT vt = var_type(st, t.s);
        CT pt = promote(vt);
        auto cur = norm(st.vars.contains(t.s) ? st.vars[t.s] : 0, vt);
        auto nw = norm(binop(norm(cur, pt), t.a == "++" ? "+" : "-", 1, pt), vt);
        st.vars[t.s] = nw;
        return t.k == Nk::Post ? cur : nw;
    }
    case Nk::Str: return 1;
    case Nk::Call: {
        if ((t.a == "strcpy" || t.a == "strcat") && t.ch.size() >= 2) {
            eval_cstr_copy(st, t.ch[0], t.ch[1], std::nullopt, t.a == "strcat");
            return 0;
        }
        if (t.a == "strncpy" && t.ch.size() >= 3) {
            auto n = eval_tree(t.ch[2], st);
            eval_cstr_copy(st, t.ch[0], t.ch[1], static_cast<int>(std::clamp<int64_t>(n, 0, INT_MAX_32)), false);
            return 0;
        }
        for (auto& a : t.ch) eval_tree(a, st);
        return 0;
    }
    case Nk::Tern: {
        CT ty = tree_type(t, st);
        return norm(truth(eval_tree(t.ch[0], st)) ? eval_tree(t.ch[1], st) : eval_tree(t.ch[2], st), ty);
    }
    case Nk::Comma:
        eval_tree(t.ch[0], st);
        return eval_tree(t.ch[1], st);
    case Nk::Bin: {
        if (t.a == "&&") {
            if (!truth(eval_tree(t.ch[0], st))) return 0;
            return truth(eval_tree(t.ch[1], st)) ? 1 : 0;
        }
        if (t.a == "||") {
            if (truth(eval_tree(t.ch[0], st))) return 1;
            return truth(eval_tree(t.ch[1], st)) ? 1 : 0;
        }
        CT lt = tree_type(t.ch[0], st), rt = tree_type(t.ch[1], st);
        auto a = eval_tree(t.ch[0], st);
        auto b = eval_tree(t.ch[1], st);
        if (t.a == "<<" || t.a == ">>") {
            CT pl = promote(lt);
            return shift(norm(a, pl), t.a, norm(b, promote(rt)), pl);
        }
        CT ct = common_type(lt, rt);
        return binop(norm(a, ct), t.a, norm(b, ct), ct);
    }
    }
    throw ParseFail("bad tree");
}

// The value of `src` and its type.
std::pair<int64_t, CT> eval_typed(St& st, const std::string& src) {
    EParser p{st, tok(strip(src)), 0};
    auto tree = p.parse(0);
    if (p.pos != p.tokens.size()) throw ParseFail("trailing tokens");
    auto ty = tree_type(tree, st);
    return {norm(eval_tree(tree, st), ty), ty};
}

}  // namespace

namespace stages_detail {
bool float_unencoded(const FunctionInfo& fn) {
    static Regex fl("(?i)\\bfloat\\b|\\bdouble\\b");
    static Regex body_fl("\\d+\\.\\d+[fFlL]?|\\b(?:float|double)\\b");
    if (fl.search(fn.return_type)) return true;
    for (auto& [t, n] : fn.params)
        if (fl.search(t)) return true;
    return body_fl.search(fn.body);
}

int64_t eval_src(St& st, const std::string& src) { return eval_typed(st, src).first; }

}  // namespace stages_detail

namespace {
std::pair<std::string, std::string> consume_stmt_src(std::string raw);

struct CParser {
    std::string body;
    St& st;
    CParser(std::string b, St& s) : body(std::move(b)), st(s) {}
    void run() { stmts(prep(body)); }
    static std::string prep(std::string b) {
        // Directive lines only: `"test issue #22"` in a string is not one.
        static Regex re("^[ \\t]*#.*", true);
        std::string out;
        std::size_t i = 0;
        for (auto& m : re.finditer(b)) {
            if (m.spans.empty()) continue;
            auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
            auto e = static_cast<std::size_t>(std::max(0, m.spans[0].second));
            if (a < i) continue;
            out.append(b, i, a - i);
            out += ' ';
            i = e;
        }
        out.append(b, i, std::string::npos);
        return out;
    }
    void stmts(std::string text);
    void decl(std::string s);
    void assign_or_expr(std::string s);
    void astore(const std::string& name, const std::string& idx, const std::string& rhs);
    std::string do_if(const std::string& text);
    std::string do_while(const std::string& text);
    std::string do_do(const std::string& text);
    std::string do_for(const std::string& text);
    std::string do_switch(const std::string& text);
    std::string do_assert(const std::string& text);
    bool run_loop_body(const std::string& b) {
        try {
            stmts(b);
        } catch (const BreakEx&) {
            return false;
        } catch (const ContinueEx&) {
        }
        return true;
    }
};

// Top-level pieces of `s` split at `sep`, outside (), [] and {} and quotes.
std::vector<std::string> split_top(const std::string& s, char sep) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (quote) {
            cur.push_back(ch);
            if (ch == '\\' && i + 1 < s.size()) cur.push_back(s[++i]);
            else if (ch == quote) quote = 0;
            continue;
        }
        // A ' after a digit or letter is a digit separator (1'000), not a quote.
        if (ch == '"' || (ch == '\'' && !(i && std::isalnum(static_cast<unsigned char>(s[i - 1]))))) quote = ch;
        else if (ch == '(' || ch == '[' || ch == '{') ++depth;
        else if (ch == ')' || ch == ']' || ch == '}') --depth;
        if (ch == sep && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

// One statement that may carry a brace initialiser (`int p[] = {1, 2};`):
// up to the first `;` outside (), {} and quotes.
std::pair<std::string, std::string> decl_stmt(const std::string& text) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (quote) {
            if (ch == '\\') ++i;
            else if (ch == quote) quote = 0;
            continue;
        }
        // A ' after a digit or letter is a digit separator (1'000), not a quote.
        if (ch == '"' || (ch == '\'' && !(i && std::isalnum(static_cast<unsigned char>(text[i - 1]))))) quote = ch;
        else if (ch == '(' || ch == '{') ++depth;
        else if (ch == ')' || ch == '}') --depth;
        else if (ch == ';' && depth == 0) return {text.substr(0, i + 1), text.substr(i + 1)};
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<std::size_t>(80, text.size())));
}

// The length of a constant array dimension: an integer literal or an enum
// constant. Anything else is a variable-length array.
std::size_t array_dim(St& st, const std::string& dim) {
    auto d = strip(dim);
    int64_t n = -1;
    if (!d.empty() && std::isdigit(static_cast<unsigned char>(d[0]))) {
        try {
            n = int_literal(d).first;
        } catch (const ParseFail&) {
            throw ParseFail("VLA unencoded");
        }
    } else if (is_ident(d) && st.enums.contains(d)) {
        n = st.enums.at(d);
    } else {
        throw ParseFail("VLA unencoded");
    }
    if (n <= 0 || n > (1 << 20)) throw ParseFail("UNENCODED: array of " + d + " elements");
    return static_cast<std::size_t>(n);
}

void CParser::decl(std::string s) {
    s = strip(s);
    while (!s.empty() && s.back() == ';') s.pop_back();
    s = strip(s);
    // Leading type words, then the declarators.
    std::vector<std::string> words;
    std::size_t pos = 0;
    while (true) {
        std::size_t e = pos;
        while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) ++e;
        auto w = s.substr(pos, e - pos);
        if (w.empty() || !kDeclKws.contains(w)) break;
        words.push_back(w);
        pos = e;
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    auto ct = words.empty() ? std::nullopt : scalar_ctype(join_sv(words, " "));
    if (!ct) throw ParseFail("unparsed decl: " + s.substr(0, 80));
    auto rest = s.substr(pos);
    static Regex one(R"(([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)(?:=\s*([\s\S]*))?$)");
    for (auto& part : split_top(rest, ',')) {
        auto d = strip(part);
        auto m = match_at(one, d);
        if (!m || static_cast<std::size_t>(m->spans[0].second) != d.size())
            throw ParseFail("unparsed decl: " + s.substr(0, 80));
        auto name = m->group(1);
        auto dims = strip(m->group(2));
        auto init = strip(m->group(3));
        bool has_init = m->spans.size() > 3 && m->spans[3].first >= 0;
        if (dims.empty()) {
            if (has_init && init.empty()) throw ParseFail("unparsed decl: " + s.substr(0, 80));
            st.types[name] = *ct;
            st.vars[name] = has_init ? norm(eval_typed(st, init).first, *ct) : 0;
            continue;
        }
        // Exactly one dimension: a 2-D array is not modelled.
        auto close = dims.find(']');
        if (!strip(dims.substr(close + 1)).empty())
            throw ParseFail("UNENCODED: multi-dimensional array " + name);
        auto dim_src = dims.substr(1, close - 1);
        std::vector<int64_t> vals;
        bool str_init = false;
        if (has_init && init.starts_with("{")) {
            if (!init.ends_with("}")) throw ParseFail("unparsed decl: " + s.substr(0, 80));
            auto inner = strip(init.substr(1, init.size() - 2));
            if (!inner.empty()) {
                auto elems = split_top(inner, ',');
                if (strip(elems.back()).empty()) elems.pop_back();  // trailing comma
                for (auto& e0 : elems) {
                    auto e = strip(e0);
                    if (e.starts_with("{")) throw ParseFail("UNENCODED: nested initialiser " + name);
                    if (e.starts_with("[") || e.starts_with("."))
                        throw ParseFail("UNENCODED: designated initialiser " + name);
                    vals.push_back(norm(eval_typed(st, e).first, *ct));
                }
            }
        } else if (has_init && init.starts_with("\"") && ct->w == 8) {
            auto toks = tok(init);
            if (toks.size() != 1) throw ParseFail("unparsed decl: " + s.substr(0, 80));
            for (unsigned char c : string_lit_value(toks[0])) vals.push_back(norm(c, *ct));
            vals.push_back(0);
            str_init = true;
        } else if (has_init) {
            throw ParseFail("unparsed decl: " + s.substr(0, 80));
        }
        std::size_t n = 0;
        if (strip(dim_src).empty()) {
            if (!has_init) throw ParseFail("unparsed decl: " + s.substr(0, 80));
            n = vals.size();
            if (!n) throw ParseFail("UNENCODED: zero-length array " + name);
        } else {
            n = array_dim(st, dim_src);
            // char s[3] = "abc": the terminating NUL is dropped (C11 6.7.9p14).
            if (str_init && vals.size() == n + 1) vals.pop_back();
            if (vals.size() > n) throw ParseFail("excess initialisers for " + name);
        }
        vals.resize(n, 0);
        st.arrays[name] = std::move(vals);
        st.arr_t[name] = *ct;
    }
}

void CParser::astore(const std::string& name, const std::string& idx, const std::string& rhs) {
    if (!st.arrays.contains(name)) throw ParseFail("unknown array " + name);
    auto i = eval_src(st, idx);
    auto v = eval_src(st, rhs);
    auto& arr = st.arrays.at(name);
    auto et = st.arr_t.contains(name) ? st.arr_t.at(name) : kInt;
    arr[checked_index(i, arr.size(), "MEM-OOB-WRITE")] = norm(v, et);
}

void CParser::assign_or_expr(std::string s) {
    s = strip(s);
    while (!s.empty() && s.back() == ';') s.pop_back();
    s = strip(s);
    if (s.empty()) return;
    auto parts = split_comma(s);
    if (parts.size() > 1) {
        for (auto& part : parts) {
            auto piece = strip(part);
            if (!piece.empty()) assign_or_expr(piece);
        }
        return;
    }
    static Regex arr("([A-Za-z_]\\w*)\\s*\\[(.+)\\]\\s*=\\s*(.+)$");
    if (auto m = match_at(arr, s)) {
        astore(m->group(1), m->group(2), m->group(3));
        return;
    }
    static Regex asg("([A-Za-z_]\\w*)\\s*(<<=|>>=|[+\\-*/%|&^]?=)\\s*(.+)$");
    if (auto m = match_at(asg, s)) {
        auto name = m->group(1);
        auto op = m->group(2);
        auto [val, rt] = eval_typed(st, m->group(3));
        CT vt = var_type(st, name);
        if (op == "=") {
            st.vars[name] = norm(val, vt);
        } else {
            // E1 op= E2 is E1 = E1 op E2 with E1 evaluated once (C11 6.5.16.2).
            auto cur = norm(st.vars.contains(name) ? st.vars[name] : 0, vt);
            auto bop = op.substr(0, op.size() - 1);
            int64_t r = 0;
            if (bop == "<<" || bop == ">>") {
                CT pl = promote(vt);
                r = shift(norm(cur, pl), bop, norm(val, promote(rt)), pl);
            } else {
                CT ct = common_type(vt, rt);
                r = binop(norm(cur, ct), bop, norm(val, ct), ct);
            }
            st.vars[name] = norm(r, vt);
        }
        return;
    }
    eval_src(st, s);
}

std::string CParser::do_assert(const std::string& text) {
    static Regex re("assert\\s*\\((.*)\\)\\s*;", false, true);
    if (auto m = match_at(re, text)) {
        if (!truth(eval_src(st, m->group(1)))) throw UB("FUNC-CONTRACT");
        return text.substr(static_cast<std::size_t>(m->spans[0].second));
    }
    auto lp = text.find('(');
    if (lp == std::string::npos) throw ParseFail("assert");
    auto [inner, rest] = paren(text.substr(lp));
    rest = lstrip(rest);
    if (rest.starts_with(";")) rest = rest.substr(1);
    if (!truth(eval_src(st, inner))) throw UB("FUNC-CONTRACT");
    return rest;
}

std::string CParser::do_if(const std::string& text) {
    auto rest = lstrip(text.substr(2));
    auto [cond, after] = paren(rest);
    std::string then_src;
    std::tie(then_src, after) = take_block(after);
    std::optional<std::string> else_src;
    auto stripped = lstrip(after);
    if (starts_kw(stripped, "else")) {
        auto [es, af] = take_block(stripped.substr(4));
        else_src = std::move(es);
        after = std::move(af);
    }
    if (truth(eval_src(st, cond))) stmts(then_src);
    else if (else_src) stmts(*else_src);
    return after;
}

std::string CParser::do_while(const std::string& text) {
    auto rest = lstrip(text.substr(5));
    auto [cond, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    while (truth(eval_src(st, cond))) {
        st.tick();
        if (!run_loop_body(body)) break;
    }
    return after;
}

std::string CParser::do_do(const std::string& text) {
    auto rest = lstrip(text.substr(2));
    auto [body, after] = take_block(rest);
    after = lstrip(after);
    if (!starts_kw(after, "while")) throw ParseFail("do without while");
    after = lstrip(after.substr(5));
    std::string cond;
    std::tie(cond, after) = paren(after);
    after = lstrip(after);
    if (after.starts_with(";")) after = after.substr(1);
    if (!run_loop_body(body)) return after;
    while (truth(eval_src(st, cond))) {
        st.tick();
        if (!run_loop_body(body)) break;
    }
    return after;
}

std::string CParser::do_for(const std::string& text) {
    auto rest = lstrip(text.substr(3));
    auto [head, after] = paren(rest);
    auto parts = split_semi(head);
    while (parts.size() < 3) parts.emplace_back();
    auto init = strip(parts[0]), cond = strip(parts[1]), incr = strip(parts[2]);
    if (!init.empty()) {
        auto init_stmt = init.ends_with(";") ? init : init + ";";
        if (auto miss = unencoded_layout_stmt(init_stmt)) throw ParseFail(*miss);
        if (looks_like_decl(init_stmt)) decl(init_stmt);
        else assign_or_expr(init_stmt);
    }
    std::string body;
    std::tie(body, after) = take_block(after);
    if (cond.empty()) cond = "1";
    while (truth(eval_src(st, cond))) {
        st.tick();
        if (!run_loop_body(body)) break;
        if (!incr.empty()) assign_or_expr(incr.ends_with(";") ? incr : incr + ";");
    }
    return after;
}

struct SwitchArm {
    std::vector<std::optional<int64_t>> labels;
    std::string code;
    bool stops = false;
};

// Case labels are converted to the promoted type of the controlling
// expression (C11 6.8.4.2p5).
std::vector<SwitchArm> parse_switch_arms(CParser& p, std::string text, CT scrut_t) {
    std::vector<SwitchArm> arms;
    std::vector<std::optional<int64_t>> labels;
    std::vector<std::string> chunks;
    bool stops = false;
    auto flush = [&] {
        if (!labels.empty() || !chunks.empty())
            arms.push_back({labels, join_sv(chunks, "\n"), stops});
        labels.clear();
        chunks.clear();
        stops = false;
    };
    while (!text.empty()) {
        text = lstrip(text);
        if (text.empty()) break;
        if (starts_kw(text, "case")) {
            if (!chunks.empty() || stops) flush();
            auto rest = lstrip(text.substr(4));
            auto [src, nxt] = upto_colon(rest);
            labels.push_back(norm(eval_src(p.st, src), scrut_t));
            text = nxt;
            continue;
        }
        if (starts_kw(text, "default")) {
            if (!chunks.empty() || stops) flush();
            auto rest = lstrip(text.substr(7));
            if (!rest.starts_with(":")) throw ParseFail("expected : after default");
            labels.push_back(std::nullopt);
            text = rest.substr(1);
            continue;
        }
        if (starts_kw(text, "break")) {
            std::tie(std::ignore, text) = stmt(text);
            stops = true;
            continue;
        }
        std::string src;
        std::tie(src, text) = consume_stmt_src(text);
        if (stops) continue;
        if (!strip(src).empty()) chunks.push_back(strip(src));
    }
    flush();
    return arms;
}

std::pair<std::string, std::string> consume_stmt_src(std::string raw) {
    auto text = lstrip(raw);
    std::size_t skip = raw.size() - text.size();
    auto taken = [&](const std::string& rest) -> std::pair<std::string, std::string> {
        auto idx = raw.size() - rest.size();
        return {raw.substr(skip, idx - skip), rest};
    };
    if (text.empty()) return {"", ""};
    if (text.starts_with("{")) {
        auto [_, rest] = brace(text);
        return taken(rest);
    }
    if (starts_kw(text, "if") || starts_kw(text, "for") || starts_kw(text, "while") || starts_kw(text, "switch") ||
        starts_kw(text, "do")) {
        std::string rest = text;
        if (starts_kw(rest, "do")) {
            rest = lstrip(rest.substr(2));
            std::tie(std::ignore, rest) = take_block(rest);
            rest = lstrip(rest);
            if (starts_kw(rest, "while")) {
                rest = lstrip(rest.substr(5));
                std::tie(std::ignore, rest) = paren(rest);
                rest = lstrip(rest);
                if (rest.starts_with(";")) rest = rest.substr(1);
            }
            return taken(rest);
        }
        std::size_t kw = starts_kw(text, "if") ? 2 : starts_kw(text, "for") ? 3 : starts_kw(text, "while") ? 5 : 6;
        rest = lstrip(text.substr(kw));
        std::tie(std::ignore, rest) = paren(rest);
        std::tie(std::ignore, rest) = take_block(rest);
        auto stripped = lstrip(rest);
        if (starts_kw(text, "if") && starts_kw(stripped, "else")) {
            std::tie(std::ignore, rest) = take_block(stripped.substr(4));
        }
        return taken(rest);
    }
    auto [st, rest] = looks_like_decl(text) ? decl_stmt(text) : stmt(text);
    return taken(rest);
}

std::string CParser::do_switch(const std::string& text) {
    auto rest = lstrip(text.substr(6));
    auto [cond, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    auto [scrut0, scrut_t0] = eval_typed(st, cond);
    CT scrut_t = promote(scrut_t0);
    auto scrut = norm(scrut0, scrut_t);
    auto arms = parse_switch_arms(*this, body, scrut_t);
    if (arms.empty()) return after;
    int idx = -1, def = -1;
    for (int i = 0; i < static_cast<int>(arms.size()); ++i) {
        for (auto& lab : arms[static_cast<std::size_t>(i)].labels) {
            if (!lab) def = i;
            else if (*lab == scrut) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) break;
    }
    if (idx < 0) idx = def;
    if (idx < 0) return after;
    try {
        for (int j = idx; j < static_cast<int>(arms.size()); ++j) {
            if (!strip(arms[static_cast<std::size_t>(j)].code).empty())
                stmts(arms[static_cast<std::size_t>(j)].code);
            if (arms[static_cast<std::size_t>(j)].stops) break;
        }
    } catch (const BreakEx&) {
    }
    return after;
}

void CParser::stmts(std::string text) {
    text = strip(text);
    while (!text.empty()) {
        st.tick();
        text = lstrip(text);
        if (text.empty()) break;
        if (text.starts_with("{")) {
            auto [inner, rest] = brace(text);
            stmts(inner);
            text = rest;
            continue;
        }
        if (starts_kw(text, "if")) {
            text = do_if(text);
            continue;
        }
        if (starts_kw(text, "switch")) {
            text = do_switch(text);
            continue;
        }
        if (starts_kw(text, "do")) {
            text = do_do(text);
            continue;
        }
        if (starts_kw(text, "while")) {
            text = do_while(text);
            continue;
        }
        if (starts_kw(text, "for")) {
            text = do_for(text);
            continue;
        }
        if (starts_kw(text, "assert")) {
            text = do_assert(text);
            continue;
        }
        if (starts_kw(text, "return")) {
            auto [stt, rest] = stmt(text);
            text = rest;
            auto restv = strip(stt);
            if (starts_kw(restv, "return")) restv = strip(restv.substr(6));
            if (!restv.empty() && restv.back() == ';') restv.pop_back();
            restv = strip(restv);
            std::optional<int64_t> val;
            if (!restv.empty()) {
                val = eval_src(st, restv);
                if (!st.ret_void) val = norm(*val, st.ret);
            }
            throw ReturnEx(val);
        }
        if (starts_kw(text, "break")) {
            std::tie(std::ignore, text) = stmt(text);
            throw BreakEx();
        }
        if (starts_kw(text, "continue")) {
            std::tie(std::ignore, text) = stmt(text);
            throw ContinueEx();
        }
        if (is_nested_function(text)) throw ParseFail("nested function unencoded");
        if (starts_kw(text, "goto")) {
            if (is_computed_goto(text)) throw ParseFail("computed goto unencoded");
            throw ParseFail("goto unencoded");
        }
        if (starts_kw(text, "throw")) throw ParseFail("throw unencoded");
        if (starts_kw(text, "asm") || starts_kw(text, "__asm__") || starts_kw(text, "__asm"))
            throw ParseFail("asm unencoded");
        if (starts_kw(text, "try") || starts_kw(text, "catch")) throw ParseFail("try unencoded");
        if (starts_kw(text, "case") || starts_kw(text, "default")) throw ParseFail("case/default outside switch");
        if (auto miss = unencoded_layout_prefix(text)) throw ParseFail(*miss);
        const bool is_decl = looks_like_decl(text);
        auto [stt, rest] = is_decl ? decl_stmt(text) : stmt(text);
        text = rest;
        if (auto miss = unencoded_layout_stmt(stt)) throw ParseFail(*miss);
        if (is_decl)
            decl(stt);
        else
            assign_or_expr(stt);
    }
}

}  // namespace

namespace stages_detail {
ExecResult execute(const FunctionInfo& fn, const Args& args,
                   std::optional<std::map<std::string, int>> enums) {
    if (fn.kind == "POINTER") return {"", std::nullopt, "skip-pointer", 0};
    auto en = enums ? *enums : enums_for(fn);
    St st(fn.params, args, std::move(en));
    if (re_search("^\\s*(?:(?:static|inline|extern)\\s+)*void\\s*$", fn.return_type)) st.ret_void = true;
    else if (auto rt = scalar_ctype(fn.return_type)) st.ret = *rt;
    try {
        CParser(fn.body, st).run();
    } catch (const UB& u) {
        return {u.cls, std::nullopt, "", st.steps};
    } catch (const ReturnEx& r) {
        return {"", r.value, "", st.steps};
    } catch (const ParseFail& ex) {
        return {"", std::nullopt, ex.what(), st.steps};
    } catch (const BreakEx&) {
        return {"", st.vars.contains("__ret") ? std::optional<int64_t>(st.vars["__ret"]) : std::nullopt, "",
                st.steps};
    } catch (const std::exception& ex) {
        return {"", std::nullopt, ex.what(), st.steps};
    }
    return {"", st.vars.contains("__ret") ? std::optional<int64_t>(st.vars["__ret"]) : std::nullopt, "", st.steps};
}

std::optional<bool> eval_cond(const FunctionInfo& fn, const Args& args, const std::string& cond) {
    try {
        auto en = enums_for(fn);
        St st(fn.params, args, en);
        return truth(eval_src(st, cond));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace stages_detail

ConcreteRec concrete_execute(const FunctionInfo& fn, const std::map<std::string, int>& args) {
    auto rec = execute(fn, Args(args.begin(), args.end()));
    ConcreteRec out;
    out.ub = rec.ub;
    out.rc = rec.value ? static_cast<int>(*rec.value) : 0;
    return out;
}

}  // namespace prism
