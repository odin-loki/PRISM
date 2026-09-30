#include "prism/stages.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include "prism/cparse.hpp"
#include "stages/interp.hpp"

#include <algorithm>
#include <fstream>
#include <format>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prism {
namespace {

constexpr int kWidth = 32;
constexpr int64_t kIntMin = -(int64_t{1} << (kWidth - 1));
constexpr int64_t kIntMax = (int64_t{1} << (kWidth - 1)) - 1;

// A range of values of a type of width w (32, or 64 for long, long long,
// size_t and the 64-bit typedefs: LP64, as the bmc encoder). An unsigned
// range is kept in the signed range of its width (the domain never alarms on
// unsigned wrap).
struct R {
    int64_t lo = 0;
    int64_t hi = 0;
    bool unsigned_ = false;
    int w = kWidth;
    bool empty() const { return lo > hi; }
    bool contains(int64_t v) const { return lo <= v && v <= hi; }
};

int64_t tmin(int w) { return w >= 64 ? std::numeric_limits<int64_t>::min() : kIntMin; }
int64_t tmax(int w) { return w >= 64 ? std::numeric_limits<int64_t>::max() : kIntMax; }
R top(int w, bool u = false) { return R{tmin(w), tmax(w), u, w}; }

const R kTop{kIntMin, kIntMax, false};
const R kBot{1, 0, false};
const R kCallDummy{1, 1, false};

const char* kDeclType =
    "(?:unsigned\\s+long\\s+long(?:\\s+int)?|long\\s+long(?:\\s+int)?|"
    "unsigned\\s+long(?:\\s+int)?|uint64_t|int64_t|"
    "unsigned(?:\\s+int)?|int|long|short|char|uint32_t|int32_t|size_t)";

const std::unordered_set<std::string> kCastWords = {
    "char", "short", "int", "long", "unsigned", "signed",
    "const", "volatile", "void", "_Bool", "bool",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
};

const std::unordered_set<std::string> kDeclKws = {
    "int", "unsigned", "long", "short", "char",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
};

const std::unordered_set<std::string> kStmtStartWords = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "goto", "break", "continue",
    "assert", "throw", "try", "catch", "asm", "__asm__", "__asm",
    "typedef", "static", "extern", "auto", "register",
    "int", "unsigned", "signed", "long", "short", "char",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
    "void", "float", "double", "_Bool", "bool",
    "const", "volatile", "_Atomic", "struct", "union", "enum",
};

const std::unordered_map<std::string, int> kTypeSize = {
    {"char", 1}, {"signed char", 1}, {"unsigned char", 1},
    {"short", 2}, {"short int", 2}, {"signed short", 2}, {"unsigned short", 2},
    {"int", 4}, {"signed", 4}, {"signed int", 4}, {"unsigned", 4}, {"unsigned int", 4},
    {"long", 8}, {"long int", 8}, {"unsigned long", 8},
    {"long long", 8}, {"long long int", 8}, {"unsigned long long", 8},
    {"uint32_t", 4}, {"int32_t", 4}, {"uint64_t", 8}, {"int64_t", 8}, {"size_t", 8},
    {"_Bool", 1}, {"bool", 1},
};

// The width of a scalar type in this domain: 64 for a 64-bit type (LP64),
// 32 for every narrower integer type (read as int).
int type_bits(std::string_view typ) {
    auto ct = stages_detail::scalar_ctype(typ);
    return ct && ct->w >= 64 ? 64 : kWidth;
}

const char* kCmp[] = {"==", "!=", "<=", ">=", "<", ">"};

struct ParseFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Alarm : std::runtime_error {
    std::string cls;
    std::string msg;
    Alarm(std::string c, std::string m)
        : std::runtime_error(m), cls(std::move(c)), msg(std::move(m)) {}
};
struct ReturnEx : std::exception {};

