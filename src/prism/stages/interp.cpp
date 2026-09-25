// The concrete C interpreter (the Python engine prism/concrete.py):
// execute, eval_src, eval_cond and the public concrete_execute.
#include "interp.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
const std::unordered_set<std::string> kCastWords = {
    "char", "short", "int", "long", "unsigned", "signed", "const", "volatile", "void",
    "_Bool", "bool", "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
};

const std::unordered_set<std::string> kDeclKws = {
    "int", "unsigned", "long", "short", "char", "uint32_t", "int32_t", "uint64_t", "int64_t",
    "size_t",
};

const std::unordered_set<std::string> kStmtStartWords = {
    "if",     "for",        "while",    "switch",   "return",   "sizeof",   "typeof",
    "else",   "do",         "case",     "default",  "goto",     "break",    "continue",
    "assert", "throw",      "try",      "catch",    "asm",      "__asm__",  "__asm",
    "typedef","static",     "extern",   "auto",     "register", "int",      "unsigned",
    "signed", "long",       "short",    "char",     "uint32_t", "int32_t",  "uint64_t",
    "int64_t","size_t",     "void",     "float",    "double",   "_Bool",    "bool",
    "const",  "volatile",   "_Atomic",  "struct",   "union",    "enum",
};

const std::map<std::string, int> kTypeSize = {
    {"char", 1}, {"signed char", 1}, {"unsigned char", 1},
    {"short", 2}, {"short int", 2}, {"signed short", 2}, {"unsigned short", 2},
    {"int", 4}, {"signed", 4}, {"signed int", 4}, {"unsigned", 4}, {"unsigned int", 4},
    {"long", 4}, {"long int", 4}, {"unsigned long", 4},
    {"long long", 8}, {"long long int", 8}, {"unsigned long long", 8},
    {"uint32_t", 4}, {"int32_t", 4}, {"size_t", 4}, {"_Bool", 1}, {"bool", 1},
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
    std::string t = lower_copy(std::string(typ));
    if (t.find("long long") != std::string::npos || re_search("\\b[iu]nt64_t\\b", t)) return 64;
    return WIDTH;
}

}  // namespace stages_detail