std::string strip(std::string s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lstrip(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

bool is_ident(std::string_view t) {
    static Regex re("[A-Za-z_]\\w*");
    auto m = re.search_match(t, 0);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           static_cast<std::size_t>(m->spans[0].second) == t.size();
}

bool starts_kw(std::string_view text, std::string_view kw) {
    if (!text.starts_with(kw)) return false;
    if (text.size() == kw.size()) return true;
    unsigned char c = static_cast<unsigned char>(text[kw.size()]);
    return !(std::isalnum(c) || c == '_');
}

bool is_computed_goto(std::string_view text) {
    if (!starts_kw(text, "goto")) return false;
    auto rest = lstrip(std::string(text.substr(4)));
    return rest.starts_with("*");
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
    for (auto& kw : kDeclKws) {
        if (starts_kw(s, kw)) return true;
    }
    return false;
}

bool type_is_unsigned(std::string_view typ) {
    static Regex re("(?i)\\bunsigned\\b|\\bsize_t\\b|\\buint\\d*_t\\b|\\bu_int\\b|\\bu_long\\b");
    return re.search(typ);
}

std::optional<Match> match_at(const Regex& re, std::string_view s) {
    auto m = re.search_match(s, 0);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}

bool fullmatch(const Regex& re, std::string_view s) {
    auto m = match_at(re, s);
    return m && static_cast<std::size_t>(m->spans[0].second) == s.size();
}

std::vector<std::string> split_comma(const std::string& s) {
    std::vector<std::string> parts;
    int pdepth = 0, bdepth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '(') ++pdepth;
        else if (ch == ')') --pdepth;
        else if (ch == '[') ++bdepth;
        else if (ch == ']') --bdepth;
        if (ch == ',' && pdepth == 0 && bdepth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::pair<std::string, std::string> paren(std::string text) {
    text = lstrip(std::move(text));
    if (!text.starts_with("(")) throw ParseFail("expected (");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced (");
}

std::pair<std::string, std::string> brace(std::string text) {
    text = lstrip(std::move(text));
    if (!text.starts_with("{")) throw ParseFail("expected {");
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            --depth;
            if (depth == 0) return {text.substr(1, i - 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("unbalanced {");
}

std::pair<std::string, std::string> stmt(const std::string& text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == '{' && depth == 0) break;
        else if (ch == ';' && depth == 0) {
            return {text.substr(0, i + 1), text.substr(i + 1)};
        }
    }
    throw ParseFail("no semicolon in " + text.substr(0, std::min<std::size_t>(80, text.size())));
}

std::pair<std::string, std::string> take_block(std::string text) {
    text = lstrip(std::move(text));
    if (text.starts_with("{")) return brace(text);
    return stmt(text);
}

std::vector<std::string> tok(const std::string& src) {
    static Regex rx(
        R"(0x[0-9a-fA-F]+|\d+|'(?:\\.|[^\\'])'|"(?:\\.|[^\\"])*"|[A-Za-z_]\w*|&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--|[+\-*/%<>=!&|^~()[\],?:])");
    std::vector<std::string> out;
    for (auto& m : rx.finditer(src)) out.push_back(m.text);
    return out;
}

std::string replace_str_lits(const std::string& src) {
    static Regex re(R"re("([^"\\]|\\.)*")re");
    std::string out;
    std::size_t i = 0;
    for (auto& m : re.finditer(src)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        out.append(src, i, a - i);
        out += "1";
        i = b;
    }
    out.append(src, i, std::string::npos);
    return out;
}

std::vector<std::string> interval_tok(const std::string& src) {
    return tok(replace_str_lits(src.empty() ? "0" : src));
}

int char_lit_value(const std::string& tok_s) {
    if (tok_s.size() < 2) throw ParseFail("empty character literal");
    std::string inner = tok_s.substr(1, tok_s.size() - 2);
    if (inner.empty()) throw ParseFail("empty character literal");
    if (inner[0] == '\\' && inner.size() >= 2) {
        char esc = inner[1];
        switch (esc) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case '0': return 0;
        case '\\': return 92;
        case '\'': return 39;
        case '"': return 34;
        default: return static_cast<unsigned char>(esc);
        }
    }
    return static_cast<unsigned char>(inner[0]);
}

int sizeof_interval(const std::vector<std::string>& inner) {
    if (inner.empty()) return kWidth / 8;
    for (auto& t : inner) {
        if (t == "*") return 8;
    }
    std::string joined;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (i) joined += " ";
        joined += inner[i];
    }
    auto it = kTypeSize.find(joined);
    if (it != kTypeSize.end()) return it->second;
    return kWidth / 8;
}

R clip(__int128 lo, __int128 hi, bool uns = false, int w = kWidth) {
    return R{static_cast<int64_t>(std::max<__int128>(lo, tmin(w))),
             static_cast<int64_t>(std::min<__int128>(hi, tmax(w))), uns, w};
}

R join_r(const R& a, const R& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return R{std::min(a.lo, b.lo), std::max(a.hi, b.hi), a.unsigned_ && b.unsigned_, std::max(a.w, b.w)};
}

R meet_r(const R& a, const R& b) {
    if (a.empty() || b.empty()) return kBot;
    return R{std::max(a.lo, b.lo), std::min(a.hi, b.hi), a.unsigned_ || b.unsigned_, std::max(a.w, b.w)};
}

using State = std::map<std::string, R>;

State copy_state(const State& st) { return st; }

State join_state(const State& a, const State& b) {
    State out = copy_state(a);
    for (auto& [k, v] : b) {
        auto it = out.find(k);
        out[k] = (it != out.end()) ? join_r(it->second, v) : v;
    }
    for (auto& [k, v] : a) {
        if (!b.contains(k)) out[k] = join_r(v, top(v.w));
    }
    return out;
}

// A float/double parameter or local: outside the integer domain. An
// expression that reads one is unencoded (ParseFail), never an integer
// overflow alarm on `double add(double a, double b) { return a + b; }`.
bool is_float_type(std::string_view typ) {
    static Regex re(R"(\b(?:float|double|_Float\d+|__float128)\b)");
    return re.search(typ);
}

std::unordered_set<std::string> float_locals(std::string_view body) {
    static Regex decl(R"(\b(?:float|double|_Float\d+|__float128)\b([^;(){}]*);)");
    static Regex name(R"(^\s*\**\s*([A-Za-z_]\w*))");
    std::unordered_set<std::string> out;
    for (auto& m : decl.finditer(body)) {
        // `double a = 1, *b, c[4];`: the first identifier of each top-level
        // comma segment.
        auto list = m.group(1);
        int depth = 0;
        std::string seg;
        for (std::size_t i = 0; i <= list.size(); ++i) {
            char c = i < list.size() ? list[i] : ',';
            if (c == '[' || c == '(') ++depth;
            else if (c == ']' || c == ')') --depth;
            if (c == ',' && depth == 0) {
                if (auto nm = name.search_match(seg)) out.insert(nm->group(1));
                seg.clear();
            } else {
                seg.push_back(c);
            }
        }
    }
    return out;
}

struct Engine {
    State st;
    std::unordered_set<std::string> unsigned_names;
    std::unordered_set<std::string> wide_names;  // declared with a 64-bit type
    std::unordered_set<std::string> float_names;
    bool live = true;

    explicit Engine(const std::vector<std::pair<std::string, std::string>>& params) {
        for (auto& [typ, name] : params) {
            if (name.empty()) continue;
            if (is_float_type(typ)) {
                float_names.insert(name);
                continue;
            }
            bool u = type_is_unsigned(typ);
            if (u) unsigned_names.insert(name);
            int w = type_bits(typ);
            if (w >= 64) wide_names.insert(name);
            st[name] = top(w, u);
        }
    }

    int width_of(const std::string& name) const { return wide_names.contains(name) ? 64 : kWidth; }

    R get(const std::string& name) const {
        if (float_names.contains(name)) throw ParseFail("floating-point unencoded");
        auto it = st.find(name);
        R cur = it == st.end() ? top(width_of(name)) : it->second;
        cur.w = width_of(name);
        if (unsigned_names.contains(name)) cur.unsigned_ = true;
        return cur;
    }
    // Store r converted to the variable's type: a value of a wider type
    // that may not fit is the whole range of the narrower one.
    void set(const std::string& name, R r) {
        int w = width_of(name);
        if (!r.empty() && r.w > w && (r.lo < tmin(w) || r.hi > tmax(w))) r = top(w, r.unsigned_);
        r.w = w;
        if (unsigned_names.contains(name)) r.unsigned_ = true;
        st[name] = r;
    }
};

Engine fork_engine(const Engine& e) {
    Engine n({});
    n.st = copy_state(e.st);
    n.unsigned_names = e.unsigned_names;
    n.wide_names = e.wide_names;
    n.float_names = e.float_names;
    n.live = e.live;
    return n;
}

std::optional<std::string> unencoded_layout_prefix(std::string_view text);
std::optional<std::string> unencoded_layout_stmt(std::string_view stmt_s);

R eval_expr(Engine& e, const std::string& src);
R binop(Engine& e, const R& a, const std::string& op, const R& b);
void stmts(Engine& e, std::string text, int bound = 8);
void decl(Engine& e, std::string stmt_s);
void assign(Engine& e, std::string stmt_s);
void refine(Engine& e, const std::string& cond, bool truth);
std::string if_stmt(Engine& e, const std::string& text, int bound);
std::string while_stmt(Engine& e, const std::string& text, int bound);
std::string for_stmt(Engine& e, const std::string& text, int bound);
std::string do_stmt(Engine& e, const std::string& text, int bound);

bool out_of(__int128 v, int w) { return v < tmin(w) || v > tmax(w); }
bool ovf_add(int64_t a, int64_t b, int w = kWidth) { return out_of(static_cast<__int128>(a) + b, w); }
bool ovf_sub(int64_t a, int64_t b, int w = kWidth) { return out_of(static_cast<__int128>(a) - b, w); }

int64_t py_floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((a < 0) != (b < 0))) --q;
    return q;
}
int64_t py_mod(int64_t a, int64_t b) { return a - py_floordiv(a, b) * b; }

R uneg(Engine&, const R& v) {
    if (v.empty()) return v;
    if (v.contains(tmin(v.w))) {
        if (!v.unsigned_) throw Alarm("INT-SIGNED-OVF", v.w >= 64 ? "negation of LLONG_MIN" : "negation of INT_MIN");
        return top(v.w, true);
    }
    return R{-v.hi, -v.lo, v.unsigned_, v.w};
}

R binop(Engine&, const R& a, const std::string& op, const R& b) {
    if (a.empty() || b.empty()) return kBot;
    bool u = a.unsigned_ || b.unsigned_;
    // the usual arithmetic conversions: the wider operand's width
    const int w = std::max(a.w, b.w);
    if (op == "+") {
        bool ovf = ovf_add(a.lo, b.lo, w) || ovf_add(a.lo, b.hi, w) || ovf_add(a.hi, b.lo, w) || ovf_add(a.hi, b.hi, w);
        if (!u && ovf) throw Alarm("INT-SIGNED-OVF", "signed + may overflow");
        if (u && ovf) return top(w, true);
        return clip(static_cast<__int128>(a.lo) + b.lo, static_cast<__int128>(a.hi) + b.hi, u, w);
    }
    if (op == "-") {
        bool ovf = ovf_sub(a.lo, b.lo, w) || ovf_sub(a.lo, b.hi, w) || ovf_sub(a.hi, b.lo, w) || ovf_sub(a.hi, b.hi, w);
        if (!u && ovf) throw Alarm("INT-SIGNED-OVF", "signed - may overflow");
        if (u && ovf) return top(w, true);
        return clip(static_cast<__int128>(a.lo) - b.hi, static_cast<__int128>(a.hi) - b.lo, u, w);
    }
    if (op == "*") {
        __int128 corners[4] = {static_cast<__int128>(a.lo) * b.lo, static_cast<__int128>(a.lo) * b.hi,
                               static_cast<__int128>(a.hi) * b.lo, static_cast<__int128>(a.hi) * b.hi};
        for (auto c : corners) {
            if (out_of(c, w)) {
                if (u) return top(w, true);
                throw Alarm("INT-SIGNED-OVF", "signed * may overflow");
            }
        }
        return clip(*std::min_element(std::begin(corners), std::end(corners)),
                    *std::max_element(std::begin(corners), std::end(corners)), u, w);
    }
    if (op == "/" || op == "%") {
        if (b.contains(0)) throw Alarm("INT-DIV-ZERO", "divisor range includes 0");
        if (!u && a.contains(tmin(w)) && b.contains(-1))
            throw Alarm("INT-SIGNED-OVF", w >= 64 ? "LLONG_MIN / -1" : "INT_MIN / -1");
        if (b.lo == b.hi) {
            if (op == "/") {
                int64_t lo = py_floordiv(a.lo, b.lo), hi = py_floordiv(a.hi, b.lo);
                return R{std::min(lo, hi), std::max(lo, hi), u, w};
            }
            int64_t lo = py_mod(a.lo, b.lo), hi = py_mod(a.hi, b.lo);
            return R{std::min(lo, hi), std::max(lo, hi), u, w};
        }
        return top(w, u);
    }
    if (op == "<<" || op == ">>") {
        // the result has the (promoted) type of the left operand
        const int lw = a.w;
        const std::string range = lw >= 64 ? "shift amount out of 0..63" : "shift amount out of 0..31";
        if (u) {
            if (b.hi >= lw) throw Alarm("INT-SHIFT-UB", range);
        } else if (b.lo < 0 || b.hi >= lw) {
            throw Alarm("INT-SHIFT-UB", range);
        }
        if (!u && op == "<<" && a.contains(1) && b.hi >= lw - 1)
            throw Alarm("INT-SHIFT-UB", lw >= 64 ? "1<<63 is undefined for signed long" : "1<<31 is undefined for signed int");
        if (a.lo == a.hi && b.lo == b.hi && b.lo >= 0 && b.lo < lw) {
            __int128 v = op == "<<" ? static_cast<__int128>(a.lo) << b.lo : static_cast<__int128>(a.lo >> b.lo);
            if (!out_of(v, lw)) return R{static_cast<int64_t>(v), static_cast<int64_t>(v), u, lw};
        }
        return top(lw, u);
    }
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (a.lo == a.hi && b.lo == b.hi) {
            // two constants: compare in the common type
            auto x = static_cast<__int128>(a.lo), y = static_cast<__int128>(b.lo);
            if (u) {
                const __int128 m = static_cast<__int128>(1) << w;
                if (x < 0) x += m;
                if (y < 0) y += m;
            }
            bool r = op == "==" ? x == y : op == "!=" ? x != y : op == "<" ? x < y : op == ">" ? x > y
                   : op == "<=" ? x <= y : x >= y;
            return R{r, r, false};
        }
        return R{0, 1, false};
    }
    if (op == "&" || op == "|" || op == "^") return top(w, u);
    throw ParseFail("op " + op);
}

struct Parser {
    Engine& e;
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    const std::unordered_map<std::string, int> prec{
        {"||", 10}, {"&&", 20},
        {"==", 30}, {"!=", 30}, {"<", 30}, {">", 30}, {"<=", 30}, {">=", 30},
        {"+", 40}, {"-", 40},
        {"*", 50}, {"/", 50}, {"%", 50},
        {"<<", 45}, {">>", 45},
    };

    std::string peek() const { return pos < tokens.size() ? tokens[pos] : std::string{}; }

    std::string eat(const std::string* expect = nullptr) {
        if (pos >= tokens.size()) throw ParseFail("eof");
        auto got = tokens[pos];
        if (expect && got != *expect) throw ParseFail("expected " + *expect + " got " + got);
        ++pos;
        return got;
    }
    std::string eat_s(std::string t) { return eat(&t); }

    bool is_digit_tok(const std::string& t) const {
        if (t.empty()) return false;
        for (char c : t) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    R nud() {
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
                // sizeof is a size_t (unsigned long, LP64)
                int n = sizeof_interval(inner);
                return R{n, n, true, 64};
            }
            auto name = eat();
            int n = sizeof_interval({name});
            return R{n, n, true, 64};
        }
        if (t == "(") {
            if (peek() == "{") throw ParseFail("statement-expr unencoded");
            if (kCastWords.contains(peek())) {
                std::string words;
                bool ptr = false;
                while (!peek().empty() && peek() != ")") {
                    if (!kCastWords.contains(peek()) && peek() != "*") break;
                    if (peek() == "*") ptr = true;
                    words += (words.empty() ? "" : " ") + eat();
                }
                eat_s(")");
                auto v = parse(90);
                auto ct = ptr ? std::nullopt : stages_detail::scalar_ctype(words);
                if (!ct || v.empty()) return v;
                // (T)v: the value converted to T (a narrowing cast of a value
                // that may not fit is the whole range of T).
                int cw = ct->w >= 64 ? 64 : kWidth;
                if (ct->w < 32 || (cw < v.w && (v.lo < tmin(cw) || v.hi > tmax(cw)))) return top(cw, ct->u);
                if (ct->u && !v.unsigned_ && v.lo < 0) return top(cw, true);
                return R{v.lo, v.hi, ct->u, cw};
            }
            auto v = parse(0);
            eat_s(")");
            return v;
        }
        if (t == "-") return uneg(e, parse(90));
        if (t == "+") return parse(90);
        if (t == "!") {
            auto v = parse(90);
            if (v.lo == 0 && v.hi == 0) return R{1, 1, false};
            if (!v.contains(0)) return R{0, 0, false};
            return R{0, 1, false};
        }
        if (t == "~") {
            auto v = parse(90);
            if (v.lo == v.hi) return R{~v.lo, ~v.lo, v.unsigned_, v.w};
            return top(v.w, v.unsigned_);
        }
        if (t == "++" || t == "--") {
            auto name = eat();
            auto cur = e.get(name);
            auto nxt = binop(e, cur, t == "++" ? "+" : "-", R{1, 1, false});
            e.set(name, nxt);
            return nxt;
        }
        if (is_digit_tok(t) || t.starts_with("0x")) {
            uint64_t v = 0;
            try {
                std::size_t used = 0;
                v = std::stoull(t, &used, 0);
                if (used != t.size()) throw ParseFail("nud " + t);
            } catch (const ParseFail&) {
                throw;
            } catch (...) {
                throw ParseFail("64-bit literal unencoded");
            }
            // Integer suffix (the tokenizer splits `16U` / `5ULL`): U makes the
            // literal unsigned, L / LL makes it 64-bit (LP64). Without a
            // suffix a decimal constant above INT_MAX is a long; a hex or
            // octal one up to UINT_MAX is an unsigned int.
            bool uns = false, wide = false;
            if (auto sfx = peek(); !sfx.empty() && sfx.find_first_not_of("uUlL") == std::string::npos) {
                wide = sfx.find_first_of("lL") != std::string::npos;
                uns = sfx.find_first_of("uU") != std::string::npos;
                eat();
            }
            const bool dec = !(t.size() > 1 && t[0] == '0');
            if (v > static_cast<uint64_t>(kIntMax) && !wide) {
                if (!dec && v <= 0xFFFFFFFFull) {
                    uns = true;
                } else {
                    wide = true;
                }
            }
            // A value above LLONG_MAX (an unsigned long long) is outside the
            // signed range this domain keeps for 64-bit values.
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) throw ParseFail("64-bit literal unencoded");
            int64_t n = static_cast<int64_t>(v);
            if (!wide && n > kIntMax) n -= (int64_t{1} << kWidth);  // an unsigned int, kept in the signed range
            return R{n, n, uns, wide ? 64 : kWidth};
        }
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'') {
            int n = char_lit_value(t);
            return R{n, n, false};
        }
        if (is_ident(t)) {
            if (t == "_Generic" || t == "offsetof") throw ParseFail(t + " unencoded");
            if (peek() == "(") {
                eat_s("(");
                if (!peek().empty() && peek() != ")") {
                    parse(2);
                    while (peek() == ",") {
                        eat_s(",");
                        parse(2);
                    }
                }
                eat_s(")");
                return kCallDummy;
            }
            if (peek() == "[") throw ParseFail("index");
            if (peek() == "++" || peek() == "--") {
                auto op = eat();
                auto cur = e.get(t);
                auto nxt = binop(e, cur, op == "++" ? "+" : "-", R{1, 1, false});
                e.set(t, nxt);
                return cur;
            }
            return e.get(t);
        }
        throw ParseFail("nud " + t);
    }

    R parse(int minp) {
        auto left = nud();
        while (prec.contains(peek()) && prec.at(peek()) >= minp) {
            auto op = eat();
            auto right = parse(prec.at(op) + 1);
            left = binop(e, left, op, right);
        }
        if (minp <= 5 && peek() == "?") {
            eat_s("?");
            auto then_v = parse(0);
            eat_s(":");
            auto else_v = parse(5);
            left = join_r(then_v, else_v);
        }
        if (minp <= 1 && peek() == ",") {
            eat_s(",");
            left = parse(0);
        }
        return left;
    }
};