namespace {
uint32_t u32(int64_t x) { return static_cast<uint32_t>(static_cast<uint64_t>(x) & 0xFFFFFFFFu); }

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

std::vector<std::string> tok(const std::string& src) {
    static Regex rx(
        R"(0x[0-9a-fA-F]+|\d+|'(?:\\.|[^\\'])'|"(?:\\.|[^\\"])*"|[A-Za-z_]\w*|&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--|[+\-*/%<>=!&|^~()[\],?:])");
    std::vector<std::string> out;
    for (auto& m : rx.finditer(src)) out.push_back(m.text);
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
        R"((?:int|unsigned(?:\\s+int)?|long|short|char|uint32_t|int32_t|size_t)\\s+\\w+\\s*=\\s*\\([^)]*\\)\\s*\\{)");
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

int sizeof_concrete(const std::vector<std::string>& inner, const std::map<std::string, std::vector<int64_t>>& arrays) {
    if (inner.empty()) return WIDTH / 8;
    for (auto& t : inner)
        if (t == "*") return WIDTH / 8;
    std::string joined = join_sv(inner, " ");
    auto it = kTypeSize.find(joined);
    if (it != kTypeSize.end()) return it->second;
    if (inner.size() == 1 && is_ident(inner[0])) {
        auto a = arrays.find(inner[0]);
        if (a != arrays.end()) return static_cast<int>(a->second.size()) * (WIDTH / 8);
        return WIDTH / 8;
    }
    return WIDTH / 8;
}

enum class Nk { Num, Id, Idx, Un, Pre, Post, Str, Call, Tern, Comma, Bin };

struct Node {
    Nk k{};
    std::string a;
    int64_t n = 0;
    std::string s;
    std::vector<Node> ch;
};

int64_t binop(int64_t a, const std::string& op, int64_t b, bool uns, int width);

int64_t eval_tree(const Node& t, St& st);

bool tree_unsigned(const Node& t, const St& st);

int tree_width(const Node& t, const St& st);

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
    Node nud() {
        auto t = eat();
        if (t == "sizeof") {
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
                return Node{Nk::Num, {}, sizeof_concrete(inner, st.arrays)};
            }
            auto name = eat();
            return Node{Nk::Num, {}, sizeof_concrete({name}, st.arrays)};
        }
        if (t == "(") {
            if (peek() == "{") throw ParseFail("statement-expr unencoded");
            if (kCastWords.contains(peek())) {
                while (!peek().empty() && peek() != ")") {
                    if (!kCastWords.contains(peek()) && peek() != "*") break;
                    eat();
                }
                eat_s(")");
                return parse(110);
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
        if (t == "++" || t == "--") {
            auto name = eat();
            if (!is_ident(name)) throw ParseFail("bad token " + name);
            return Node{Nk::Pre, t, 0, name};
        }
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'')
            return Node{Nk::Num, {}, char_lit_value(t)};
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
            return Node{Nk::Str, {}, 0, string_lit_value(t)};
        if (std::isdigit(static_cast<unsigned char>(t[0])) || t.starts_with("0x") || t.starts_with("0X")) {
            try {
                return Node{Nk::Num, {}, std::stoll(t, nullptr, 0)};
            } catch (...) {
                throw ParseFail("bad num " + t);
            }
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

bool tree_unsigned(const Node& t, const St& st) {
    if (t.k == Nk::Id) return st.uns.contains(t.a);
    if (t.k == Nk::Bin) return tree_unsigned(t.ch[0], st) || tree_unsigned(t.ch[1], st);
    if (t.k == Nk::Un) return tree_unsigned(t.ch[0], st);
    if (t.k == Nk::Tern) return tree_unsigned(t.ch[1], st) || tree_unsigned(t.ch[2], st);
    if (t.k == Nk::Comma) return tree_unsigned(t.ch[1], st);
    return false;
}

int tree_width(const Node& t, const St& st) {
    if (t.k == Nk::Id) {
        auto it = st.bits.find(t.a);
        return it == st.bits.end() ? WIDTH : it->second;
    }
    if (t.k == Nk::Post || t.k == Nk::Pre) {
        auto it = st.bits.find(t.s);
        return it == st.bits.end() ? WIDTH : it->second;
    }
    if (t.k == Nk::Bin) return std::max(tree_width(t.ch[0], st), tree_width(t.ch[1], st));
    if (t.k == Nk::Un) return tree_width(t.ch[0], st);
    if (t.k == Nk::Tern) return std::max(tree_width(t.ch[1], st), tree_width(t.ch[2], st));
    if (t.k == Nk::Comma) return tree_width(t.ch[1], st);
    return WIDTH;
}

int64_t binop(int64_t a, const std::string& op, int64_t b, bool uns, int width) {
    if (uns) {
        uint32_t ua = u32(a), ub = u32(b);
        if (op == "+") return i32(static_cast<int64_t>(ua) + ub);
        if (op == "-") return i32(static_cast<int64_t>(ua) - ub);
        if (op == "*") return i32(static_cast<int64_t>(ua) * ub);
        if (op == "/") {
            if (ub == 0) throw UB("INT-DIV-ZERO");
            return i32(ua / ub);
        }
        if (op == "%") {
            if (ub == 0) throw UB("INT-DIV-ZERO");
            return i32(ua % ub);
        }
        if (op == "<<") {
            if (ub >= static_cast<uint32_t>(WIDTH)) throw UB("INT-SHIFT-UB");
            return i32(static_cast<int64_t>(ua) << ub);
        }
        if (op == ">>") {
            if (ub >= static_cast<uint32_t>(WIDTH)) throw UB("INT-SHIFT-UB");
            return i32(ua >> ub);
        }
        if (op == "&") return i32(ua & ub);
        if (op == "|") return i32(ua | ub);
        if (op == "^") return i32(ua ^ ub);
        if (op == "==") return ua == ub;
        if (op == "!=") return ua != ub;
        if (op == "<") return ua < ub;
        if (op == ">") return ua > ub;
        if (op == "<=") return ua <= ub;
        if (op == ">=") return ua >= ub;
        throw ParseFail("op " + op);
    }
    if (width >= 64) {
        if (op == "+") {
            auto r = a + b;
            if (r < INT64_MIN_V || r > INT64_MAX_V) throw UB("INT-SIGNED-OVF");
            return r;
        }
        if (op == "-") {
            auto r = a - b;
            if (r < INT64_MIN_V || r > INT64_MAX_V) throw UB("INT-SIGNED-OVF");
            return r;
        }
        if (op == "*") {
            auto r = a * b;
            if (r < INT64_MIN_V || r > INT64_MAX_V) throw UB("INT-SIGNED-OVF");
            return r;
        }
        if (op == "/") {
            if (b == 0) throw UB("INT-DIV-ZERO");
            if (a == INT64_MIN_V && b == -1) throw UB("INT-SIGNED-OVF");
            return a / b;
        }
        if (op == "%") {
            if (b == 0) throw UB("INT-DIV-ZERO");
            if (a == INT64_MIN_V && b == -1) throw UB("INT-SIGNED-OVF");  // C11 6.5.5p6
            return a - (a / b) * b;
        }
        if (op == "<<") {
            if (b < 0 || b >= 64) throw UB("INT-SHIFT-UB");
            // Negative operand or unrepresentable result (C11 6.5.7p4).
            if (a < 0 || a > (INT64_MAX_V >> b)) throw UB("INT-SHIFT-UB");
            return a << b;
        }
        if (op == ">>") {
            if (b < 0 || b >= 64) throw UB("INT-SHIFT-UB");
            return a >> b;
        }
        if (op == "&") return a & b;
        if (op == "|") return a | b;
        if (op == "^") return a ^ b;
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == ">") return a > b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        throw ParseFail("op " + op);
    }
    a = i32(a);
    b = i32(b);
    if (op == "+") {
        auto r = a + b;
        if (r < INT_MIN_32 || r > INT_MAX_32) throw UB("INT-SIGNED-OVF");
        return r;
    }
    if (op == "-") {
        auto r = a - b;
        if (r < INT_MIN_32 || r > INT_MAX_32) throw UB("INT-SIGNED-OVF");
        return r;
    }
    if (op == "*") {
        auto r = a * b;
        if (r < INT_MIN_32 || r > INT_MAX_32) throw UB("INT-SIGNED-OVF");
        return r;
    }
    if (op == "/") {
        if (b == 0) throw UB("INT-DIV-ZERO");
        if (a == INT_MIN_32 && b == -1) throw UB("INT-SIGNED-OVF");
        return a / b;
    }
    if (op == "%") {
        if (b == 0) throw UB("INT-DIV-ZERO");
        if (a == INT_MIN_32 && b == -1) throw UB("INT-SIGNED-OVF");  // C11 6.5.5p6
        return a - (a / b) * b;
    }
    if (op == "<<") {
        if (b < 0 || b >= WIDTH) throw UB("INT-SHIFT-UB");
        // Negative operand or unrepresentable result (C11 6.5.7p4).
        if (a < 0 || a > (INT_MAX_32 >> b)) throw UB("INT-SHIFT-UB");
        return i32(a << b);
    }
    if (op == ">>") {
        if (b < 0 || b >= WIDTH) throw UB("INT-SHIFT-UB");
        return i32(a >> b);
    }
    if (op == "&") return i32(a & b);
    if (op == "|") return i32(a | b);
    if (op == "^") return i32(a ^ b);
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<") return a < b;
    if (op == ">") return a > b;
    if (op == "<=") return a <= b;
    if (op == ">=") return a >= b;
    throw ParseFail("op " + op);
}

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

int64_t eval_tree(const Node& t, St& st) {
    switch (t.k) {
    case Nk::Num: return i32(t.n);
    case Nk::Id: {
        auto it = st.vars.find(t.a);
        if (it != st.vars.end()) {
            int w = st.bits.contains(t.a) ? st.bits[t.a] : WIDTH;
            return w <= 32 ? i32(it->second) : it->second;
        }
        if (st.enums.contains(t.a)) return i32(st.enums[t.a]);
        st.vars[t.a] = 0;
        return 0;
    }
    case Nk::Idx: {
        auto it = st.arrays.find(t.a);
        if (it == st.arrays.end()) throw ParseFail("unknown array " + t.a);
        auto idx = eval_tree(t.ch[0], st);
        auto& arr = it->second;
        if (tree_unsigned(t.ch[0], st)) {
            auto ui = u32(idx);
            if (ui >= arr.size()) throw UB("MEM-OOB-READ");
            return i32(arr[ui]);
        }
        if (idx < 0 || static_cast<std::size_t>(idx) >= arr.size()) throw UB("MEM-OOB-READ");
        return i32(arr[static_cast<std::size_t>(idx)]);
    }
    case Nk::Un: {
        auto v = eval_tree(t.ch[0], st);
        if (t.a == "-") {
            if (!tree_unsigned(t.ch[0], st) && v == INT_MIN_32) throw UB("INT-SIGNED-OVF");
            return i32(-v);
        }
        if (t.a == "!") return truth(v) ? 0 : 1;
        if (t.a == "~") return i32(~(v & 0xFFFFFFFF));
        throw ParseFail("unop " + t.a);
    }
    case Nk::Post:
    case Nk::Pre: {
        auto cur = st.vars.contains(t.s) ? st.vars[t.s] : 0;
        int w = st.bits.contains(t.s) ? st.bits[t.s] : WIDTH;
        bool u = st.uns.contains(t.s);
        auto nw = binop(cur, t.a == "++" ? "+" : "-", 1, u, w);
        st.vars[t.s] = nw;
        auto v = t.k == Nk::Post ? cur : nw;
        return w <= 32 ? i32(v) : v;
    }
    case Nk::Str: return 1;
    case Nk::Call: {
        if ((t.a == "strcpy" || t.a == "strcat") && t.ch.size() >= 2) {
            eval_cstr_copy(st, t.ch[0], t.ch[1], std::nullopt, t.a == "strcat");
            return 0;
        }
        if (t.a == "strncpy" && t.ch.size() >= 3) {
            auto n = eval_tree(t.ch[2], st);
            eval_cstr_copy(st, t.ch[0], t.ch[1], static_cast<int>(std::max<int64_t>(0, n)), false);
            return 0;
        }
        for (auto& a : t.ch) eval_tree(a, st);
        return 0;
    }
    case Nk::Tern:
        return truth(eval_tree(t.ch[0], st)) ? eval_tree(t.ch[1], st) : eval_tree(t.ch[2], st);
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
        auto a = eval_tree(t.ch[0], st);
        auto b = eval_tree(t.ch[1], st);
        bool u = tree_unsigned(t.ch[0], st) || tree_unsigned(t.ch[1], st);
        int w = std::max(tree_width(t.ch[0], st), tree_width(t.ch[1], st));
        return binop(a, t.a, b, u, w);
    }
    }
    throw ParseFail("bad tree");
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

int64_t eval_src(St& st, const std::string& src) {
    EParser p{st, tok(strip(src)), 0};
    auto tree = p.parse(0);
    if (p.pos != p.tokens.size()) throw ParseFail("trailing tokens");
    auto v = eval_tree(tree, st);
    return tree_width(tree, st) >= 64 ? v : i32(v);
}

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

void CParser::decl(std::string s) {
    s = strip(s);
    while (!s.empty() && s.back() == ';') s.pop_back();
    s = strip(s);
    static Regex re(
        "(?:int|unsigned(?:\\s+int)?|long|short|char|uint32_t|int32_t|size_t)"
        "\\s+([A-Za-z_]\\w*)(?:\\s*\\[(\\d+)\\])?(?:\\s*=\\s*(.*))?$");
    auto m = match_at(re, s);
    if (!m) {
        if (re_search("\\[[^\\]]+\\]", s)) throw ParseFail("VLA unencoded");
        throw ParseFail("unparsed decl: " + s.substr(0, 80));
    }
    auto name = m->group(1);
    auto dim = m->group(2);
    auto init = m->group(3);
    if (!dim.empty()) {
        st.arrays[name] = std::vector<int64_t>(static_cast<std::size_t>(std::stoi(dim)), 0);
        return;
    }
    auto start1 = m->spans.size() > 1 ? m->spans[1].first : 0;
    if (type_is_unsigned(s.substr(0, static_cast<std::size_t>(std::max(0, start1))))) st.uns.insert(name);
    st.vars[name] = init.empty() ? 0 : i32(eval_src(st, init));
}

void CParser::astore(const std::string& name, const std::string& idx, const std::string& rhs) {
    auto it = st.arrays.find(name);
    if (it == st.arrays.end()) throw ParseFail("unknown array " + name);
    auto i = eval_src(st, idx);
    auto v = eval_src(st, rhs);
    bool uidx = is_ident(strip(idx)) && st.uns.contains(strip(idx));
    if (uidx) {
        auto ui = u32(i);
        if (ui >= it->second.size()) throw UB("MEM-OOB-WRITE");
        it->second[ui] = i32(v);
        return;
    }
    if (i < 0 || static_cast<std::size_t>(i) >= it->second.size()) throw UB("MEM-OOB-WRITE");
    it->second[static_cast<std::size_t>(i)] = i32(v);
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
    static Regex asg("([A-Za-z_]\\w*)\\s*([+\\-*/%|&^]?=)\\s*(.+)$");
    if (auto m = match_at(asg, s)) {
        auto name = m->group(1);
        auto op = m->group(2);
        auto val = eval_src(st, m->group(3));
        int w = st.bits.contains(name) ? st.bits[name] : WIDTH;
        if (op == "=") {
            st.vars[name] = w <= 32 ? i32(val) : val;
        } else {
            auto cur = st.vars.contains(name) ? st.vars[name] : 0;
            auto r = binop(cur, op.substr(0, 1), val, st.uns.contains(name), w);
            st.vars[name] = w <= 32 ? i32(r) : r;
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
        assign_or_expr(init_stmt);
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

std::vector<SwitchArm> parse_switch_arms(CParser& p, std::string text) {
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
            labels.push_back(eval_src(p.st, src));
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
    auto [st, rest] = stmt(text);
    return taken(rest);
}

std::string CParser::do_switch(const std::string& text) {
    auto rest = lstrip(text.substr(6));
    auto [cond, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    auto scrut = eval_src(st, cond);
    auto arms = parse_switch_arms(*this, body);
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
            if (!restv.empty()) val = eval_src(st, restv);
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
        auto [stt, rest] = stmt(text);
        text = rest;
        if (auto miss = unencoded_layout_stmt(stt)) throw ParseFail(*miss);
        if (stt.starts_with("int ") || stt.starts_with("unsigned ") || stt.starts_with("long ") ||
            stt.starts_with("short ") || stt.starts_with("char ") || stt.starts_with("uint32_t ") ||
            stt.starts_with("int32_t ") || stt.starts_with("size_t "))
            decl(stt);
        else
            assign_or_expr(stt);
    }
}

}  // namespace

namespace stages_detail {
ExecResult execute(const FunctionInfo& fn, const std::map<std::string, int>& args,
                   std::optional<std::map<std::string, int>> enums) {
    if (fn.kind == "POINTER") return {"", std::nullopt, "skip-pointer", 0};
    auto en = enums ? *enums : enums_for(fn);
    St st(fn.params, args, std::move(en));
    try {
        CParser(fn.body, st).run();
    } catch (const UB& u) {
        return {u.cls, std::nullopt, "", st.steps};
    } catch (const ReturnEx& r) {
        return {"", r.value ? std::optional<int64_t>(i32(*r.value)) : std::nullopt, "", st.steps};
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
    auto rec = execute(fn, args);
    ConcreteRec out;
    out.ub = rec.ub;
    out.rc = rec.value ? static_cast<int>(*rec.value) : 0;
    return out;
}

}  // namespace prism