R eval_expr(Engine& e, const std::string& src) {
    Parser p{e, interval_tok(src), 0};
    auto v = p.parse(0);
    if (p.pos != p.tokens.size()) {
        std::string trail;
        for (std::size_t i = p.pos; i < p.tokens.size(); ++i) {
            if (!trail.empty()) trail += " ";
            trail += p.tokens[i];
        }
        throw ParseFail("trailing " + trail);
    }
    return v;
}

void refine(Engine& e, const std::string& cond, bool truth) {
    auto tokens = tok(cond);
    int idx = -1;
    for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        for (auto* c : kCmp) {
            if (tokens[static_cast<std::size_t>(i)] == c) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) break;
    }
    if (idx < 0 || idx == 0 || !is_ident(tokens[0]) || idx != 1) return;
    std::string name = tokens[0];
    std::string op = tokens[1];
    std::string rhs_s;
    for (std::size_t i = 2; i < tokens.size(); ++i) {
        if (!rhs_s.empty()) rhs_s += " ";
        rhs_s += tokens[i];
    }
    R rhs;
    try {
        rhs = eval_expr(e, rhs_s);
    } catch (const ParseFail&) {
        return;
    } catch (const Alarm&) {
        return;
    }
    if (rhs.lo != rhs.hi) {
        if (op == "==" && truth) e.set(name, meet_r(e.get(name), rhs));
        return;
    }
    int64_t c = rhs.lo;
    auto cur = e.get(name);
    if (!truth) {
        if (op == "<") op = ">=";
        else if (op == ">") op = "<=";
        else if (op == "<=") op = ">";
        else if (op == ">=") op = "<";
        else if (op == "==") op = "!=";
        else if (op == "!=") op = "==";
    }
    const int64_t lo = tmin(cur.w), hi = tmax(cur.w);
    if ((op == "<" && c == lo) || (op == ">" && c == hi)) {
        e.set(name, kBot);
        return;
    }
    if (op == "<") e.set(name, meet_r(cur, R{lo, c - 1, false}));
    else if (op == "<=") e.set(name, meet_r(cur, R{lo, c, false}));
    else if (op == ">") e.set(name, meet_r(cur, R{c + 1, hi, false}));
    else if (op == ">=") e.set(name, meet_r(cur, R{c, hi, false}));
    else if (op == "==") e.set(name, meet_r(cur, R{c, c, false}));
    else if (op == "!=") {
        if (cur.lo == c && cur.hi > c) e.set(name, R{c + 1, cur.hi, cur.unsigned_});
        else if (cur.hi == c && cur.lo < c) e.set(name, R{cur.lo, c - 1, cur.unsigned_});
        else if (cur.lo == cur.hi && cur.hi == c) e.set(name, kBot);
    }
}

void decl(Engine& e, std::string stmt_s) {
    stmt_s = strip(stmt_s);
    while (!stmt_s.empty() && stmt_s.back() == ';') stmt_s.pop_back();
    stmt_s = strip(std::move(stmt_s));
    static Regex re(
        "((?:(?:unsigned|signed|long|short|int|char|uint32_t|int32_t|uint64_t|int64_t|size_t)\\s+)+)"
        "(?:const\\s+)?([A-Za-z_]\\w*)(?:\\s*=\\s*(.*))?$");
    auto m = match_at(re, stmt_s);
    auto ct = m ? stages_detail::scalar_ctype(m->group(1)) : std::nullopt;
    if (!m || !ct) {
        static Regex arr("\\[([^\\]]+)\\]");
        static Regex digits("\\d+");
        if (auto am = arr.search_match(stmt_s); am) {
            auto inner = strip(am->group(1));
            if (!fullmatch(digits, inner)) throw ParseFail("VLA unencoded");
        }
        if (stmt_s.find('[') != std::string::npos) throw ParseFail("array decl");
        // `unsigned long int un = ..., ur, i;`: a local of a type (or a
        // declarator list) the engine does not model. Untracked, its uses
        // would read a signed full range and alarm falsely (tinyexpr ncr).
        throw ParseFail("unmodelled declaration");
    }
    std::string name = m->group(2);
    std::string init = m->group(3);
    const bool has_init = m->spans.size() > 3 && m->spans[3].first >= 0;
    if (ct->u) e.unsigned_names.insert(name);
    if (ct->w >= 64) e.wide_names.insert(name);
    else e.wide_names.erase(name);
    // `int x =` with nothing after it reads as 0, like any empty expression.
    e.set(name, has_init ? eval_expr(e, init) : top(e.width_of(name)));
}

void assign(Engine& e, std::string stmt_s) {
    stmt_s = strip(stmt_s);
    while (!stmt_s.empty() && stmt_s.back() == ';') stmt_s.pop_back();
    stmt_s = strip(std::move(stmt_s));
    if (stmt_s.empty()) return;
    auto parts = split_comma(stmt_s);
    if (parts.size() > 1) {
        for (auto& part : parts) {
            auto piece = strip(part);
            if (!piece.empty()) assign(e, piece);
        }
        return;
    }
    static Regex post("([A-Za-z_]\\w*)\\s*(\\+\\+|--)$");
    if (auto m = match_at(post, stmt_s)) {
        auto name = m->group(1);
        auto op = m->group(2);
        auto cur = e.get(name);
        e.set(name, binop(e, cur, op == "++" ? "+" : "-", R{1, 1, false}));
        return;
    }
    static Regex pre("(\\+\\+|--)([A-Za-z_]\\w*)$");
    if (auto m = match_at(pre, stmt_s)) {
        auto op = m->group(1);
        auto name = m->group(2);
        auto cur = e.get(name);
        e.set(name, binop(e, cur, op == "++" ? "+" : "-", R{1, 1, false}));
        return;
    }
    static Regex asg("([A-Za-z_]\\w*)\\s*([+\\-*/%&|^]|<<|>>)?=\\s*(.*)$");
    auto m = match_at(asg, stmt_s);
    if (!m) {
        eval_expr(e, stmt_s);
        return;
    }
    auto name = m->group(1);
    auto op = m->group(2);
    auto rhs = m->group(3);
    auto rv = eval_expr(e, rhs);
    if (!op.empty()) e.set(name, binop(e, e.get(name), op, rv));
    else e.set(name, rv);
}

std::vector<std::string> split_for(const std::string& header) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char ch : header) {
        if (ch == '(') {
            ++depth;
            cur.push_back(ch);
        } else if (ch == ')') {
            depth = std::max(0, depth - 1);
            cur.push_back(ch);
        } else if (ch == ';' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::string if_stmt(Engine& e, const std::string& text, int bound) {
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
    Engine then_e = fork_engine(e);
    refine(then_e, cond, true);
    Engine else_e = fork_engine(e);
    refine(else_e, cond, false);
    bool then_dead = false, else_dead = false;
    // A constant condition (`sizeof(long) == 4`) leaves one branch dead.
    try {
        Engine probe = fork_engine(e);
        auto cv = eval_expr(probe, cond);
        if (!cv.empty() && cv.lo == 0 && cv.hi == 0) then_dead = true;
        else if (!cv.empty() && !cv.contains(0)) else_dead = true;
    } catch (const ParseFail&) {
    } catch (const Alarm&) {
    }
    if (!then_dead) {
        try {
            stmts(then_e, then_src, bound);
        } catch (const ReturnEx&) {
            then_dead = true;
        }
    }
    if (else_src && !else_dead) {
        try {
            stmts(else_e, *else_src, bound);
        } catch (const ReturnEx&) {
            else_dead = true;
        }
    }
    if (then_dead && else_dead) {
        e.live = false;
        return after;
    }
    if (then_dead) {
        e.st = else_e.st;
        e.unsigned_names = else_e.unsigned_names;
        e.wide_names = else_e.wide_names;
        return after;
    }
    if (else_dead) {
        e.st = then_e.st;
        e.unsigned_names = then_e.unsigned_names;
        e.wide_names = then_e.wide_names;
        return after;
    }
    e.st = join_state(then_e.st, else_e.st);
    return after;
}

std::string do_stmt(Engine& e, const std::string& text, int bound) {
    auto rest = lstrip(text.substr(2));
    auto [body, after] = take_block(rest);
    after = lstrip(after);
    if (!starts_kw(after, "while")) throw ParseFail("do without while");
    after = lstrip(after.substr(5));
    std::string cond;
    std::tie(cond, after) = paren(after);
    after = lstrip(after);
    if (after.starts_with(";")) after = after.substr(1);
    Engine body_e = fork_engine(e);
    try {
        stmts(body_e, body, bound);
    } catch (const ReturnEx&) {
        e.st = body_e.st;
        e.unsigned_names = body_e.unsigned_names;
        e.wide_names = body_e.wide_names;
        return after;
    }
    e.st = join_state(e.st, body_e.st);
    for (int i = 0; i < std::max(1, bound) - 1; ++i) {
        Engine be = fork_engine(e);
        refine(be, cond, true);
        try {
            stmts(be, body, bound);
        } catch (const ReturnEx&) {
            break;
        }
        e.st = join_state(e.st, be.st);
    }
    refine(e, cond, false);
    return after;
}

std::string while_stmt(Engine& e, const std::string& text, int bound) {
    auto rest = lstrip(text.substr(5));
    auto [cond, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    for (int i = 0; i < std::max(1, bound); ++i) {
        Engine body_e = fork_engine(e);
        refine(body_e, cond, true);
        try {
            stmts(body_e, body, bound);
        } catch (const ReturnEx&) {
            break;
        }
        e.st = join_state(e.st, body_e.st);
    }
    refine(e, cond, false);
    return after;
}

std::string for_stmt(Engine& e, const std::string& text, int bound) {
    auto rest = lstrip(text.substr(3));
    auto [header, after] = paren(rest);
    std::string body;
    std::tie(body, after) = take_block(after);
    auto raw_parts = split_for(header);
    std::vector<std::string> parts;
    for (auto& p : raw_parts) parts.push_back(strip(p));
    std::string init = parts.empty() ? "" : parts[0];
    std::string cond = parts.size() > 1 ? parts[1] : "";
    std::string step = parts.size() > 2 ? parts[2] : "";
    if (!init.empty()) {
        std::string init_stmt = init.ends_with(";") ? init : init + ";";
        if (auto miss = unencoded_layout_stmt(init_stmt)) throw ParseFail(*miss);
        if (looks_like_decl(init_stmt)) decl(e, init);
        else assign(e, init);
    }
    for (int i = 0; i < std::max(1, bound); ++i) {
        Engine body_e = fork_engine(e);
        if (!cond.empty()) refine(body_e, cond, true);
        try {
            stmts(body_e, body, bound);
            if (!step.empty()) assign(body_e, step);
        } catch (const ReturnEx&) {
            break;
        }
        e.st = join_state(e.st, body_e.st);
    }
    if (!cond.empty()) refine(e, cond, false);
    return after;
}

void stmts(Engine& e, std::string text, int bound) {
    text = strip(text);
    while (!text.empty() && e.live) {
        text = lstrip(text);
        if (text.empty()) break;
        if (text.starts_with("{")) {
            auto [inner, rest] = brace(text);
            stmts(e, inner, bound);
            text = rest;
            continue;
        }
        if (starts_kw(text, "if")) {
            text = if_stmt(e, text, bound);
            continue;
        }
        if (starts_kw(text, "while")) {
            text = while_stmt(e, text, bound);
            continue;
        }
        if (starts_kw(text, "for")) {
            text = for_stmt(e, text, bound);
            continue;
        }
        if (starts_kw(text, "do")) {
            text = do_stmt(e, text, bound);
            continue;
        }
        if (starts_kw(text, "switch")) throw ParseFail("switch");
        if (starts_kw(text, "return")) {
            auto [st, rest] = stmt(text);
            text = rest;
            auto expr = st.substr(6);
            while (!expr.empty() && expr.back() == ';') expr.pop_back();
            expr = strip(expr);
            if (!expr.empty()) eval_expr(e, expr);
            throw ReturnEx();
        }
        if (starts_kw(text, "break") || starts_kw(text, "continue")) {
            std::tie(std::ignore, text) = stmt(text);
            e.live = false;
            return;
        }
        if (starts_kw(text, "assert")) {
            auto [st, rest] = stmt(text);
            text = rest;
            auto lp = st.find('(');
            auto rp = st.rfind(')');
            if (lp != std::string::npos && rp != std::string::npos && rp > lp)
                eval_expr(e, st.substr(lp + 1, rp - lp - 1));
            continue;
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
        if (auto miss = unencoded_layout_prefix(text)) throw ParseFail(*miss);
        auto [st, rest] = stmt(text);
        text = rest;
        if (auto miss = unencoded_layout_stmt(st)) throw ParseFail(*miss);
        if (looks_like_decl(st)) decl(e, st);
        else assign(e, st);
    }
}

std::optional<std::string> unencoded_layout_prefix(std::string_view text) {
    auto s = lstrip(std::string(text));
    static Regex su(R"((?:struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{)");
    static Regex en(R"(enum\s*\{)");
    static Regex se(R"((?:static|extern)\b)");
    static Regex al(R"((?:_Alignas|alignas)\s*\()");
    static Regex at(R"(__auto_type\b)");
    static Regex tl(R"((?:_Thread_local|thread_local)\b)");
    static Regex cx(R"((?:_Complex|_Imaginary)\b)");
    static Regex df(R"((?:_Decimal32|_Decimal64|_Decimal128)\b)");
    static Regex ie(R"((?:_Float16|_Float32|_Float64|__fp16)\b)");
    static Regex tq(R"((?:typeof_unqual|__typeof_unqual__)\s*\()");
    static Regex ty(R"((?:typeof|__typeof__)\s*\()");
    static Regex ce(R"(constexpr\b)");
    static Regex as(R"(\[\[\s*assume\s*\()");
    static Regex cl(
        R"((?:int|unsigned(?:\s+int)?|long|short|char|uint32_t|int32_t|size_t)\s+\w+\s*=\s*\([^)]*\)\s*\{)");
    if (match_at(su, s)) return "struct unencoded";
    if (match_at(en, s)) return "anon enum unencoded";
    if (match_at(se, s)) return "storage-duration unencoded";
    if (match_at(al, s)) return "alignas unencoded";
    if (match_at(at, s)) return "storage-class unencoded";
    if (match_at(tl, s)) return "thread-local unencoded";
    if (match_at(cx, s)) return "complex unencoded";
    if (match_at(df, s)) return "decimal-float unencoded";
    if (match_at(ie, s)) return "extra-IEEE unencoded";
    if (match_at(tq, s)) return "typeof_unqual unencoded";
    if (match_at(ty, s)) return "typeof unencoded";
    if (match_at(ce, s)) return "constexpr unencoded";
    if (match_at(as, s)) return "assume unencoded";
    if (match_at(cl, s)) return "compound-lit unencoded";
    return std::nullopt;
}

std::optional<std::string> unencoded_layout_stmt(std::string_view stmt_s) {
    auto s = strip(std::string(stmt_s));
    if (s.empty()) return std::nullopt;
    if (re_search(R"(\b(?:__int128(?:_t)?|_BitInt)\b)", s)) return "128-bit unencoded";
    if (re_search(R"(\b(?:_Decimal32|_Decimal64|_Decimal128)\b)", s)) return "decimal-float unencoded";
    if (re_search(R"(\b(?:_Float16|_Float32|_Float64|__fp16)\b)", s)) return "extra-IEEE unencoded";
    if (starts_kw(s, "constexpr")) return "constexpr unencoded";
    if (re_search(R"(\[\[\s*assume\s*\()", s)) return "assume unencoded";
    if (re_search(R"(__attribute__\s*\(\s*\(\s*cleanup)", s)) return "cleanup unencoded";
    if (re_search(R"(__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b)", s))
        return "vector_size unencoded";
    if (starts_kw(s, "const")) return "const unencoded";
    if (starts_kw(s, "register") || starts_kw(s, "auto")) return "storage-class unencoded";
    static Regex at(R"(__auto_type\b)");
    if (match_at(at, s)) return "storage-class unencoded";
    if (starts_kw(s, "static") || starts_kw(s, "extern")) return "storage-duration unencoded";
    static Regex tl(R"((?:_Thread_local|thread_local)\b)");
    if (match_at(tl, s)) return "thread-local unencoded";
    static Regex cx(R"((?:_Complex|_Imaginary)\b)");
    if (match_at(cx, s)) return "complex unencoded";
    static Regex tq(R"((?:typeof_unqual|__typeof_unqual__)\s*\()");
    if (match_at(tq, s)) return "typeof_unqual unencoded";
    static Regex ty(R"((?:typeof|__typeof__)\s*\()");
    if (match_at(ty, s)) return "typeof unencoded";
    if (is_nested_function(s)) return "nested function unencoded";
    static Regex al(R"((?:_Alignas|alignas)\s*\()");
    if (match_at(al, s)) return "alignas unencoded";
    if (re_search(R"(=\s*\([^)]*\)\s*\{)", s)) return "compound-lit unencoded";
    if (starts_kw(s, "struct") || starts_kw(s, "union")) {
        static Regex anon(R"((?:struct|union)\s*\{)");
        static Regex named(R"((?:struct|union)\s+[A-Za-z_]\w*\s*\{)");
        static Regex var(R"((?:struct|union)\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*)");
        if (match_at(anon, s) || match_at(named, s) || match_at(var, s)) return "struct unencoded";
        return std::nullopt;
    }
    if (starts_kw(s, "enum")) {
        static Regex anon(R"(enum\s*\{)");
        static Regex var(R"(enum\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*)");
        if (match_at(anon, s)) return "anon enum unencoded";
        if (match_at(var, s)) return "struct unencoded";
        return std::nullopt;
    }
    // `T name`, and `T *name` / `T * const name` (a pointer to a typedef type).
    static Regex td(R"(([A-Za-z_]\w*)(?:\s+|\s*\*+\s*(?:const\s+)?)[A-Za-z_]\w*\s*(?:[=;\[]|$))");
    auto m = match_at(td, s);
    if (!m) return std::nullopt;
    if (kStmtStartWords.contains(m->group(1))) return std::nullopt;
    return "typedef local unencoded";
}


std::optional<Finding> interval_function(const FunctionInfo& fn) {
    if (fn.kind == "POINTER" || fn.kind == "OTHER") return std::nullopt;
    if (body_needs_pointer_harness(fn.body)) return std::nullopt;
    if (unencoded_syntax_reason_cached(fn, "interval")) return std::nullopt;
    Engine eng(fn.params);
    for (auto& n : float_locals(fn.body)) eng.float_names.insert(n);
    try {
        stmts(eng, fn.body);
    } catch (const Alarm& a) {
        Finding f;
        f.stage = "interval";
        f.file = fn.file;
        f.function = fn.name;
        f.line = fn.line;
        f.strength = std::string(laws::STRENGTH_FINDS);
        f.extra["oracle"] = "interval";
        f.status = std::string(laws::FAILED);
        f.cls = a.cls;
        f.message = "interval: " + a.msg;
        return f;
    } catch (const ParseFail&) {
        return std::nullopt;
    } catch (const ReturnEx&) {
        return std::nullopt;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::vector<Finding> run_interval(const std::vector<FunctionInfo>& functions) {
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (auto rec = interval_function(fn)) out.push_back(std::move(*rec));
    }
    return out;
}

}  // namespace prism
