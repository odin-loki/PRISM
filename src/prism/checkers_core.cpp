#include "prism/checkers.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prism {
namespace {

const std::unordered_set<std::string> kSignedWords = {
    "int", "short", "long", "char", "signed", "ssize_t", "ptrdiff_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "intmax_t", "intptr_t",
    "off_t", "pid_t",
};
const std::unordered_set<std::string> kKw = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default",
};

const std::unordered_map<std::string, int> kFmtFn = {
    {"printf", 0}, {"fprintf", 1}, {"sprintf", 1}, {"snprintf", 2},
    {"warn", 0}, {"err", 0}, {"syslog", 0},
};
const std::unordered_map<std::string, int> kUnboundedCopyFn = {
    {"strcpy", 0}, {"gets", 0}, {"strcat", 0},
};
const std::unordered_map<std::string, int> kSprintfFn = {
    {"sprintf", 0}, {"vsprintf", 0},
};
const std::unordered_map<std::string, int> kWcsUnboundedFn = {
    {"wcscpy", 0}, {"wcscat", 0},
};
const std::unordered_map<std::string, std::vector<int>> kGetenvUseCallees = {
    {"strlen", {0}}, {"strcpy", {1}}, {"strcat", {1}}, {"strcmp", {0, 1}},
    {"atoi", {0}}, {"atol", {0}}, {"atoll", {0}},
};
const std::unordered_map<std::string, std::vector<int>> kStrNullFn = {
    {"strlen", {0}}, {"strcpy", {1}}, {"strcmp", {0, 1}},
};

const char kUnaryBitopPrev[] = "=(,?:;{[|&!~^+-*/%<>";

std::string strip(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::string squeeze_ws(std::string_view s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
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

std::string strip_ws(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    return out;
}

std::string to_lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string re_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '_') out.push_back(static_cast<char>(c));
        else {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view body) {
    std::vector<std::string> out;
    if (body.empty()) return out;
    std::size_t i = 0;
    while (true) {
        auto n = body.find('\n', i);
        if (n == std::string_view::npos) {
            out.emplace_back(body.substr(i));
            break;
        }
        out.emplace_back(body.substr(i, n - i));
        i = n + 1;
        if (i == body.size()) break;
    }
    return out;
}

std::string join_range(const std::vector<std::string>& v, std::size_t a, std::size_t b) {
    std::string s;
    if (b > v.size()) b = v.size();
    for (std::size_t i = a; i < b; ++i) {
        if (i > a) s.push_back('\n');
        s += v[i];
    }
    return s;
}

std::string join_all(const std::vector<std::string>& v) {
    return join_range(v, 0, v.size());
}

std::vector<std::string> chunk_of(const std::vector<std::string>& lines, const FunctionInfo& fn) {
    int start = fn.span.first, end = fn.span.second;
    if (start < 1) return {};
    auto a = static_cast<std::size_t>(start - 1);
    auto b = static_cast<std::size_t>(std::max(end, 0));
    if (b > lines.size()) b = lines.size();
    if (a >= lines.size() || a >= b) return {};
    return {lines.begin() + static_cast<std::ptrdiff_t>(a),
            lines.begin() + static_cast<std::ptrdiff_t>(b)};
}

std::optional<std::string> func_of_line(const std::vector<FunctionInfo>& funcs, int ln) {
    for (auto& f : funcs)
        if (f.span.first <= ln && ln <= f.span.second) return f.name;
    return std::nullopt;
}

bool is_ident(std::string_view s) {
    static Regex re(R"(^[A-Za-z_]\w*$)");
    auto m = re.search_match(s);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           m->spans[0].second == static_cast<int>(s.size());
}

bool fullmatch(const Regex& re, std::string_view s) {
    auto m = re.search_match(s);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           m->spans[0].second == static_cast<int>(s.size());
}

std::vector<std::string> findall1(const Regex& re, std::string_view s) {
    std::vector<std::string> out;
    for (auto& m : re.finditer(s)) out.push_back(m.group(1));
    return out;
}

std::string named_or(const Match& m, std::string_view a, std::string_view b) {
    auto x = m.named(a);
    return !x.empty() ? x : m.named(b);
}

int count_nl(std::string_view s) {
    return static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

int max_before(const std::vector<int>& hits, int r) {
    int last = -1;
    for (int h : hits)
        if (h < r && h > last) last = h;
    return last;
}

std::vector<std::string> split_call_args(std::string_view inner) {
    std::vector<std::string> args;
    int depth = 0;
    std::string cur;
    bool in_str = false, esc = false;
    for (char ch : inner) {
        if (in_str) {
            cur.push_back(ch);
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            cur.push_back(ch);
        } else if (ch == '(') {
            ++depth;
            cur.push_back(ch);
        } else if (ch == ')') {
            --depth;
            cur.push_back(ch);
        } else if (ch == ',' && depth == 0) {
            args.push_back(strip(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    auto tail = strip(cur);
    if (!tail.empty()) args.push_back(std::move(tail));
    return args;
}

std::optional<std::vector<std::string>> find_call_args(std::string_view line, std::string_view fn) {
    Regex re(std::string("\\b") + std::string(fn) + "\\s*\\(");
    auto m = re.search_match(line);
    if (!m) return std::nullopt;
    auto start = static_cast<std::size_t>(m->spans[0].second);
    int depth = 1;
    std::size_t i = start;
    while (i < line.size() && depth > 0) {
        if (line[i] == '(') ++depth;
        else if (line[i] == ')') --depth;
        ++i;
    }
    if (depth != 0) return std::nullopt;
    return split_call_args(line.substr(start, i - 1 - start));
}

bool is_string_literal(std::string_view s) {
    static Regex re(R"(^L?")");
    return re.match_line(strip(s));
}

std::optional<std::string> string_literal_inner(std::string s) {
    s = strip(s);
    if (s.size() >= 3 && s.starts_with("L\"") && s.ends_with('"'))
        return s.substr(2, s.size() - 3);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return std::nullopt;
}

bool literal_has_percent_n(std::string_view s) {
    auto inner_o = string_literal_inner(std::string(s));
    if (!inner_o) return false;
    const auto& inner = *inner_o;
    std::size_t i = 0;
    while (i < inner.size()) {
        if (inner[i] == '\\') {
            i += 2;
            continue;
        }
        if (inner[i] != '%') {
            ++i;
            continue;
        }
        if (i + 1 < inner.size() && inner[i + 1] == '%') {
            i += 2;
            continue;
        }
        auto j = i + 1;
        while (j < inner.size() && std::string_view("-+ #0").find(inner[j]) != std::string_view::npos)
            ++j;
        if (j < inner.size() && inner[j] == '*') ++j;
        else
            while (j < inner.size() && std::isdigit(static_cast<unsigned char>(inner[j]))) ++j;
        if (j < inner.size() && inner[j] == '.') {
            ++j;
            if (j < inner.size() && inner[j] == '*') ++j;
            else
                while (j < inner.size() && std::isdigit(static_cast<unsigned char>(inner[j]))) ++j;
        }
        while (j < inner.size() && std::string_view("hlLjztz").find(inner[j]) != std::string_view::npos)
            ++j;
        if (j < inner.size() && inner[j] == 'n') return true;
        i = j < inner.size() ? j + 1 : j;
    }
    return false;
}

bool fmt_literal_has_percent_s(std::string_view s) {
    auto inner_o = string_literal_inner(std::string(s));
    if (!inner_o) return false;
    const auto& inner = *inner_o;
    std::size_t i = 0;
    while (i < inner.size()) {
        if (inner[i] == '%') {
            if (i + 1 < inner.size() && inner[i + 1] == '%') {
                i += 2;
                continue;
            }
            if (i + 1 < inner.size() && inner[i + 1] == 's') return true;
        }
        ++i;
    }
    return false;
}

std::optional<int> parse_int_literal(std::string_view raw) {
    static Regex re(R"(^(?P<n>-?(?:0x[0-9a-fA-F]+|0[0-7]*|\d+))(?:[uUlL]{0,3})?$)");
    auto s = strip(raw);
    auto m = re.search_match(s);
    if (!m || m->spans.empty() || m->spans[0].first != 0 ||
        m->spans[0].second != static_cast<int>(s.size()))
        return std::nullopt;
    auto n = m->named("n");
    try {
        int base = 10;
        if (n.size() >= 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) base = 16;
        else if (n.size() > 1 && n[0] == '0' && std::isdigit(static_cast<unsigned char>(n[1])))
            base = 8;
        std::size_t idx = 0;
        long v = std::stol(n, &idx, base);
        if (idx != n.size()) return std::nullopt;
        return static_cast<int>(v);
    } catch (...) {
        return std::nullopt;
    }
}

bool is_char_literal(std::string_view s) {
    static Regex re(R"(^'(?:\\.|[^\\'])'$)");
    auto t = strip(s);
    return fullmatch(re, t);
}

bool is_zero_literal(std::string_view s) {
    static Regex re(R"((?i)^0(?:u|l|ll|ul|ull)?$)");
    return fullmatch(re, strip(s));
}

bool is_tiny_literal(std::string_view s) {
    static Regex re(R"((?i)^(?:0|1)(?:u|l|ll|ul|ull)?$)");
    return fullmatch(re, strip(s));
}

bool is_sizeof_expr(std::string_view s) {
    static Regex re(R"(^sizeof\s*\()");
    return re.match_line(strip(s));
}

bool has_wrap_mul(std::string_view s) {
    static Regex a(R"((?:\([^)]*\)\s*)*[A-Za-z_]\w*\s*\*\s*sizeof\s*\()");
    static Regex b(R"(sizeof\s*\([^)]+\)\s*\*\s*(?:\([^)]*\)\s*)*[A-Za-z_]\w*)");
    auto t = strip(s);
    return a.search(t) || b.search(t);
}

bool has_assignment(std::string_view var, std::string_view line) {
    Regex re("\\b" + re_escape(var) + "\\s*=(?!=)");
    return re.search(line);
}

bool reassigns_var(std::string_view var, std::string_view line) {
    Regex re("\\b" + re_escape(var) + "\\s*=(?!=)\\s*(.+?)\\s*(?:;|,|$)");
    auto m = re.search_match(line);
    if (!m) return false;
    auto rhs = strip(m->group(1));
    static Regex nil(R"(^(?:NULL|nullptr|0)\b)");
    if (nil.match_line(rhs)) return false;
    return true;
}

bool reads_var(std::string_view var, std::string_view line) {
    if (has_assignment(var, line)) return false;
    auto v = re_escape(var);
    if (Regex("\\b" + v + "\\s*->").search(line)) return true;
    if (Regex("\\*\\s*" + v + "\\b").search(line)) return true;
    if (Regex("\\b" + v + "\\s*\\[").search(line)) return true;
    return Regex("\\b" + v + "\\b").search(line);
}

std::string cap_norm(std::string_view s) { return strip_ws(s); }

bool trunc_rhs_bad(std::string rhs, std::string_view narrow) {
    rhs = strip(rhs);
    if (is_char_literal(rhs)) return false;
    if (is_ident(rhs)) return !kKw.contains(std::string(rhs));
    auto val = parse_int_literal(rhs);
    if (!val) return false;
    if (narrow == "char") return !(-128 <= *val && *val <= 127);
    return !(-32768 <= *val && *val <= 32767);
}

const Regex& unsigned_ty() {
    static Regex re(R"(unsigned|size_t|uint|u_int|u_char|u_short|u_long)");
    return re;
}

std::optional<std::string> narrow_kind(std::string_view typ) {
    if (unsigned_ty().search(typ)) return std::nullopt;
    static Regex ch(R"(\bchar\b)");
    static Regex sh(R"(\bshort\b)");
    if (ch.search(typ)) return std::string("char");
    if (sh.search(typ)) return std::string("short");
    return std::nullopt;
}

std::map<std::string, std::string> narrow_names(const FunctionInfo& fn) {
    static Regex decl(R"(\b(?:signed\s+)?(?:char|short)\s+([A-Za-z_]\w*)\s*;)");
    static Regex init(
        R"(\b(?:signed\s+)?(?:char|short)\s+([A-Za-z_]\w*)\s*=\s*(?P<rhs>[^;,]+))");
    static Regex is_char(R"(\bchar\b)");
    std::map<std::string, std::string> names;
    for (auto& [typ, name] : fn.params) {
        if (name.empty() || typ.find('*') != std::string::npos || typ.find('[') != std::string::npos)
            continue;
        if (auto kind = narrow_kind(typ)) names[name] = *kind;
    }
    for (auto& m : decl.finditer(fn.body))
        names[m.group(1)] = is_char.search(m.text) ? "char" : "short";
    for (auto& m : init.finditer(fn.body))
        names[m.group(1)] = is_char.search(m.text) ? "char" : "short";
    return names;
}

std::unordered_set<std::string> unsigned_names(const FunctionInfo& fn) {
    static Regex loc(
        R"(\b(?:unsigned\s+(?:int|short|long|char)|size_t|uint\d+_t)\s+([A-Za-z_]\w*)\s*;)");
    std::unordered_set<std::string> names;
    for (auto& [typ, name] : fn.params) {
        if (name.empty() || typ.find('*') != std::string::npos || typ.find('[') != std::string::npos)
            continue;
        if (unsigned_ty().search(typ)) names.insert(name);
    }
    for (auto& m : loc.finditer(fn.body)) names.insert(m.group(1));
    return names;
}

std::unordered_set<std::string> signed_index_names(const FunctionInfo& fn) {
    static Regex words(R"([A-Za-z_]\w*)");
    static Regex loc(
        R"(\b(?:signed\s+)?(?:int|short|long|char|ssize_t|ptrdiff_t|int\d+_t)"
        R"(|intmax_t|intptr_t|off_t|pid_t)\s+([A-Za-z_]\w*)\s*;)");
    std::unordered_set<std::string> names;
    for (auto& [typ, name] : fn.params) {
        if (name.empty() || typ.find('*') != std::string::npos || typ.find('[') != std::string::npos)
            continue;
        if (unsigned_ty().search(typ)) continue;
        for (auto& m : words.finditer(typ))
            if (kSignedWords.contains(m.text)) names.insert(name);
    }
    for (auto& m : loc.finditer(fn.body)) names.insert(m.group(1));
    return names;
}

std::pair<std::unordered_set<std::string>, std::unordered_set<std::string>> locals_in_fn(
    const FunctionInfo& fn) {
    static Regex decl(
        R"re((?m)^\s*(?P<static>static\s+)?(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t|wchar_t)\s+(?P<name>[A-Za-z_]\w*)(?P<array>\s*\[[^\]]+\])?\s*;)re",
        true);
    std::unordered_set<std::string> params, scalars, arrays;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : decl.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (params.contains(name) || kKw.contains(name)) continue;
        if (!m.named("array").empty()) arrays.insert(name);
        else scalars.insert(name);
    }
    return {scalars, arrays};
}

std::unordered_set<std::string> local_char_arrays(const FunctionInfo& fn) {
    static Regex re(
        R"re((?m)^\s*(?P<static>static\s+)?(?:const\s+)?char\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;)re",
        true);
    std::unordered_set<std::string> params, names;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : re.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (!params.contains(name) && !kKw.contains(name)) names.insert(name);
    }
    return names;
}

std::unordered_set<std::string> local_wchar_arrays(const FunctionInfo& fn) {
    static Regex re(
        R"re((?m)^\s*(?P<static>static\s+)?(?:const\s+)?wchar_t\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;)re",
        true);
    std::unordered_set<std::string> params, names;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : re.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (!params.contains(name) && !kKw.contains(name)) names.insert(name);
    }
    return names;
}

std::map<std::string, int> local_char_array_sizes(const FunctionInfo& fn) {
    static Regex re(
        R"re((?m)^\s*(?P<static>static\s+)?(?:const\s+)?char\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;)re",
        true);
    std::unordered_set<std::string> params;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    std::map<std::string, int> sizes;
    for (auto& m : re.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (!params.contains(name) && !kKw.contains(name))
            sizes[name] = std::stoi(m.named("size"));
    }
    return sizes;
}

bool sizeof_dst(std::string_view dst, std::string_view buf) {
    auto d = strip_ws(dst), b = strip_ws(buf);
    return d == "sizeof(" + b + ")" || d == "sizeof" + b;
}

std::optional<int> string_lit_len(std::string s) {
    static Regex re(R"(^L?")");
    s = strip(s);
    if (!re.match_line(s)) return std::nullopt;
    auto inner = s.substr(1, s.size() >= 2 ? s.size() - 2 : 0);
    int length = 0;
    std::size_t i = 0;
    while (i < inner.size()) {
        if (inner[i] == '\\') {
            i += i + 1 < inner.size() ? 2 : 1;
            ++length;
            continue;
        }
        ++length;
        ++i;
    }
    return length;
}

bool has_floor(std::string_view name, std::string_view text) {
    auto v = re_escape(name);
    Regex re("\\b" + v +
             "\\s*(?:<\\s*0|>=\\s*0|>\\s*-\\s*1|==\\s*-\\s*1|!=\\s*-\\s*1|<=\\s*-\\s*1)"
             "|0\\s*(?:>|<=|<|>=)\\s*\\b" +
             v + "\\b" + "|-\\s*1\\s*(?:<|>=|==|!=)\\s*\\b" + v + "\\b" +
             "|\\(\\s*(?:unsigned|u_int|u_long|size_t|uint\\d+_t)\\s*\\)\\s*\\(?\\s*\\b" + v +
             "\\b");
    return re.search(text);
}

bool pinned_or_loop(std::string_view name, std::string_view text) {
    auto v = re_escape(name);
    Regex re("\\b" + v + "\\s*=\\s*(?:0|[1-9]\\d*)\\s*[;,)]" +
             "|\\bfor\\s*\\(\\s*(?:[A-Za-z_]\\w*\\s+)?" + v + "\\s*=");
    return re.search(text);
}

bool nonzero_guard_for(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    if (Regex("\\bif\\s*\\(\\s*!\\s*" + v + "\\s*\\)").search(text)) return true;
    if (Regex("\\bif\\s*\\(\\s*" + v + "\\s*==\\s*0\\b").search(text)) return true;
    if (Regex("\\bif\\s*\\(\\s*0\\s*==\\s*" + v + "\\b").search(text)) return true;
    if (Regex("\\bif\\s*\\(\\s*" + v + "\\s*!=\\s*0\\b").search(text)) return true;
    if (Regex("\\bif\\s*\\(\\s*" + v + "\\s*\\)").search(text)) return true;
    return false;
}

bool null_test_for(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    Regex a("\\bif\\s*\\(\\s*(?:" + v + "\\s*==\\s*(?:NULL|0|nullptr)|!\\s*" + v + "\\b|" + v +
            "\\s*\\))");
    if (a.search(text)) return true;
    Regex b("\\(\\s*" + v + "\\s*=(?!=)[^;)]*?\\)\\s*==\\s*(?:NULL|nullptr|0)\\b");
    return b.search(text);
}

bool alloc_in_null_condition(std::string_view line, std::string_view var) {
    static Regex iff(R"(\bif\s*\()");
    static Regex nul(R"(==\s*(?:NULL|nullptr|0)\b)");
    Regex as("\\b" + re_escape(var) + "\\s*=(?!=)");
    return iff.search(line) && as.search(line) && nul.search(line);
}

std::optional<int> first_ptr_use(std::string_view var, const std::vector<std::string>& chunk,
                                 int start) {
    static Regex sz(R"(sizeof\s*\([^)]*\))");
    auto v = re_escape(var);
    Regex arrow("\\b" + v + "\\s*->");
    Regex star("\\*\\s*" + v + "\\b");
    Regex idx("\\b" + v + "\\s*\\[");
    for (int j = start + 1; j < static_cast<int>(chunk.size()); ++j) {
        std::string stripped;
        std::size_t off = 0;
        for (auto& m : sz.finditer(chunk[static_cast<std::size_t>(j)])) {
            auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
            stripped.append(chunk[static_cast<std::size_t>(j)], off, a - off);
            off = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        }
        stripped.append(chunk[static_cast<std::size_t>(j)], off, std::string::npos);
        if (arrow.search(stripped) || star.search(stripped) || idx.search(stripped)) return j;
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, int>> unchecked_alloc_site(const std::vector<std::string>& chunk,
                                                                int i, std::string var,
                                                                std::string_view fn,
                                                                bool require_nowait) {
    var = strip_ws(var);
    auto& line = chunk[static_cast<std::size_t>(i)];
    if (fn == "realloc") {
        Regex argm("realloc\\s*\\(\\s*(?P<p>" + re_escape(var) + ")\\s*,");
        if (argm.search(line)) return std::nullopt;
    }
    if (alloc_in_null_condition(line, var)) return std::nullopt;
    auto use_j = first_ptr_use(var, chunk, i);
    if (!use_j) return std::nullopt;
    auto between = join_range(chunk, static_cast<std::size_t>(i),
                              static_cast<std::size_t>(*use_j + 1));
    if (null_test_for(var, between)) return std::nullopt;
    std::string stmt;
    for (int x = i; x < i + 4 && x < static_cast<int>(chunk.size()); ++x) {
        if (!stmt.empty()) stmt.push_back(' ');
        stmt += strip(chunk[static_cast<std::size_t>(x)]);
    }
    if (require_nowait && stmt.find("M_NOWAIT") == std::string::npos) return std::nullopt;
    return std::pair{var, *use_j};
}

std::string unwrap_wrapping_braces(std::string s) {
    s = strip(s);
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
        auto inner = strip(s.substr(1, s.size() - 2));
        if (!inner.empty()) return inner;
    }
    return s;
}

std::string missing_return_last_chunk(std::string_view stmt) {
    std::string last;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= stmt.size(); ++i) {
        if (i == stmt.size() || stmt[i] == ';') {
            auto part = strip(stmt.substr(start, i - start));
            if (!part.empty()) last = std::move(part);
            start = i + 1;
        }
    }
    return last;
}

std::optional<std::string> last_body_stmt(const std::vector<std::string>& body_lines) {
    for (auto it = body_lines.rbegin(); it != body_lines.rend(); ++it) {
        auto s = strip(*it);
        if (s.empty() || s == "{" || s == "}") continue;
        s = unwrap_wrapping_braces(s);
        auto last = missing_return_last_chunk(s);
        return last.empty() ? s : last;
    }
    return std::nullopt;
}

bool is_value_returning(std::string_view ret) {
    static Regex value_ret(R"(\b(?:unsigned\s+)?(?:int|long|short|char)\b)");
    static Regex voidb(R"(\bvoid\b)");
    auto r = strip(ret);
    if (voidb.search(r) || r.find('*') != std::string::npos) return false;
    return value_ret.search(r);
}

bool missing_return_tail_ok(std::string_view stmt) {
    static Regex ok(R"(^\s*return\b)");
    static Regex tail(
        R"(^\s*(?:\(\s*void\s*\)\s*)?(?:abort|exit|_exit|__builtin_unreachable)\s*\()");
    static Regex fn(R"(\b(?:panic|fatal|errx)\w*\s*\()");
    auto s = unwrap_wrapping_braces(std::string(stmt));
    auto last = missing_return_last_chunk(s);
    if (last.empty()) last = s;
    if (ok.match_line(last)) return true;
    if (tail.match_line(last) || tail.match_line(s)) return true;
    return fn.search(last) || fn.search(s);
}

std::vector<std::pair<std::string, int>> switch_bodies(std::string_view text) {
    static Regex sw(R"(\bswitch\s*\([^)]*\))");
    std::vector<std::pair<std::string, int>> out;
    for (auto& m : sw.finditer(text)) {
        auto rest = text.substr(static_cast<std::size_t>(m.spans[0].second));
        auto brace = rest.find('{');
        if (brace == std::string_view::npos) continue;
        int depth = 0;
        std::optional<std::size_t> end;
        for (std::size_t k = 0; k < rest.size() - brace; ++k) {
            auto ch = rest[brace + k];
            if (ch == '{') ++depth;
            else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    end = brace + k;
                    break;
                }
            }
        }
        if (!end) continue;
        auto body = std::string(rest.substr(brace + 1, *end - (brace + 1)));
        int line_off = count_nl(text.substr(0, static_cast<std::size_t>(m.spans[0].second) + brace));
        out.emplace_back(std::move(body), line_off);
    }
    return out;
}

bool arm_falls_through(std::string_view arm) {
    static Regex annot(
        R"(\b(?:fallthrough|FALLTHROUGH|\[\[fallthrough\]\]|__attribute__\s*\(\s*\(\s*fallthrough\s*\)\s*\)))");
    static Regex stop(R"(\b(?:break|return|goto|continue)\b)");
    static Regex label(R"(\b(?:case\b[^:]*:|default\s*:))");
    if (annot.search(arm)) return false;
    bool has_stmt = false;
    for (auto& ln : split_lines(arm)) {
        auto s = strip(ln);
        if (s.empty()) continue;
        if (label.search(s)) continue;
        has_stmt = true;
        if (stop.search(s)) return false;
    }
    return has_stmt;
}

std::unordered_set<std::string> uninit_locals(const FunctionInfo& fn) {
    static Regex re(
        R"re((?m)^\s*(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s+([A-Za-z_]\w*)\s*;)re",
        true);
    std::unordered_set<std::string> params, names;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : re.finditer(fn.body)) {
        auto name = m.group(1);
        if (!params.contains(name) && !kKw.contains(name)) names.insert(name);
    }
    return names;
}

std::unordered_set<std::string> local_uninit_pointers(const FunctionInfo& fn) {
    static Regex re(
        R"re((?m)^\s*(?:static\s+)?(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|void|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s*\*\s*([A-Za-z_]\w*)\s*;)re",
        true);
    std::unordered_set<std::string> params, names;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : re.finditer(fn.body)) {
        auto name = m.group(1);
        if (!params.contains(name) && !kKw.contains(name)) names.insert(name);
    }
    return names;
}

bool ptr_decl_line(std::string_view p, std::string_view ln) {
    static Regex re(
        R"re(^\s*(?:static\s+)?(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|void|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s*\*\s*([A-Za-z_]\w*)\s*;)re");
    auto m = re.search_match(ln);
    return m && re.match_line(ln) && m->group(1) == p;
}

bool declared_noreturn(const FunctionInfo& fn, const std::vector<std::string>& lines) {
    static Regex attr(
        R"(__dead2|__dead\b|_Noreturn|\bnoreturn\b|__attribute__\s*\(\s*\(\s*noreturn\s*\)\s*\))");
    if (attr.search(fn.signature)) return true;
    int start = std::max(0, fn.span.first - 3);
    auto head = join_range(lines, static_cast<std::size_t>(start),
                           static_cast<std::size_t>(fn.span.first));
    return attr.search(head);
}

bool has_comparison(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        if (s.substr(i).starts_with("==") || s.substr(i).starts_with("!=")) return true;
        if (s.substr(i).starts_with("<=") || s.substr(i).starts_with(">=")) return true;
        if (s.substr(i).starts_with("<<") || s.substr(i).starts_with(">>")) {
            i += 2;
            continue;
        }
        if (s[i] == '<' || s[i] == '>') return true;
        ++i;
    }
    return false;
}

std::vector<int> binary_bitop_indices(std::string_view s) {
    std::vector<int> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch != '&' && ch != '|') continue;
        char nxt = i + 1 < s.size() ? s[i + 1] : '\0';
        if (nxt == ch || nxt == '=') {
            ++i;
            continue;
        }
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && (s[static_cast<std::size_t>(j)] == ' ' ||
                          s[static_cast<std::size_t>(j)] == '\t'))
            --j;
        if (j < 0 || std::string_view(kUnaryBitopPrev).find(s[static_cast<std::size_t>(j)]) !=
                         std::string_view::npos)
            continue;
        out.push_back(static_cast<int>(i));
    }
    return out;
}

std::optional<std::string> unlock_of(std::string_view name) {
    static Regex lock(R"((?i)lock)");
    auto hits = lock.finditer(name);
    if (hits.empty()) return std::nullopt;
    auto& m = hits.back();
    auto up = std::string(name);
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (up.find("ASSERT") != std::string::npos) return std::nullopt;
    int st = m.spans[0].first;
    int from = std::max(0, st - 2);
    auto pre = to_lower_copy(std::string(name.substr(static_cast<std::size_t>(from),
                                                     static_cast<std::size_t>(st - from))));
    if (pre == "un") return std::nullopt;
    auto got = m.text;
    std::string rep = std::all_of(got.begin(), got.end(), [](unsigned char c) {
        return std::isupper(c);
    })
                          ? "UNLOCK"
                          : "unlock";
    return std::string(name.substr(0, static_cast<std::size_t>(m.spans[0].first))) + rep +
           std::string(name.substr(static_cast<std::size_t>(m.spans[0].second)));
}

std::optional<std::string> lock_of(std::string_view name) {
    static Regex un(R"((?i)unlock)");
    auto hits = un.finditer(name);
    if (hits.empty()) return std::nullopt;
    auto up = std::string(name);
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (up.find("ASSERT") != std::string::npos) return std::nullopt;
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

std::optional<std::pair<std::string, std::string>> first_two_locks(std::string_view body) {
    static Regex call(R"(\b(\w*lock)\s*\(\s*&?([A-Za-z_]\w*))");
    std::unordered_set<std::string> seen;
    std::vector<std::string> seq;
    for (auto& ln : split_lines(body)) {
        for (auto& m : call.finditer(ln)) {
            if (to_lower_copy(m.group(1)).find("unlock") != std::string::npos) continue;
            auto name = m.group(2);
            if (seen.contains(name)) continue;
            seen.insert(name);
            seq.push_back(name);
            if (seq.size() >= 2) return std::pair{seq[0], seq[1]};
        }
    }
    return std::nullopt;
}

std::unordered_set<std::string> file_scope_int_globals(const std::vector<std::string>& lines) {
    static Regex ig(R"(^int\s+(?P<name>[A-Za-z_]\w*)\s*;)");
    static Regex ug(R"(^unsigned\s+(?P<name>[A-Za-z_]\w*)\s*;)");
    int depth = 0;
    std::unordered_set<std::string> g;
    for (auto& line : lines) {
        auto stripped = strip(line);
        if (stripped.empty() || stripped.starts_with('#')) {
            depth += static_cast<int>(std::count(stripped.begin(), stripped.end(), '{') -
                                      std::count(stripped.begin(), stripped.end(), '}'));
            continue;
        }
        if (depth == 0) {
            if (auto m = ig.search_match(line); m && ig.match_line(line))
                g.insert(m->named("name"));
            else if (auto m2 = ug.search_match(line); m2 && ug.match_line(line))
                g.insert(m2->named("name"));
        }
        depth += static_cast<int>(std::count(stripped.begin(), stripped.end(), '{') -
                                  std::count(stripped.begin(), stripped.end(), '}'));
    }
    return g;
}

bool global_rmw(std::string_view body, std::string_view name) {
    auto v = re_escape(name);
    if (Regex("\\b" + v + "\\s*=\\s*" + v + "\\s*\\+").search(body)) return true;
    if (Regex("\\b" + v + "\\+\\+").search(body)) return true;
    if (Regex("\\+\\+\\s*" + v + "\\b").search(body)) return true;
    if (Regex("\\b" + v + "\\s*\\+=").search(body)) return true;
    return false;
}

bool fn_has_lock_call(std::string_view body) {
    static Regex re(R"((?i)\b(?:mtx_lock|pthread_mutex|\w*lock\w*)\s*\()");
    return re.search(body);
}

bool similar_param_type(std::string_view a, std::string_view b) {
    return squeeze_ws(to_lower_copy(std::string(a))) == squeeze_ws(to_lower_copy(std::string(b)));
}

std::vector<std::pair<std::string, std::string>> integer_param_names(const FunctionInfo& fn) {
    static Regex ty(R"((?i)\b(?:int|long|short|unsigned|size_t|uint)\b)");
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [typ, name] : fn.params) {
        if (name.empty() || typ.find('*') != std::string::npos || typ.find('[') != std::string::npos)
            continue;
        if (ty.search(typ)) out.emplace_back(name, typ);
    }
    return out;
}

bool param_guarded(std::string_view name, const std::vector<std::string>& body_lines) {
    auto v = re_escape(name);
    std::vector<Regex> tests;
    tests.emplace_back("\\bif\\s*\\(\\s*" + v + "\\s*<\\s*");
    tests.emplace_back("\\bif\\s*\\(\\s*" + v + "\\s*>\\s*");
    tests.emplace_back("\\bif\\s*\\(\\s*" + v + "\\s*>=\\s*");
    tests.emplace_back("\\bif\\s*\\(\\s*" + v + "\\s*<=\\s*");
    tests.emplace_back("\\bif\\s*\\(\\s*" + v + "\\s*==\\s*NULL\\b");
    static Regex ret(R"(\breturn\b)");
    for (std::size_t i = 0; i < body_lines.size(); ++i) {
        bool hit = false;
        for (auto& p : tests)
            if (p.search(body_lines[i])) {
                hit = true;
                break;
            }
        if (!hit) continue;
        auto nxt = i + 1 < body_lines.size() ? body_lines[i + 1] : std::string();
        if (ret.search(body_lines[i]) || ret.search(nxt)) return true;
    }
    return false;
}

bool has_if_operand(std::string_view name, std::string_view body) {
    Regex re("\\bif\\s*\\([^)]*\\b" + re_escape(name) + "\\b");
    return re.search(body);
}

bool subscript_uses(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    if (Regex("\\[[^\\]]*\\b" + v + "\\b[^\\]]*\\]").search(body)) return true;
    return Regex("\\b" + v + "\\s*\\[").search(body);
}

int first_subscript_line(const FunctionInfo& fn, std::string_view name,
                         const std::vector<std::string>& lines) {
    auto v = re_escape(name);
    Regex a("\\[[^\\]]*\\b" + v + "\\b[^\\]]*\\]");
    Regex b("\\b" + v + "\\s*\\[");
    int start = fn.span.first, end = fn.span.second;
    if (start < 1) return start;
    auto a0 = static_cast<std::size_t>(start - 1);
    auto b0 = std::min(static_cast<std::size_t>(std::max(end, 0)), lines.size());
    int i = start;
    for (auto p = a0; p < b0; ++p, ++i) {
        if (a.search(lines[p]) || b.search(lines[p])) return i;
    }
    return start;
}

bool explicit_nul_store_for(std::string_view var, std::string_view text) {
    Regex re("\\b" + re_escape(var) + "\\s*\\[\\s*[^\\]]+\\s*\\]\\s*=\\s*(?:0\\b|'\\\\0')");
    return re.search(text);
}

bool chroot_followed_by_chdir(const std::vector<std::string>& body_lines, int chroot_idx) {
    for (int i = chroot_idx + 1; i < static_cast<int>(body_lines.size()); ++i)
        if (find_call_args(body_lines[static_cast<std::size_t>(i)], "chdir")) return true;
    return false;
}

std::vector<std::string> getenv_assign_vars(std::string_view ln) {
    static Regex ge(R"(\b(?:secure_)?getenv\s*\()");
    static Regex as(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:secure_)?getenv\s*\()");
    if (!ge.search(ln)) return {};
    auto m = as.search_match(ln);
    if (m) return {m->named("var")};
    return {};
}

bool getenv_in_truthy_if(std::string_view ln, std::string_view var) {
    Regex re("\\bif\\s*\\([^)]*\\b" + re_escape(var) + "\\s*=\\s*(?:secure_)?getenv\\s*\\(");
    return re.search(ln);
}

bool getenv_uses(std::string_view var, std::string_view ln) {
    auto v = re_escape(var);
    if (Regex("\\breturn\\s+" + v + "\\s*;").search(ln)) return false;
    if (Regex("\\b" + v + "\\s*\\[").search(ln)) return true;
    if (Regex("\\*\\s*" + v + "\\b").search(ln)) return true;
    for (auto& [callee, idxs] : kGetenvUseCallees) {
        auto args = find_call_args(ln, callee);
        if (!args) continue;
        for (int idx : idxs)
            if (idx < static_cast<int>(args->size()) && strip((*args)[static_cast<std::size_t>(idx)]) == var)
                return true;
    }
    for (auto& [fname, fmt_idx] : kFmtFn) {
        auto args = find_call_args(ln, fname);
        if (!args || static_cast<int>(args->size()) <= fmt_idx) continue;
        if (!fmt_literal_has_percent_s((*args)[static_cast<std::size_t>(fmt_idx)])) continue;
        for (std::size_t i = static_cast<std::size_t>(fmt_idx) + 1; i < args->size(); ++i)
            if (strip((*args)[i]) == var) return true;
    }
    return false;
}

bool getenv_only_returned(std::string_view var, const std::vector<std::string>& chunk, int assign_i) {
    auto v = re_escape(var);
    Regex retb(R"(\breturn\b)");
    Regex retv("\\breturn\\s+" + v + "\\s*;");
    bool saw = false;
    for (int j = assign_i + 1; j < static_cast<int>(chunk.size()); ++j) {
        auto& ln = chunk[static_cast<std::size_t>(j)];
        if (getenv_uses(var, ln)) return false;
        if (retb.search(ln)) {
            if (retv.search(ln)) saw = true;
            else return false;
        }
    }
    return saw;
}

std::vector<std::string> strdup_assign_vars(std::string_view ln) {
    static Regex sd(R"(\bstrn?dup\s*\()");
    static Regex as(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?strn?dup\s*\()");
    if (!sd.search(ln)) return {};
    auto m = as.search_match(ln);
    if (m) return {m->named("var")};
    return {};
}

bool strdup_in_truthy_if(std::string_view ln, std::string_view var) {
    Regex re("\\bif\\s*\\([^)]*\\b" + re_escape(var) + "\\s*=\\s*strn?dup\\s*\\(");
    return re.search(ln);
}

bool strdup_only_returned(std::string_view var, const std::vector<std::string>& chunk, int assign_i) {
    auto v = re_escape(var);
    Regex retb(R"(\breturn\b)");
    Regex retv("\\breturn\\s+" + v + "\\s*;");
    bool saw = false;
    for (int j = assign_i + 1; j < static_cast<int>(chunk.size()); ++j) {
        auto& ln = chunk[static_cast<std::size_t>(j)];
        if (getenv_uses(var, ln)) return false;
        if (retb.search(ln)) {
            if (retv.search(ln)) saw = true;
            else return false;
        }
    }
    return saw;
}

bool umask_arg_unsafe(std::string_view arg) {
    auto lit = parse_int_literal(arg);
    return lit && *lit == 0;
}

std::unordered_set<std::string> atoi_vars(std::string_view body) {
    static Regex re(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:atoi|atol|atoll)\s*\()");
    std::unordered_set<std::string> out;
    for (auto& m : re.finditer(body)) out.insert(m.named("var"));
    return out;
}

bool has_bounds_guard(std::string_view var, std::string_view body) {
    auto v = re_escape(var);
    if (Regex("\\bif\\s*\\([^)]*" + v + "\\s*<\\s*0[^)]*\\|\\|[^)]*" + v + "\\s*>=").search(body))
        return true;
    if (Regex("\\bif\\s*\\([^)]*" + v + "\\s*>=\\s*\\d+[^)]*\\|\\|[^)]*" + v + "\\s*<").search(body))
        return true;
    bool has_lower = Regex("\\bif\\s*\\(\\s*0\\s*>\\s*" + v + "\\b").search(body) ||
                     Regex("\\bif\\s*\\(\\s*" + v + "\\s*<\\s*0\\b").search(body) ||
                     Regex("\\bif\\s*\\(\\s*" + v + "\\s*<=\\s*-1\\b").search(body);
    bool has_upper = Regex("\\bif\\s*\\(\\s*" + v + "\\s*>=\\s*\\d+\\b").search(body) ||
                     Regex("\\bif\\s*\\(\\s*" + v + "\\s*>\\s*\\d+\\b").search(body) ||
                     Regex("\\bif\\s*\\(\\s*\\d+\\s*<=\\s*" + v + "\\b").search(body) ||
                     Regex("\\bif\\s*\\(\\s*\\d+\\s*>\\s*" + v + "\\b").search(body);
    return has_lower && has_upper;
}

bool atoi_index_or_size_use(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    if (Regex("\\[\\s*" + v + "\\s*\\]").search(text)) return true;
    if (Regex("\\b(?:malloc|alloca)\\s*\\(\\s*" + v + "\\b").search(text)) return true;
    if (Regex("\\bcalloc\\s*\\(\\s*" + v + "\\b").search(text)) return true;
    return Regex("\\brealloc\\s*\\([^,]+,\\s*" + v + "\\b").search(text);
}

std::map<std::string, std::vector<std::string>> named_enum_members(std::string_view text) {
    static Regex re(R"(\benum\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>[^{}]*)\})");
    static Regex ident(R"(^[A-Za-z_]\w*$)");
    std::map<std::string, std::vector<std::string>> out;
    for (auto& m : re.finditer(text)) {
        std::vector<std::string> members;
        auto body = m.named("body");
        std::size_t p = 0;
        while (p <= body.size()) {
            auto c = body.find(',', p);
            auto part = squeeze_ws(c == std::string::npos ? body.substr(p) : body.substr(p, c - p));
            if (!part.empty()) {
                auto eq = part.find('=');
                auto name = strip(eq == std::string::npos ? part : part.substr(0, eq));
                if (fullmatch(ident, name)) members.push_back(name);
            }
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (!members.empty()) out[m.named("tag")] = std::move(members);
    }
    return out;
}

std::map<std::string, std::string> enum_vars_in_fn(
    const FunctionInfo& fn, const std::map<std::string, std::vector<std::string>>& tags) {
    static Regex param(R"(\benum\s+([A-Za-z_]\w*)\b)");
    static Regex loc(R"(\benum\s+(?P<tag>[A-Za-z_]\w*)\s+(?P<name>[A-Za-z_]\w*)\b)");
    std::map<std::string, std::string> vars;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        auto m = param.search_match(typ);
        if (m && tags.contains(m->group(1))) vars[name] = m->group(1);
    }
    for (auto& m : loc.finditer(fn.body)) {
        auto tag = m.named("tag"), name = m.named("name");
        if (tags.contains(tag) && !kKw.contains(name)) vars[name] = tag;
    }
    return vars;
}

struct IdentSwitch {
    std::string var, body;
    int line_off = 0;
};

std::vector<IdentSwitch> ident_switch_bodies(std::string_view text) {
    static Regex re(R"(\bswitch\s*\(\s*([A-Za-z_]\w*)\s*\))");
    std::vector<IdentSwitch> out;
    for (auto& m : re.finditer(text)) {
        auto var = m.group(1);
        auto rest = text.substr(static_cast<std::size_t>(m.spans[0].second));
        auto brace = rest.find('{');
        if (brace == std::string_view::npos || rest.substr(0, brace).find(';') != std::string_view::npos)
            continue;
        int depth = 0;
        std::optional<std::size_t> end;
        for (std::size_t k = 0; k < rest.size() - brace; ++k) {
            auto ch = rest[brace + k];
            if (ch == '{') ++depth;
            else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    end = brace + k;
                    break;
                }
            }
        }
        if (!end) continue;
        IdentSwitch sw;
        sw.var = std::move(var);
        sw.body = std::string(rest.substr(brace + 1, *end - (brace + 1)));
        sw.line_off = count_nl(text.substr(0, static_cast<std::size_t>(m.spans[0].second) + brace));
        out.push_back(std::move(sw));
    }
    return out;
}

std::vector<std::string> pointer_param_names(const FunctionInfo& fn) {
    std::vector<std::string> out;
    for (auto& [typ, name] : fn.params)
        if (!name.empty() && typ.find('*') != std::string::npos) out.push_back(name);
    return out;
}

bool param_null_tested(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    std::vector<std::string> tests = {
        "\\bif\\s*\\(\\s*" + v + "\\s*==\\s*(?:NULL|nullptr|0)\\b",
        "\\bif\\s*\\(\\s*(?:NULL|nullptr|0)\\s*==\\s*" + v + "\\b",
        "\\bif\\s*\\(\\s*" + v + "\\s*!=\\s*(?:NULL|nullptr|0)\\b",
        "\\bif\\s*\\(\\s*(?:NULL|nullptr|0)\\s*!=\\s*" + v + "\\b",
        "\\bif\\s*\\(\\s*!\\s*" + v + "\\b",
    };
    for (auto& p : tests)
        if (Regex(p).search(body)) return true;
    return false;
}

bool param_if_guard(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    if (Regex("\\bif\\s*\\(\\s*!\\s*" + v + "\\b").search(body)) return true;
    return Regex("\\bif\\s*\\(\\s*" + v + "\\b").search(body);
}

bool ptr_index_arith(std::string_view p, std::string_view n, std::string_view text) {
    auto pe = re_escape(p), ne = re_escape(n);
    if (Regex("\\*\\s*\\(\\s*" + pe + "\\s*\\+\\s*" + ne + "\\s*\\)").search(text)) return true;
    return Regex("\\b" + pe + "\\s*\\[\\s*" + ne + "\\s*\\]").search(text);
}

std::string comment_preamble(const std::vector<std::string>& lines, int body_start,
                             int max_comments = 3) {
    int i = body_start - 1;
    bool in_block = false;
    while (i >= 0) {
        auto& raw = lines[static_cast<std::size_t>(i)];
        auto s = strip(raw);
        if (in_block) {
            if (raw.find("/*@") != std::string::npos || s.starts_with("/*")) in_block = false;
            --i;
            continue;
        }
        if (s.empty()) {
            --i;
            continue;
        }
        if (s.starts_with("//")) {
            --i;
            continue;
        }
        if (s.ends_with("*/") || s.starts_with("/*") || s.starts_with("*")) {
            if (raw.find("/*") != std::string::npos && raw.find("*/") != std::string::npos) {
                --i;
                continue;
            }
            in_block = true;
            --i;
            continue;
        }
        break;
    }
    auto preamble = join_range(lines, static_cast<std::size_t>(i + 1),
                               static_cast<std::size_t>(std::max(body_start, 0)));
    std::vector<std::string> kept;
    for (auto& ln : split_lines(preamble)) {
        auto s = strip(ln);
        if (s.empty()) continue;
        if (s.starts_with("//") || s.starts_with("/*") || s.starts_with("*") ||
            s.find("*/") != std::string::npos)
            kept.push_back(ln);
    }
    if (static_cast<int>(kept.size()) > max_comments)
        kept.erase(kept.begin(), kept.end() - max_comments);
    return join_all(kept);
}

bool has_increment_on(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    return Regex("\\b" + v + "\\s*\\+\\+").search(body) ||
           Regex("\\+\\+\\s*" + v + "\\b").search(body) ||
           Regex("\\b" + v + "\\s*\\+").search(body) || Regex("\\+\\s*" + v + "\\b").search(body);
}

std::unordered_set<std::string> local_structs(const FunctionInfo& fn) {
    static Regex re(
        R"re((?m)^\s*(?P<static>static\s+)?struct\s+\w+\s+(?P<name>[A-Za-z_]\w*)\s*(?P<zero>=\s*\{0\})?\s*;)re",
        true);
    std::unordered_set<std::string> params, names;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : re.finditer(fn.body)) {
        if (!m.named("static").empty() || !m.named("zero").empty()) continue;
        auto name = m.named("name");
        if (!params.contains(name) && !kKw.contains(name)) names.insert(name);
    }
    return names;
}

bool local_zeroed(std::string_view body, std::string_view var) {
    auto v = re_escape(var);
    if (Regex("memset\\s*\\(\\s*&?\\s*" + v + "\\s*,\\s*0\\b").search(body)) return true;
    return Regex("struct\\s+\\w+\\s+" + v + "\\s*=\\s*\\{0\\}").search(body);
}

std::optional<std::string> precond_var_from_preamble(std::string_view preamble) {
    static Regex pats[] = {
        Regex(R"((?i)requires\s+(\w+)\s*>\s*0)"),
        Regex(R"((?i)(\w+)\s+must\s+be\s+positive)"),
        Regex(R"((?i)(\w+)\s+is\s+positive)"),
        Regex(R"((?i)(\w+)\s*>\s*0)"),
    };
    for (auto& pat : pats)
        if (auto m = pat.search_match(preamble)) return m->group(1);
    return std::nullopt;
}

std::unordered_set<std::string> parsed_input_vars(std::string_view body) {
    static Regex as(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:atoi|strtol|strtoul)\s*\()");
    static Regex sc(R"(\bscanf\s*\([^)]*&\s*(?P<var>[A-Za-z_]\w*))");
    std::unordered_set<std::string> names;
    for (auto& m : as.finditer(body)) names.insert(m.named("var"));
    for (auto& m : sc.finditer(body)) names.insert(m.named("var"));
    return names;
}

bool has_if_guard(std::string_view name, std::string_view body) {
    return Regex("\\bif\\s*\\(\\s*" + re_escape(name) + "\\b").search(body);
}

bool unvalidated_index_use(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    if (Regex("\\[\\s*" + v + "\\s*\\]").search(text)) return true;
    return Regex("(?:/|%)\\s*" + v + "\\b").search(text);
}

bool ptr_rand_store(std::string_view ptr, std::string_view line) {
    auto pe = re_escape(ptr);
    if (Regex("\\b" + pe + "\\s*\\[[^\\]]+\\]\\s*=\\s*(?:rand|random)\\s*\\(").search(line))
        return true;
    return Regex("\\*\\s*" + pe + "\\s*=\\s*(?:rand|random)\\s*\\(").search(line);
}

bool ptr_local_store(std::string_view ptr, std::string_view local, std::string_view text) {
    auto pe = re_escape(ptr), le = re_escape(local);
    if (Regex("\\b" + pe + "\\s*\\[[^\\]]+\\]\\s*=\\s*" + le + "\\b").search(text)) return true;
    if (Regex("\\*\\s*" + pe + "\\s*=\\s*" + le + "\\b").search(text)) return true;
    return Regex("\\b(?:memcpy|memmove)\\s*\\(\\s*" + pe + "\\b[^;]*\\b" + le + "\\b").search(text);
}

std::optional<std::string> realloc_zero_old_ptr(std::string_view ln) {
    auto args = find_call_args(ln, "realloc");
    if (!args || args->size() < 2) return std::nullopt;
    if (parse_int_literal((*args)[1]) != 0) return std::nullopt;
    static Regex cast(R"(^\([^)]*\)\s*)");
    auto ptr = strip((*args)[0]);
    if (auto m = cast.search_match(ptr); m && cast.match_line(ptr))
        ptr = strip(ptr.substr(static_cast<std::size_t>(m->spans[0].second)));
    Regex self("^" + re_escape(ptr) + "\\s*=\\s*(?:\\([^)]*\\)\\s*)*realloc\\s*\\(");
    if (self.match_line(ln)) return std::nullopt;
    return ptr;
}

bool stale_ptr_use(std::string_view var, std::string_view ln) {
    auto v = re_escape(var);
    if (Regex("\\b" + v + "\\s*\\[").search(ln)) return true;
    if (Regex("\\*\\s*" + v + "\\b").search(ln) || Regex("\\b" + v + "\\s*->").search(ln))
        return true;
    if (Regex("\\bfree\\s*\\(\\s*(?:\\([^)]*\\)\\s*)*" + v + "\\s*\\)").search(ln)) return true;
    return Regex("\\breturn\\s+" + v + "\\s*;").search(ln);
}

std::string unwrap_parens(std::string expr) {
    auto t = strip(expr);
    while (t.starts_with('(') && t.ends_with(')')) {
        auto inner = strip(t.substr(1, t.size() - 2));
        if (static_cast<int>(std::count(inner.begin(), inner.end(), '(')) !=
            static_cast<int>(std::count(inner.begin(), inner.end(), ')')))
            break;
        t = inner;
    }
    return t;
}

std::unordered_set<std::string> flex_array_tags(std::string_view text) {
    static Regex def(R"(\b(?:struct|class)\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>[^{}]*)\})");
    static Regex tail(R"([A-Za-z_]\w*\s*\[\s*(?:0\s*)?\]\s*$)");
    std::unordered_set<std::string> tags;
    for (auto& m : def.finditer(text)) {
        std::vector<std::string> decls;
        auto body = m.named("body");
        std::size_t p = 0;
        while (p <= body.size()) {
            auto c = body.find(';', p);
            auto part = strip(c == std::string::npos ? body.substr(p) : body.substr(p, c - p));
            if (!part.empty()) decls.push_back(part);
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (!decls.empty() && tail.search(decls.back())) tags.insert(m.named("tag"));
    }
    return tags;
}

std::string re_sub(const Regex& re, std::string_view repl, const std::string& s) {
    std::string out;
    std::size_t off = 0;
    for (auto& m : re.finditer(s)) {
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        out.append(s, off, a - off);
        out.append(repl);
        off = static_cast<std::size_t>(std::max(0, m.spans[0].second));
    }
    out.append(s, off, std::string::npos);
    return out;
}

std::pair<std::optional<std::string>, std::optional<std::string>> bare_sizeof(std::string_view expr) {
    static Regex st(R"(^sizeof\s*\(\s*(?:struct\s+)?([A-Za-z_]\w*)\s*\)$)");
    static Regex star(R"(^sizeof\s*(?:\(\s*\*\s*([A-Za-z_]\w*)\s*\)|\*\s*([A-Za-z_]\w*))$)");
    auto t = unwrap_parens(std::string(expr));
    if (t.find('+') != std::string::npos) return {std::nullopt, std::nullopt};
    if (fullmatch(st, t)) {
        auto m = st.search_match(t);
        return {std::string("struct"), m->group(1)};
    }
    if (fullmatch(star, t)) {
        auto m = star.search_match(t);
        auto id = m->group(1);
        if (id.empty()) id = m->group(2);
        return {std::string("star"), id};
    }
    return {std::nullopt, std::nullopt};
}

std::unordered_set<std::string> pointer_locals(const FunctionInfo& fn) {
    static Regex decl(
        R"(\b(?:const\s+|volatile\s+)?(?:struct\s+[A-Za-z_]\w+\s+)?(?:\w+)\s*\*\s*([A-Za-z_]\w*)\b)");
    std::unordered_set<std::string> ptrs;
    for (auto& [typ, name] : fn.params)
        if (!name.empty() && typ.find('*') != std::string::npos) ptrs.insert(name);
    for (auto& m : decl.finditer(fn.body)) ptrs.insert(m.group(1));
    return ptrs;
}

// --- lints ---

void mem_lifetime(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex free_call(
        R"(\b(?:free|kfree|ck_free)\s*\(\s*(?:\([^)]*\)\s*)*(?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\))");
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::map<std::string, int> freed_uaf, freed_df;
        std::unordered_set<std::string> reported_uaf;
        std::set<std::pair<std::string, int>> reported_df;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto& ln = chunk[static_cast<std::size_t>(i)];
            for (auto it = freed_uaf.begin(); it != freed_uaf.end();) {
                if (reassigns_var(it->first, ln)) {
                    freed_df.erase(it->first);
                    reported_uaf.erase(it->first);
                    it = freed_uaf.erase(it);
                } else {
                    if (has_assignment(it->first, ln)) freed_df.erase(it->first);
                    ++it;
                }
            }
            for (auto& [var, _] : freed_uaf) {
                if (reported_uaf.contains(var)) continue;
                if (reads_var(var, ln)) {
                    lint_add(out, rel, fn.name, start + i, "MEM-UAF",
                             var + " used after free without reassignment", lines);
                    reported_uaf.insert(var);
                }
            }
            auto fm = free_call.search_match(ln);
            if (!fm) continue;
            auto var = cap_norm(fm->named("var"));
            if (freed_df.contains(var)) {
                auto key = std::pair{var, freed_df[var]};
                if (!reported_df.contains(key)) {
                    lint_add(out, rel, fn.name, start + i, "MEM-DOUBLE-FREE",
                             var + " freed again without reassignment", lines);
                    reported_df.insert(key);
                }
            }
            freed_uaf[var] = i;
            freed_df[var] = i;
        }
    }
}

void shift31_realloc(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex shift31(R"((?<![A-Za-z0-9_])1\s*<<\s*(?:31|0x1f|0x1F)\b)");
    static Regex realloc_self(
        R"((?P<p>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*=\s*realloc\s*\(\s*(?P=p)\s*,)");
    static Regex oneu(R"((?i)1u\s*<<)");
    static Regex unsigned_w(R"(\bunsigned\b)");
    static Regex uint_cast(R"(\(\s*uint)");
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto& ln = lines[static_cast<std::size_t>(i)];
        auto fn = func_of_line(funcs, i + 1);
        if (shift31.search(ln) && !unsigned_w.search(ln) && !uint_cast.search(ln) && !oneu.search(ln))
            lint_add(out, rel, fn, i + 1, "INT-SHIFT-UB", "signed 1<<31 is undefined", lines);
        if (auto m = realloc_self.search_match(ln))
            lint_add(out, rel, fn, i + 1, "MEM-REALLOC-SELF",
                     m->named("p") + " = realloc(" + m->named("p") + ", …) leaks on failure", lines);
    }
}

void masked_switch(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"(switch\s*\(\s*([^)]*?&\s*(?:\([^)]+\)|0x[0-9a-fA-F]+|\d+))\s*\))", false, true);
    static Regex defl(R"(\bdefault\s*:)");
    static Regex caseb(R"(\bcase\b)");
    static Regex litre(R"(0x([0-9a-fA-F]+)|(\d+)\s*$)");
    static Regex ident(R"([A-Za-z_]\w*)");
    static Regex assigned_re(R"(\b([A-Za-z_]\w*)\s*=)");
    static Regex decl(
        R"(^\s*(?:int|unsigned|long|char|size_t|uint\d+_t|int\d+_t)\s+([A-Za-z_]\w*)\s*;)");
    auto text = join_all(lines);
    for (auto& m : re.finditer(text)) {
        auto expr = m.group(1);
        int line = count_nl(text.substr(0, static_cast<std::size_t>(std::max(0, m.spans[0].first)))) + 1;
        auto rest = text.substr(static_cast<std::size_t>(m.spans[0].second));
        auto brace = rest.find('{');
        if (brace == std::string::npos) continue;
        int depth = 0;
        std::optional<std::size_t> end;
        for (std::size_t k = 0; k < rest.size() - brace; ++k) {
            auto ch = rest[brace + k];
            if (ch == '{') ++depth;
            else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    end = brace + k;
                    break;
                }
            }
        }
        if (!end) continue;
        auto body = rest.substr(brace, *end - brace + 1);
        if (defl.search(body)) continue;
        auto cases = caseb.finditer(body);
        std::string tail = expr;
        for (char& c : tail)
            if (c == ')') c = ' ';
        auto amp = tail.rfind('&');
        auto mask_s = strip(amp == std::string::npos ? tail : tail.substr(amp + 1));
        auto lit = litre.search_match(mask_s);
        int states = 0, nbits = 0;
        if (lit && (lit->spans.empty() || lit->spans[0].first >= 0)) {
            int mask = 0;
            if (!lit->group(1).empty()) mask = static_cast<int>(std::stoul(lit->group(1), nullptr, 16));
            else mask = std::stoi(lit->group(2));
            nbits = static_cast<int>(std::popcount(static_cast<unsigned>(mask)));
            if (nbits > 4) continue;
            states = 1 << nbits;
        } else {
            auto rhs = expr;
            auto a2 = rhs.find('&');
            if (a2 != std::string::npos) rhs = rhs.substr(a2 + 1);
            std::unordered_set<std::string> named;
            for (auto& im : ident.finditer(rhs)) {
                auto n = im.text;
                if (n != "if" && n != "switch") named.insert(n);
            }
            nbits = std::max(1, static_cast<int>(named.size()));
            if (nbits > 4) continue;
            states = 1 << nbits;
        }
        if (static_cast<int>(cases.size()) >= states) continue;
        std::unordered_set<std::string> assigned;
        for (auto& am : assigned_re.finditer(body)) assigned.insert(am.group(1));
        int after_line = line + count_nl(body);
        auto after = join_range(lines, static_cast<std::size_t>(std::max(0, after_line)),
                                static_cast<std::size_t>(std::max(0, after_line + 40)));
        std::unordered_set<std::string> declared;
        int from = std::max(0, line - 80);
        for (int li = from; li < line && li < static_cast<int>(lines.size()); ++li) {
            auto dm = decl.search_match(lines[static_cast<std::size_t>(li)]);
            if (dm && decl.match_line(lines[static_cast<std::size_t>(li)]))
                declared.insert(dm->group(1));
        }
        std::string used;
        for (auto& n : assigned) {
            if (!declared.contains(n)) continue;
            if (Regex("\\b" + n + "\\b").search(after)) {
                used = n;
                break;
            }
        }
        if (used.empty()) continue;
        lint_add(out, rel, func_of_line(funcs, line), line, "UNINIT-SWITCH",
                 "masked switch has " + std::to_string(cases.size()) + " arms for " +
                     std::to_string(states) + " states; " + used + " may escape uninitialised",
                 lines);
    }
}

void null_branch(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex null_test(
        R"(\bif\s*\(\s*(?:(?P<p1>[A-Za-z_]\w*)\s*==\s*(?:NULL|0|nullptr)|!\s*(?P<p2>[A-Za-z_]\w*))\s*\))");
    int i = 0;
    while (i < static_cast<int>(lines.size())) {
        auto m = null_test.search_match(lines[static_cast<std::size_t>(i)]);
        if (!m) {
            ++i;
            continue;
        }
        auto p = m->named("p1");
        if (p.empty()) p = m->named("p2");
        std::vector<std::string> block;
        int j = i + 1;
        auto cur = lines[static_cast<std::size_t>(i)];
        auto nxt = j < static_cast<int>(lines.size()) ? lines[static_cast<std::size_t>(j)] : std::string();
        if (j < static_cast<int>(lines.size()) && (cur + nxt).find('{') != std::string::npos) {
            int depth = static_cast<int>(std::count(cur.begin(), cur.end(), '{') -
                                         std::count(cur.begin(), cur.end(), '}'));
            if (depth <= 0 && j < static_cast<int>(lines.size())) {
                depth += static_cast<int>(std::count(nxt.begin(), nxt.end(), '{') -
                                          std::count(nxt.begin(), nxt.end(), '}'));
                block.push_back(nxt);
                ++j;
            }
            while (j < static_cast<int>(lines.size()) && depth > 0) {
                auto& ln = lines[static_cast<std::size_t>(j)];
                depth += static_cast<int>(std::count(ln.begin(), ln.end(), '{') -
                                          std::count(ln.begin(), ln.end(), '}'));
                block.push_back(ln);
                ++j;
            }
        } else if (j < static_cast<int>(lines.size())) {
            block.push_back(lines[static_cast<std::size_t>(j)]);
            ++j;
        }
        auto body = join_all(block);
        if (Regex("\\b" + re_escape(p) + "\\s*=").search(body)) {
            i = j;
            continue;
        }
        if (Regex("\\b" + re_escape(p) + "\\s*(->|\\.)").search(body)) {
            int ln = i + 1;
            lint_add(out, rel, func_of_line(funcs, ln), ln, "PTR-NULL-DEREF",
                     p + " dereferenced in the branch that proved it NULL", lines);
        }
        i = j > i + 1 ? j : i + 1;
    }
}

void lock_balance(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex call(R"(\b([A-Za-z_]\w*lock[A-Za-z0-9_]*)\s*\(([^)]*)\)\s*;)");
    static Regex unlk(R"((?i)unlock)");
    static Regex ret(R"(^\s*return\b)");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::map<std::pair<std::string, std::string>, std::vector<int>> pairs, unlocks;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : call.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto name = m.group(1);
                auto arg = squeeze_ws(m.group(2));
                if (unlock_of(name)) pairs[{name, arg}].push_back(i);
                if (unlk.search(name)) unlocks[{name, arg}].push_back(i);
            }
        }
        std::vector<int> returns;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (ret.match_line(body_lines[static_cast<std::size_t>(i)])) returns.push_back(i);
        if (returns.size() < 2) continue;
        for (auto& [key, hits] : pairs) {
            auto u = unlock_of(key.first);
            if (!u) continue;
            auto& rels = unlocks[{*u, key.second}];
            if (rels.empty()) continue;
            std::vector<int> held_returns;
            for (int r : returns) {
                int last_lock = max_before(hits, r);
                int last_un = max_before(rels, r);
                if (last_lock > last_un) held_returns.push_back(r);
            }
            if (!held_returns.empty() && held_returns.size() < returns.size()) {
                int ln = fn.span.first + held_returns[0];
                lint_add(out, rel, fn.name, ln, "LOCK-IMBALANCE",
                         key.first + " held on " + std::to_string(held_returns.size()) + " of " +
                             std::to_string(returns.size()) + " returns; released on the others",
                         lines);
            }
        }
    }
}

void lock_double_unlock(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex call(R"(\b([A-Za-z_]\w*lock[A-Za-z0-9_]*)\s*\(([^)]*)\)\s*;)");
    static Regex unlk(R"((?i)unlock)");
    for (auto& fn : funcs) {
        std::map<std::pair<std::string, std::string>, std::optional<bool>> held;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            bool found = false;
            for (auto& m : call.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto name = m.group(1);
                auto arg = squeeze_ws(m.group(2));
                if (unlk.search(name)) {
                    auto key = std::pair{name, arg};
                    auto it = held.find(key);
                    if (it != held.end() && it->second == false) {
                        lint_add(out, rel, fn.name, fn.span.first + i, "LOCK-DOUBLE-UNLOCK",
                                 name + "(" + arg + ") released twice without an acquire between",
                                 lines);
                        found = true;
                        break;
                    }
                    held[key] = false;
                    continue;
                }
                if (auto u = unlock_of(name)) held[{*u, arg}] = true;
            }
            if (found) break;
        }
    }
}

void lock_double_lock(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex call(R"(\b([A-Za-z_]\w*lock[A-Za-z0-9_]*)\s*\(([^)]*)\)\s*;)");
    static Regex unlk(R"((?i)unlock)");
    for (auto& fn : funcs) {
        std::map<std::pair<std::string, std::string>, std::optional<bool>> held;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            bool found = false;
            for (auto& m : call.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto name = m.group(1);
                auto arg = squeeze_ws(m.group(2));
                if (unlk.search(name)) {
                    if (auto lock_name = lock_of(name)) held[{*lock_name, arg}] = false;
                    continue;
                }
                if (!unlock_of(name)) continue;
                auto key = std::pair{name, arg};
                auto it = held.find(key);
                if (it != held.end() && it->second == true) {
                    lint_add(out, rel, fn.name, fn.span.first + i, "LOCK-DOUBLE-LOCK",
                             name + "(" + arg + ") acquired twice without a release between", lines);
                    found = true;
                    break;
                }
                held[key] = true;
            }
            if (found) break;
        }
    }
}

void lock_missing_init(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex local_re(
        R"re((?m)^\s*(?:static\s+)?(?:pthread_mutex_t|mtx_t)\s+(?P<name>[A-Za-z_]\w*)(?P<init>\s*=)?)re",
        true);
    static Regex init_call(R"(\b(?:pthread_mutex_init|mtx_init)\s*\(\s*&?\s*([A-Za-z_]\w*))");
    static Regex lock_call(
        R"(\b(?:pthread_mutex_lock|pthread_mutex_trylock|mtx_lock|mtx_trylock)\s*\(\s*&?\s*([A-Za-z_]\w*))");
    for (auto& fn : funcs) {
        std::unordered_set<std::string> inited, locals;
        for (auto& m : local_re.finditer(fn.body)) {
            locals.insert(m.named("name"));
            if (!m.named("init").empty()) inited.insert(m.named("name"));
        }
        if (locals.empty()) continue;
        for (auto& m : init_call.finditer(fn.body)) inited.insert(m.group(1));
        int start = fn.span.first;
        std::unordered_set<std::string> reported;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (auto m = lock_call.search_match(ln)) {
                auto name = m->group(1);
                if (locals.contains(name) && !inited.contains(name) && !reported.contains(name)) {
                    reported.insert(name);
                    lint_add(out, rel, fn.name, start + i, "LOCK-MISSING-INIT",
                             name + " is locked without pthread_mutex_init/"
                             "mtx_init or a static initializer",
                             lines);
                }
            }
            ++i;
        }
    }
}

void div_zero_const(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex dz(R"([/%]\s*0\b)");
    static Regex flt(R"([0-9]\s*[/%]\s*0\.\d)");
    static Regex zdot(R"([/%]\s*0\.)");
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto& ln = lines[static_cast<std::size_t>(i)];
        if (dz.search(ln) && !flt.search(ln)) {
            if (zdot.search(ln)) continue;
            lint_add(out, rel, func_of_line(funcs, i + 1), i + 1, "INT-DIV-ZERO",
                     "integer division or modulo by constant 0", lines);
        }
    }
}

void onesided_index(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex onesided(
        R"(\bif\s*\(\s*(?P<i>[A-Za-z_]\w*)\s*(?:>|>=)\s*(?P<n>[A-Za-z_]\w*)\s*\))");
    for (auto& fn : funcs) {
        auto signed_n = signed_index_names(fn);
        if (signed_n.empty()) continue;
        int start = fn.span.first, end = fn.span.second;
        auto chunk = join_range(lines, static_cast<std::size_t>(std::max(0, start - 1)),
                                static_cast<std::size_t>(std::max(end, 0)));
        std::unordered_set<std::string> seen;
        for (auto& m : onesided.finditer(chunk)) {
            auto i = m.named("i");
            if (!signed_n.contains(i) || seen.contains(i)) continue;
            if (has_floor(i, chunk) || pinned_or_loop(i, chunk)) continue;
            if (!Regex("\\[\\s*" + re_escape(i) + "\\s*\\]").search(chunk)) continue;
            seen.insert(i);
            int line = start + count_nl(chunk.substr(0, static_cast<std::size_t>(std::max(0, m.spans[0].first))));
            lint_add(out, rel, fn.name, line, "MEM-ONESIDED-INDEX",
                     i + " is a signed index bounded above only; no " + i + " < 0 test", lines);
        }
    }
}

void capacity_first(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static const char* cap_field = R"([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)+)";
    static Regex cap_grow(
        std::string("^\\s*(?:(?P<a>") + cap_field +
                    ")\\s*(?:\\+=|\\*=|<<=)\\s*[^=]|\\+\\+\\s*(?P<b>" + cap_field +
                    ")|(?P<c>" + cap_field + ")\\s*\\+\\+)");
    static Regex cap_alloc(
        R"(\b(?:realloc|reallocarray|malloc|calloc|mallocarray|memcpy|memmove)\s*\()");
    static Regex cap_nullt(R"(==\s*(?:NULL|nullptr|0)\b|(?:NULL|nullptr)\s*==|!\s*[A-Za-z_])");
    static Regex cap_leave(R"(\b(?:return|goto|break|continue)\b)");
    static Regex cap_dead(
        R"(\b(?:err|errx|xo_err|xo_errx|abort|exit|panic|_errx|out_of_mem|AbortProgram|fatal|bsdar_errc)\s*\()");
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto m = cap_grow.search_match(lines[static_cast<std::size_t>(i)]);
        if (!m || !cap_grow.match_line(lines[static_cast<std::size_t>(i)])) continue;
        auto var = cap_norm(m->named("a"));
        if (var.empty()) var = cap_norm(m->named("b"));
        if (var.empty()) var = cap_norm(m->named("c"));
        std::optional<int> aidx;
        for (int j = i + 1; j < std::min(i + 6, static_cast<int>(lines.size())); ++j)
            if (cap_alloc.search(lines[static_cast<std::size_t>(j)])) {
                aidx = j;
                break;
            }
        if (!aidx) continue;
        auto window = cap_norm(join_range(lines, static_cast<std::size_t>(*aidx),
                                          static_cast<std::size_t>(*aidx + 4)));
        if (window.find(var) == std::string::npos) continue;
        std::optional<std::string> arm;
        for (int j = *aidx + 1; j < std::min(*aidx + 7, static_cast<int>(lines.size())); ++j) {
            if (cap_nullt.search(lines[static_cast<std::size_t>(j)]) &&
                cap_leave.search(join_range(lines, static_cast<std::size_t>(j),
                                            static_cast<std::size_t>(j + 4)))) {
                arm = join_range(lines, static_cast<std::size_t>(j),
                                 static_cast<std::size_t>(j + 5));
                break;
            }
        }
        if (!arm) continue;
        if (cap_dead.search(*arm)) continue;
        if (cap_norm(*arm).find(var) != std::string::npos) continue;
        lint_add(out, rel, func_of_line(funcs, i + 1), i + 1, "MEM-CAPACITY-FIRST",
                 var + " grown before the allocation that is supposed to earn it", lines);
    }
}

void mem_overlap(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto args = find_call_args(chunk[static_cast<std::size_t>(i)], "memcpy");
            if (!args || args->size() < 2) continue;
            auto dst = strip_ws((*args)[0]), src = strip_ws((*args)[1]);
            if (dst != src || !is_ident(dst) || seen.contains(i)) continue;
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "MEM-OVERLAP",
                     "memcpy(" + dst + ", " + dst + ", …) has overlapping source and destination",
                     lines);
        }
    }
}

void unchecked_alloc(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(
        R"((?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*|\s*\[[^\]]*\])*)\s*=\s*(?:\([^)]*\)\s*)?(?P<fn>malloc|calloc|realloc)\s*\()");
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = re.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            std::string stmt;
            for (int x = i; x < i + 4 && x < static_cast<int>(chunk.size()); ++x) {
                if (!stmt.empty()) stmt.push_back(' ');
                stmt += strip(chunk[static_cast<std::size_t>(x)]);
            }
            if (stmt.find("M_NOWAIT") != std::string::npos) continue;
            auto hit = unchecked_alloc_site(chunk, i, m->named("var"), m->named("fn"), false);
            if (!hit) continue;
            auto key = std::pair{hit->first, i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, "PTR-UNCHECKED-ALLOC",
                     hit->first + " used without a NULL check after " + m->named("fn") + "()",
                     lines);
        }
    }
}

void nowait_alloc(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(
        R"((?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*|\s*\[[^\]]*\])*)\s*=\s*(?:\([^)]*\)\s*)?(?P<fn>malloc|realloc)\s*\([^)]*M_NOWAIT)");
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = re.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto hit = unchecked_alloc_site(chunk, i, m->named("var"), m->named("fn"), true);
            if (!hit) continue;
            auto key = std::pair{hit->first, i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, "MEM-NOWAIT",
                     hit->first + " used without a NULL check after M_NOWAIT allocation", lines);
        }
    }
}

void noreturn_fatal(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex fatal(
        R"(^\s*(?:\(\s*void\s*\)\s*)?(?P<callee>exit|_exit|abort|err|errx)\s*\()");
    static Regex has_ret(R"(\breturn\b)");
    for (auto& fn : funcs) {
        if (fn.name == "main") continue;
        if (declared_noreturn(fn, lines)) continue;
        auto body_lines = split_lines(fn.body);
        if (has_ret.search(fn.body)) continue;
        auto last = last_body_stmt(body_lines);
        if (!last) continue;
        auto s = strip(*last);
        auto m = fatal.search_match(s);
        if (!m || !fatal.match_line(s)) continue;
        int rel_off = static_cast<int>(body_lines.size()) - 1;
        for (int i = static_cast<int>(body_lines.size()) - 1; i >= 0; --i) {
            auto t = strip(body_lines[static_cast<std::size_t>(i)]);
            if (!t.empty() && t != "{" && t != "}") {
                rel_off = i;
                break;
            }
        }
        lint_add(out, rel, fn.name, fn.span.first + rel_off, "FUNC-NORETURN",
                 fn.name + "() ends in " + m->named("callee") + "(), not declared noreturn", lines);
    }
}

void fmt_string(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& [fname, idx] : kFmtFn) {
                auto args = find_call_args(chunk[static_cast<std::size_t>(i)], fname);
                if (!args || static_cast<int>(args->size()) <= idx) continue;
                if (is_string_literal((*args)[static_cast<std::size_t>(idx)])) continue;
                auto key = std::pair{fname, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "FMT-STRING",
                         fname + "() format argument is not a string literal", lines);
            }
        }
    }
}

void memset_swap(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto args = find_call_args(chunk[static_cast<std::size_t>(i)], "memset");
            if (!args || args->size() < 3) continue;
            if (!is_zero_literal((*args)[2])) continue;
            auto size_arg = strip((*args)[1]);
            if (!(is_sizeof_expr(size_arg) || (is_ident(size_arg) && !is_tiny_literal(size_arg))))
                continue;
            if (seen.contains(i)) continue;
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "MEM-MEMSET-SWAP",
                     "memset() has size and fill-byte arguments swapped", lines);
        }
    }
}

void taut_bound(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex iff(R"(\bif\s*\(([^)]+)\))");
    static Regex lower(R"(\b([A-Za-z_]\w*)\s*(?:>=\s*0|>\s*-\s*1)\b)");
    static Regex upper(R"(\b([A-Za-z_]\w*)\s*(?:<|<=)\s*[^|&,)]+)");
    static Regex alow(R"(\b([A-Za-z_]\w*)\s*<\s*0\b)");
    static Regex ahigh(R"(\b([A-Za-z_]\w*)\s*>\s*[^|&,)]+)");
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = iff.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto cond = m->group(1);
            std::string cls, msg, var;
            if (cond.find("||") != std::string::npos) {
                std::unordered_set<std::string> lowers, uppers;
                for (auto& x : lower.finditer(cond)) lowers.insert(x.group(1));
                for (auto& x : upper.finditer(cond)) uppers.insert(x.group(1));
                std::vector<std::string> both;
                for (auto& v : lowers)
                    if (uppers.contains(v)) both.push_back(v);
                std::sort(both.begin(), both.end());
                if (!both.empty()) {
                    var = both[0];
                    cls = "INT-TAUTOLOGY";
                    msg = var + " lower/upper bounds use ||; nearly always true";
                }
            } else if (cond.find("&&") != std::string::npos) {
                std::unordered_set<std::string> lows, highs;
                for (auto& x : alow.finditer(cond)) lows.insert(x.group(1));
                for (auto& x : ahigh.finditer(cond)) highs.insert(x.group(1));
                std::vector<std::string> both;
                for (auto& v : lows)
                    if (highs.contains(v)) both.push_back(v);
                std::sort(both.begin(), both.end());
                if (!both.empty()) {
                    var = both[0];
                    cls = "INT-TAUTOLOGY";
                    msg = var + " contradictory bounds use &&; always false";
                }
            }
            if (cls.empty() || var.empty()) continue;
            auto key = std::pair{var, i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, cls, msg, lines);
        }
    }
}

void bool_as_bit(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first;
        std::unordered_set<int> seen;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (has_comparison(ln) && !binary_bitop_indices(ln).empty() && !seen.contains(i)) {
                seen.insert(i);
                lint_add(out, rel, fn.name, start + i, "INT-BOOL-AS-BIT",
                         "bitwise &/| applied to a comparison (boolean used as a bit)", lines);
            }
            ++i;
        }
    }
}

void wrap_alloc(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static const char* fnames[] = {"malloc", "realloc", "calloc", "reallocarray"};
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto* fname : fnames) {
                auto args = find_call_args(chunk[static_cast<std::size_t>(i)], fname);
                if (!args) continue;
                std::string name = fname;
                if (name == "calloc" && args->size() >= 2) {
                    bool any = false;
                    for (auto& a : *args)
                        if (has_wrap_mul(a)) any = true;
                    if (!any) continue;
                } else if (name == "reallocarray" && args->size() >= 3) {
                    bool any = false;
                    for (auto& a : *args)
                        if (has_wrap_mul(a)) any = true;
                    if (!any) continue;
                }
                std::vector<std::string> size_args;
                if (name == "malloc") {
                    if (args->empty()) continue;
                    size_args = {(*args)[0]};
                } else if (name == "realloc" && args->size() >= 2)
                    size_args = {(*args)[1]};
                else
                    for (auto& a : *args)
                        if (has_wrap_mul(a)) size_args.push_back(a);
                for (auto& sz : size_args) {
                    if (!has_wrap_mul(sz)) continue;
                    auto key = std::pair{name, i};
                    if (seen.contains(key)) continue;
                    seen.insert(key);
                    lint_add(out, rel, fn.name, start + i, "INT-WRAP-ALLOC",
                             name + "() size uses n*sizeof; may wrap before cast", lines);
                }
            }
        }
    }
}

void int_trunc(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex ncast(R"(\((?:unsigned\s+)?(?:char|short)\)\s*([A-Za-z_]\w*)\b)");
    static Regex ninit(R"(\b(?:signed\s+)?(?:char|short)\s+([A-Za-z_]\w*)\s*=\s*(?P<rhs>[^;,]+))");
    static Regex is_char(R"(\bchar\b)");
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        auto narrow = narrow_names(fn);
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto& ln = chunk[static_cast<std::size_t>(i)];
            for (auto& m : ninit.finditer(ln)) {
                auto name = m.group(1);
                auto rhs = strip(m.named("rhs"));
                auto narrow_ty = narrow.contains(name) ? narrow[name]
                                                       : (is_char.search(m.text) ? "char" : "short");
                if (!trunc_rhs_bad(rhs, narrow_ty)) continue;
                auto key = std::pair{name, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "INT-TRUNC",
                         name + " narrowed from a wider value", lines);
            }
            for (auto& m : ncast.finditer(ln)) {
                auto ident = m.group(1);
                if (kKw.contains(ident)) continue;
                auto key = std::pair{ident, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "INT-TRUNC",
                         "(" + ident + ") narrowed via cast from a wider value", lines);
            }
            for (auto& [name, narrow_ty] : narrow) {
                Regex as("\\b" + re_escape(name) + "\\s*=(?!=)\\s*(?P<rhs>[^;,]+)");
                auto m = as.search_match(ln);
                if (!m || ninit.search(ln)) continue;
                if (!trunc_rhs_bad(m->named("rhs"), narrow_ty)) continue;
                auto key = std::pair{name, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "INT-TRUNC",
                         name + " narrowed from a wider value", lines);
            }
        }
    }
}

void int_sign_conv(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex sc(
        R"(\b(?P<a>[A-Za-z_]\w*)\s*(?P<op><|>|<=|>=)\s*(?P<b>[A-Za-z_]\w*)\b)");
    for (auto& fn : funcs) {
        auto signed_n = signed_index_names(fn);
        auto unsigned_n = unsigned_names(fn);
        if (signed_n.empty() || unsigned_n.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& m : sc.finditer(chunk[static_cast<std::size_t>(i)])) {
                auto a = m.named("a"), b = m.named("b");
                std::optional<std::pair<std::string, std::string>> pair;
                if (signed_n.contains(a) && unsigned_n.contains(b)) pair = {a, b};
                else if (signed_n.contains(b) && unsigned_n.contains(a)) pair = {b, a};
                if (!pair) continue;
                auto key = std::pair{pair->first, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "INT-SIGN-CONV",
                         pair->first + " compared to unsigned " + pair->second +
                             "; negative indices look huge",
                         lines);
            }
        }
    }
}

void stack_escape(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex ret_addr(R"(\breturn\s*\(*\s*&\s*\(*\s*([A-Za-z_]\w*)\s*\)*\s*;)");
    static Regex ret_var(R"(\breturn\s+\(?\s*(?!\*&)([A-Za-z_]\w*)\s*\)?\s*;)");
    for (auto& fn : funcs) {
        auto [scalars, arrays] = locals_in_fn(fn);
        if (scalars.empty() && arrays.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto& ln = chunk[static_cast<std::size_t>(i)];
            if (auto m = ret_addr.search_match(ln)) {
                auto var = m->group(1);
                if (scalars.contains(var)) {
                    auto key = std::pair{var, i};
                    if (!seen.contains(key)) {
                        seen.insert(key);
                        lint_add(out, rel, fn.name, start + i, "MEM-STACK-ESCAPE",
                                 "return &" + var + "; escapes address of local " + var, lines);
                    }
                }
            }
            if (auto m = ret_var.search_match(ln)) {
                auto var = m->group(1);
                if (arrays.contains(var)) {
                    auto key = std::pair{var, i};
                    if (!seen.contains(key)) {
                        seen.insert(key);
                        lint_add(out, rel, fn.name, start + i, "MEM-STACK-ESCAPE",
                                 "return " + var + "; local array decays to dangling pointer",
                                 lines);
                    }
                }
            }
        }
    }
}

void unbounded_copy(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto locals_arr = local_char_arrays(fn);
        if (locals_arr.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& [fname, dst_idx] : kUnboundedCopyFn) {
                auto args = find_call_args(chunk[static_cast<std::size_t>(i)], fname);
                if (!args || static_cast<int>(args->size()) <= dst_idx) continue;
                auto dst = strip_ws((*args)[static_cast<std::size_t>(dst_idx)]);
                if (!locals_arr.contains(dst)) continue;
                auto key = std::pair{fname, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "STR-UNBOUNDED-COPY",
                         fname + "() into local " + dst + "[…] with no bound", lines);
            }
        }
    }
}

void str_sprintf(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto locals_arr = local_char_arrays(fn);
        if (locals_arr.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& [fname, dst_idx] : kSprintfFn) {
                auto args = find_call_args(chunk[static_cast<std::size_t>(i)], fname);
                if (!args || static_cast<int>(args->size()) <= dst_idx) continue;
                auto dst = strip_ws((*args)[static_cast<std::size_t>(dst_idx)]);
                if (!locals_arr.contains(dst)) continue;
                auto key = std::pair{fname, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "STR-SPRINTF",
                         fname + "() into local " + dst + "[…] with no bound", lines);
            }
        }
    }
}

void missing_return(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex has_ret(R"(\breturn\b)");
    for (auto& fn : funcs) {
        if (!is_value_returning(fn.return_type)) continue;
        auto body_lines = split_lines(fn.body);
        if (body_lines.empty()) continue;
        bool has_return = has_ret.search(fn.body);
        auto last = last_body_stmt(body_lines);
        if (!last) continue;
        if (has_return && missing_return_tail_ok(*last)) continue;
        if (!has_return || !missing_return_tail_ok(*last)) {
            int rel_off = static_cast<int>(body_lines.size()) - 1;
            for (int i = static_cast<int>(body_lines.size()) - 1; i >= 0; --i) {
                auto s = strip(body_lines[static_cast<std::size_t>(i)]);
                if (!s.empty() && s != "{" && s != "}") {
                    rel_off = i;
                    break;
                }
            }
            auto msg = !has_return ? fn.name + "() has no return statement"
                                   : fn.name + "() does not end in return or fatal exit";
            lint_add(out, rel, fn.name, fn.span.first + rel_off, "CTRL-MISSING-RETURN", msg, lines);
        }
    }
}

void fallthrough(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex case_label(R"(\b(?:case\b[^:]*:|default\s*:))");
    for (auto& fn : funcs) {
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (auto& [body, line_off] : switch_bodies(fn.body)) {
            auto cases = case_label.finditer(body);
            for (std::size_t idx = 0; idx < cases.size(); ++idx) {
                int arm_end = idx + 1 < cases.size() ? cases[idx + 1].spans[0].first
                                                     : static_cast<int>(body.size());
                auto arm = body.substr(static_cast<std::size_t>(cases[idx].spans[0].second),
                                       static_cast<std::size_t>(std::max(0, arm_end - cases[idx].spans[0].second)));
                if (!arm_falls_through(arm)) continue;
                int rel_off = count_nl(body.substr(0, static_cast<std::size_t>(std::max(0, cases[idx].spans[0].first))));
                if (seen.contains(rel_off)) continue;
                seen.insert(rel_off);
                lint_add(out, rel, fn.name, start + line_off + rel_off + 1, "CTRL-FALLTHROUGH",
                         "case falls through to the next label without annotation", lines);
            }
        }
    }
}

void dead_guard(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex dg(R"(\bif\s*\(\s*([A-Za-z_]\w*)\s*<\s*0(?:u|U)?\s*\))");
    for (auto& fn : funcs) {
        auto unsigned_n = unsigned_names(fn);
        if (unsigned_n.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = dg.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m || !unsigned_n.contains(m->group(1))) continue;
            auto key = std::pair{m->group(1), i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, "CTRL-DEAD-GUARD",
                     m->group(1) + " is unsigned; " + m->group(1) + " < 0 is always false", lines);
        }
    }
}

void empty_infinite(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(
        R"((?:\bfor\s*\(\s*;\s*;\s*\)\s*(?:;|\{\s*\})|\bwhile\s*\(\s*(?:1|true)\s*\)\s*(?:;|\{\s*\})|\bdo\s*(?:;|\{\s*\})\s*while\s*\(\s*(?:1|true)\s*\)))");
    for (auto& fn : funcs) {
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (auto& m : re.finditer(fn.body)) {
            int i = count_nl(fn.body.substr(0, static_cast<std::size_t>(std::max(0, m.spans[0].first))));
            if (seen.contains(i)) continue;
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "CTRL-EMPTY-INFINITE",
                     "empty infinite loop (no body, no exit)", lines);
        }
    }
}

void uninit_return(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex ret_var(R"(\breturn\s+\(?\s*(?!\*&)([A-Za-z_]\w*)\s*\)?\s*;)");
    for (auto& fn : funcs) {
        auto uninit = uninit_locals(fn);
        if (uninit.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = ret_var.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto var = m->group(1);
            if (!uninit.contains(var)) continue;
            if (Regex("\\b" + re_escape(var) + "\\s*=(?!=)").search(fn.body)) continue;
            auto key = std::pair{var, i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, "UNINIT-RETURN",
                     "return " + var + "; " + var + " is never initialised", lines);
        }
    }
}

void ptr_uninit(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (fn.kind == "POINTER") continue;
        auto ptrs = local_uninit_pointers(fn);
        if (ptrs.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (auto& p : ptrs) {
            auto v = re_escape(p);
            Regex star("\\*" + v + "\\b");
            Regex arrow("\\b" + v + "\\s*->");
            Regex as("\\b" + v + "\\s*=(?!=)");
            for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
                auto& ln = chunk[static_cast<std::size_t>(i)];
                if (ptr_decl_line(p, ln)) continue;
                if (!(star.search(ln) || arrow.search(ln))) continue;
                auto prior = join_range(chunk, 0, static_cast<std::size_t>(i));
                if (as.search(prior)) continue;
                auto key = std::pair{p, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "PTR-UNINIT",
                         p + " dereferenced before it is assigned", lines);
            }
        }
    }
}

void uninit_branch(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex br(R"(\b(?:if|while)\s*\(\s*(?:!\s*)?(?P<var>[A-Za-z_]\w*)\s*\))");
    for (auto& fn : funcs) {
        auto uninit = uninit_locals(fn);
        if (uninit.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = br.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto var = m->named("var");
            if (!uninit.contains(var)) continue;
            auto prior = join_range(chunk, 0, static_cast<std::size_t>(i));
            if (Regex("\\b" + re_escape(var) + "\\s*=(?!=)").search(prior)) continue;
            auto key = std::pair{var, i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, "UNINIT-BRANCH",
                     var + " used in branch before it is assigned", lines);
        }
    }
}

void off_by_one(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto arrays = local_char_array_sizes(fn);
        if (arrays.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto& ln = chunk[static_cast<std::size_t>(i)];
            if (auto args = find_call_args(ln, "strncpy"); args && args->size() >= 3) {
                auto dst = strip_ws((*args)[0]);
                if (arrays.contains(dst)) {
                    int msz = arrays[dst];
                    auto n_arg = strip_ws((*args)[2]);
                    if (n_arg == std::to_string(msz) || sizeof_dst(n_arg, dst)) {
                        auto key = std::pair{std::string("strncpy"), i};
                        if (!seen.contains(key)) {
                            seen.insert(key);
                            lint_add(out, rel, fn.name, start + i, "STR-OFF-BY-ONE",
                                     "strncpy(" + dst + ", …, " + std::to_string(msz) +
                                         ") leaves no room for NUL in " + dst + "[" +
                                         std::to_string(msz) + "]",
                                     lines);
                        }
                    }
                }
            }
            if (auto args = find_call_args(ln, "strcpy"); args && args->size() >= 2) {
                auto dst = strip_ws((*args)[0]);
                if (arrays.contains(dst)) {
                    auto lit_len = string_lit_len((*args)[1]);
                    if (lit_len && *lit_len + 1 > arrays[dst]) {
                        auto key = std::pair{std::string("strcpy"), i};
                        if (!seen.contains(key)) {
                            seen.insert(key);
                            lint_add(out, rel, fn.name, start + i, "STR-OFF-BY-ONE",
                                     "strcpy(" + dst + ", …) needs " + std::to_string(*lit_len + 1) +
                                         " bytes in " + dst + "[" + std::to_string(arrays[dst]) + "]",
                                     lines);
                        }
                    }
                }
            }
        }
    }
}

void str_missing_nul(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first;
        std::unordered_set<int> seen;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            auto args = find_call_args(ln, "memcpy");
            if (args && args->size() >= 3) {
                auto lit_len = string_lit_len((*args)[1]);
                auto n = lit_len ? parse_int_literal((*args)[2]) : std::nullopt;
                if (lit_len && n && *n == *lit_len && !seen.contains(i)) {
                    seen.insert(i);
                    lint_add(out, rel, fn.name, start + i, "STR-MISSING-NUL",
                             "memcpy(…, string, " + std::to_string(*n) + ") omits the terminator",
                             lines);
                }
            }
            ++i;
        }
    }
}

void sibling_asymmetry(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    std::vector<const FunctionInfo*> eligible;
    for (auto& f : funcs)
        if (f.kind == "SCALAR" || f.kind == "VOID") eligible.push_back(&f);
    std::map<std::string, std::vector<std::pair<const FunctionInfo*, std::string>>> by_name;
    for (auto* fn : eligible)
        for (auto& [name, typ] : integer_param_names(*fn)) by_name[name].emplace_back(fn, typ);
    std::set<std::pair<std::string, std::string>> reported;
    for (auto& [pname, entries] : by_name) {
        if (entries.size() < 2) continue;
        std::vector<std::pair<const FunctionInfo*, std::string>> guarders;
        for (auto& [fn, typ] : entries)
            if (param_guarded(pname, split_lines(fn->body))) guarders.emplace_back(fn, typ);
        if (guarders.empty()) continue;
        for (auto& [fn, typ] : entries) {
            if (has_if_operand(pname, fn->body)) continue;
            if (!subscript_uses(pname, fn->body)) continue;
            bool similar = false;
            for (auto& [_, gt] : guarders)
                if (similar_param_type(typ, gt)) similar = true;
            if (!similar) continue;
            auto key = std::pair{fn->name, pname};
            if (reported.contains(key)) continue;
            reported.insert(key);
            int line = first_subscript_line(*fn, pname, lines);
            lint_add(out, rel, fn->name, line, "CTRL-SIBLING-ASYMMETRY",
                     pname + " guarded in a sibling but used as subscript here without a test",
                     lines);
        }
    }
}

void ignored_error(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"(^\s*(?:malloc|calloc|realloc|fopen|open)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, re, "API-IGNORED-ERROR",
                   "allocation or open result is discarded", out);
}

void scanf_unchecked(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(
        R"(^\s*(?:scanf|sscanf|fscanf|wscanf|swscanf|fwscanf)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, re, "API-SCANF-UNCHECKED",
                   "scanf-family return is discarded", out);
}

void fmt_percent_n(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            for (auto& [fname, idx] : kFmtFn) {
                auto args = find_call_args(ln, fname);
                if (!args || static_cast<int>(args->size()) <= idx) continue;
                if (!literal_has_percent_n((*args)[static_cast<std::size_t>(idx)])) continue;
                auto key = std::pair{fname, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "FMT-PERCENT-N",
                         fname + "() format uses %n write-back", lines);
            }
            ++i;
        }
    }
}

void int_atoi(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto parsed = atoi_vars(fn.body);
        if (parsed.empty()) continue;
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        std::vector<std::string> vars(parsed.begin(), parsed.end());
        std::sort(vars.begin(), vars.end());
        for (auto& var : vars) {
            if (has_bounds_guard(var, fn.body)) continue;
            for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                if (!atoi_index_or_size_use(var, body_lines[static_cast<std::size_t>(i)])) continue;
                lint_add(out, rel, fn.name, start + i, "INT-ATOI",
                         var + " from atoi/atol/atoll used as index or allocation size without a range check",
                         lines);
                break;
            }
        }
    }
}

void enum_hole(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex case_ident(R"(\bcase\s+([A-Za-z_]\w*)\s*:)");
    static Regex defl(R"(\bdefault\s*:)");
    auto text = join_all(lines);
    auto tags = named_enum_members(text);
    if (tags.empty()) return;
    for (auto& fn : funcs) {
        auto vars = enum_vars_in_fn(fn, tags);
        if (vars.empty()) continue;
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (auto& sw : ident_switch_bodies(fn.body)) {
            auto it = vars.find(sw.var);
            if (it == vars.end()) continue;
            if (defl.search(sw.body)) continue;
            std::unordered_set<std::string> covered;
            for (auto& cm : case_ident.finditer(sw.body)) covered.insert(cm.group(1));
            std::vector<std::string> missing;
            for (auto& n : tags[it->second])
                if (!covered.contains(n)) missing.push_back(n);
            if (missing.empty()) continue;
            auto key = std::pair{sw.var, sw.line_off};
            if (seen.contains(key)) continue;
            seen.insert(key);
            std::string miss;
            for (std::size_t k = 0; k < missing.size(); ++k) {
                if (k) miss += ", ";
                miss += missing[k];
            }
            lint_add(out, rel, fn.name, start + sw.line_off, "INT-ENUM-HOLE",
                     "switch (" + sw.var + ") on enum " + it->second + " misses " + miss +
                         " and has no default",
                     lines);
        }
    }
}

void fd_leak(const std::vector<std::string>& lines, std::string_view rel,
             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex fd_open(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?P<fn>fopen|open)\s*\()");
    static Regex fd_close(R"(\b(?P<fn>fclose|close)\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*\))");
    static Regex ret(R"(^\s*return\b)");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::map<std::string, std::vector<int>> opens, closes;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : fd_open.finditer(body_lines[static_cast<std::size_t>(i)]))
                opens[m.named("var")].push_back(i);
            for (auto& m : fd_close.finditer(body_lines[static_cast<std::size_t>(i)]))
                closes[m.named("var")].push_back(i);
        }
        std::vector<int> returns;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (ret.match_line(body_lines[static_cast<std::size_t>(i)])) returns.push_back(i);
        if (returns.size() < 2) continue;
        for (auto& [var, open_hits] : opens) {
            auto& close_hits = closes[var];
            if (close_hits.empty()) continue;
            std::vector<int> held_returns;
            for (int r : returns)
                if (max_before(open_hits, r) > max_before(close_hits, r)) held_returns.push_back(r);
            if (!held_returns.empty() && held_returns.size() < returns.size())
                lint_add(out, rel, fn.name, fn.span.first + held_returns[0], "RES-FD-LEAK",
                         var + " open on " + std::to_string(open_hits.size()) +
                             " path(s) but closed before only " +
                             std::to_string(returns.size() - held_returns.size()) + " of " +
                             std::to_string(returns.size()) + " returns",
                         lines);
        }
    }
}

void popen_leak(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex po(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?popen\s*\()");
    static Regex pc(R"(\bpclose\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*\))");
    static Regex ret(R"(^\s*return\b)");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::map<std::string, std::vector<int>> opens, closes;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : po.finditer(body_lines[static_cast<std::size_t>(i)]))
                opens[m.named("var")].push_back(i);
            for (auto& m : pc.finditer(body_lines[static_cast<std::size_t>(i)]))
                closes[m.named("var")].push_back(i);
        }
        if (opens.empty()) continue;
        std::vector<int> returns;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (ret.match_line(body_lines[static_cast<std::size_t>(i)])) returns.push_back(i);
        for (auto& [var, open_hits] : opens) {
            auto& close_hits = closes[var];
            bool leaked = false;
            int leak_ep = open_hits[0];
            for (int r : returns) {
                if (max_before(open_hits, r) > max_before(close_hits, r)) {
                    leaked = true;
                    leak_ep = r;
                    break;
                }
            }
            if (!leaked) {
                int last_open = *std::max_element(open_hits.begin(), open_hits.end());
                int last_close = close_hits.empty()
                                     ? -1
                                     : *std::max_element(close_hits.begin(), close_hits.end());
                if (last_open > last_close) {
                    leaked = true;
                    leak_ep = last_open;
                }
            }
            if (leaked)
                lint_add(out, rel, fn.name, fn.span.first + leak_ep, "API-POPEN",
                         var + " from popen() not pclose()'d on all exits", lines);
        }
    }
}

void mem_leak(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex alloc(
        R"((?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*|\s*\[[^\]]*\])*)\s*=\s*(?:\([^)]*\)\s*)?(?P<fn>malloc|calloc|realloc)\s*\()");
    static Regex free_call(
        R"(\b(?:free|kfree|ck_free)\s*\(\s*(?:\([^)]*\)\s*)*(?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\))");
    static Regex ret(R"(^\s*return\b)");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::map<std::string, std::vector<int>> allocs, frees;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : alloc.finditer(body_lines[static_cast<std::size_t>(i)]))
                allocs[cap_norm(m.named("var"))].push_back(i);
            for (auto& m : free_call.finditer(body_lines[static_cast<std::size_t>(i)]))
                frees[cap_norm(m.named("var"))].push_back(i);
        }
        std::vector<int> returns;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (ret.match_line(body_lines[static_cast<std::size_t>(i)])) returns.push_back(i);
        if (returns.size() < 2) continue;
        for (auto& [var, alloc_hits] : allocs) {
            auto& free_hits = frees[var];
            if (free_hits.empty()) continue;
            std::vector<int> held_returns;
            for (int r : returns)
                if (max_before(alloc_hits, r) > max_before(free_hits, r)) held_returns.push_back(r);
            if (!held_returns.empty() && held_returns.size() < returns.size())
                lint_add(out, rel, fn.name, fn.span.first + held_returns[0], "MEM-LEAK",
                         var + " allocated on " + std::to_string(alloc_hits.size()) +
                             " path(s) but freed before only " +
                             std::to_string(returns.size() - held_returns.size()) + " of " +
                             std::to_string(returns.size()) + " returns",
                         lines);
        }
    }
}

void mismatched_free(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex c_alloc(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:malloc|calloc|realloc)\s*\()");
    static Regex n_assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*new\s+(?P<rest>[^;]+))");
    static Regex del(R"(\bdelete\s*(\[\s*\])?\s*(?P<var>[A-Za-z_]\w*)\b)");
    static Regex free_call(
        R"(\b(?:free|kfree|ck_free)\s*\(\s*(?:\([^)]*\)\s*)*(?P<var>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\))");
    for (auto& fn : funcs) {
        if (fn.kind == "POINTER") continue;
        auto body_lines = split_lines(fn.body);
        std::map<std::string, std::string> alloc_family;
        for (auto& ln : body_lines) {
            if (auto m = c_alloc.search_match(ln)) alloc_family[m->named("var")] = "c";
            if (auto m = n_assign.search_match(ln)) alloc_family[m->named("var")] = "cpp";
        }
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            if (auto m = del.search_match(ln)) {
                auto var = m->named("var");
                if (alloc_family[var] != "c") continue;
                auto key = std::pair{var, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, fn.span.first + i, "MEM-MISMATCHED-FREE",
                         var + " allocated with malloc family but freed with delete", lines);
            }
            if (auto m = free_call.search_match(ln)) {
                auto var = cap_norm(m->named("var"));
                if (alloc_family[var] != "cpp") continue;
                auto key = std::pair{var, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, fn.span.first + i, "MEM-MISMATCHED-FREE",
                         var + " allocated with new but freed with free", lines);
            }
        }
    }
}

void str_null_arg(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (fn.kind != "POINTER") continue;
        auto ptr_params = pointer_param_names(fn);
        if (ptr_params.empty()) continue;
        std::unordered_set<std::string> unchecked;
        for (auto& p : ptr_params)
            if (!param_null_tested(p, fn.body)) unchecked.insert(p);
        if (unchecked.empty()) continue;
        int start = fn.span.first;
        bool reported = false;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (reported) break;
            for (auto& [callee, arg_idxs] : kStrNullFn) {
                auto args = find_call_args(ln, callee);
                if (!args) continue;
                for (int idx : arg_idxs) {
                    if (idx >= static_cast<int>(args->size())) continue;
                    auto arg = strip((*args)[static_cast<std::size_t>(idx)]);
                    if (!is_ident(arg) || !unchecked.contains(arg)) continue;
                    lint_add(out, rel, fn.name, start + i, "STR-NULL-ARG",
                             callee + "() called with unchecked pointer parameter " + arg, lines);
                    reported = true;
                    break;
                }
                if (reported) break;
            }
            ++i;
        }
    }
}

void ptr_arith(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (fn.kind != "POINTER") continue;
        auto ptrs = pointer_param_names(fn);
        auto ints = integer_param_names(fn);
        if (ptrs.empty() || ints.empty()) continue;
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        bool reported = false;
        for (auto& p : ptrs) {
            if (reported) break;
            for (auto& [n, _] : ints) {
                if (!ptr_index_arith(p, n, fn.body)) continue;
                if (param_if_guard(p, fn.body) && param_if_guard(n, fn.body)) continue;
                auto pe = re_escape(p), ne = re_escape(n);
                Regex a("\\*\\s*\\(\\s*" + pe + "\\s*\\+\\s*" + ne + "\\s*\\)");
                Regex b("\\b" + pe + "\\s*\\[\\s*" + ne + "\\s*\\]");
                for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                    auto& ln = body_lines[static_cast<std::size_t>(i)];
                    if (!(a.search(ln) || b.search(ln))) continue;
                    lint_add(out, rel, fn.name, start + i, "MEM-PTR-ARITH",
                             p + "[" + n + "] or *(" + p + "+" + n + ") without guard on " + p +
                                 " or " + n,
                             lines);
                    reported = true;
                    break;
                }
                if (reported) break;
            }
        }
    }
}

void float_ub(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"(/\s*0\.0(?:[fF])?\b|/\s*0\.(?![0-9]))");
    for (auto& fn : funcs) {
        if (fn.kind != "SCALAR" && fn.kind != "VOID") continue;
        int start = fn.span.first;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (re.search(ln))
                lint_add(out, rel, fn.name, start + i, "FLOAT-UB",
                         "division by floating zero literal", lines);
            ++i;
        }
    }
}

void vla_size(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(
        R"re((?m)^\s*(?P<static>static\s+)?(?:const\s+|volatile\s+)?(?:unsigned\s+)?(?:char|short|int|long|float|double|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>[^\]]+)\s*\]\s*;)re",
        true);
    for (auto& fn : funcs) {
        std::unordered_set<std::string> params;
        for (auto& [typ, name] : fn.params)
            if (!name.empty()) params.insert(name);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (auto& m : re.finditer(fn.body)) {
            if (!m.named("static").empty()) continue;
            auto name = m.named("name");
            if (params.contains(name) || kKw.contains(name)) continue;
            auto size = strip(m.named("size"));
            if (parse_int_literal(size)) continue;
            int rel_off = count_nl(fn.body.substr(0, static_cast<std::size_t>(std::max(0, m.spans[0].first))));
            auto key = std::pair{name, rel_off};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + rel_off, "MEM-VLA-SIZE",
                     name + "[" + size + "] is a variable-length local array", lines);
        }
    }
}

void mem_alloca(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            for (auto* fname : {"alloca", "__builtin_alloca"}) {
                auto args = find_call_args(ln, fname);
                if (!args) continue;
                auto size = strip((*args)[0]);
                if (parse_int_literal(size)) continue;
                lint_add(out, rel, fn.name, start + i, "MEM-ALLOCA",
                         std::string(fname) + "(" + size + ") size is not a constant", lines);
                break;
            }
            ++i;
        }
    }
}

void mem_sizeof_ptr(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex ident(R"(^sizeof\s*\(\s*([A-Za-z_]\w*)\s*\)$)");
    for (auto& fn : funcs) {
        auto ptrs = pointer_locals(fn);
        if (ptrs.empty()) continue;
        int start = fn.span.first;
        std::unordered_set<int> seen;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            for (auto* fname : {"malloc", "realloc", "calloc"}) {
                auto args = find_call_args(ln, fname);
                if (!args || args->empty()) continue;
                std::vector<std::string> size_args;
                std::string name = fname;
                if (name == "malloc") size_args = {(*args)[0]};
                else if (name == "realloc" && args->size() >= 2)
                    size_args = {(*args)[1]};
                else if (name == "calloc" && args->size() >= 2)
                    size_args = {(*args)[0], (*args)[1]};
                else
                    continue;
                for (auto& size : size_args) {
                    auto t = unwrap_parens(size);
                    auto m = ident.search_match(t);
                    if (!m || !fullmatch(ident, t) || !ptrs.contains(m->group(1))) continue;
                    if (seen.contains(i)) continue;
                    seen.insert(i);
                    lint_add(out, rel, fn.name, start + i, "MEM-SIZEOF-PTR",
                             name + "() size is sizeof(" + m->group(1) +
                                 "), not sizeof(*p) or the pointee type",
                             lines);
                    break;
                }
            }
            ++i;
        }
    }
}

void mem_flex_array(std::string_view stripped, const std::vector<std::string>& lines,
                    std::string_view rel, const std::vector<FunctionInfo>& funcs,
                    std::vector<Finding>& out) {
    static Regex fam_ptr(R"(\bstruct\s+(?P<tag>[A-Za-z_]\w*)\s*\*+\s*(?P<name>[A-Za-z_]\w*))");
    static Regex kw(R"(\b(?:const|volatile|struct|class)\b)");
    auto tags = flex_array_tags(stripped);
    if (tags.empty()) return;
    for (auto& fn : funcs) {
        std::map<std::string, std::string> ptrs;
        for (auto& [typ, name] : fn.params) {
            if (name.empty() || typ.find('*') == std::string::npos) continue;
            auto t = re_sub(kw, " ", typ);
            for (char& c : t)
                if (c == '*') c = ' ';
            auto tag = squeeze_ws(t);
            if (tags.contains(tag)) ptrs[name] = tag;
        }
        for (auto& m : fam_ptr.finditer(fn.body))
            if (tags.contains(m.named("tag"))) ptrs[m.named("name")] = m.named("tag");
        int start = fn.span.first;
        std::unordered_set<int> seen;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            for (auto* fname : {"malloc", "realloc", "calloc"}) {
                auto args = find_call_args(ln, fname);
                if (!args) continue;
                std::string size, name = fname;
                if (name == "malloc") size = (*args)[0];
                else if (name == "realloc" && args->size() >= 2)
                    size = (*args)[1];
                else if (name == "calloc" && args->size() >= 2)
                    size = (*args)[1];
                else
                    continue;
                auto [kind, ident] = bare_sizeof(size);
                std::string tag;
                if (kind && *kind == "struct" && ident && tags.contains(*ident)) tag = *ident;
                else if (kind && *kind == "star" && ident && ptrs.contains(*ident)) tag = ptrs[*ident];
                if (tag.empty() || seen.contains(i)) continue;
                seen.insert(i);
                lint_add(out, rel, fn.name, start + i, "MEM-FLEX-ARRAY",
                         name + "() sizes struct " + tag +
                             " at the header; flexible array needs extra bytes",
                         lines);
                break;
            }
            ++i;
        }
    }
}

void infoleak_pad(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex memcpy_addr(R"(memcpy\s*\([^,]+,\s*&\s*(?P<var>[A-Za-z_]\w*)\s*,\s*sizeof)");
    for (auto& fn : funcs) {
        auto structs = local_structs(fn);
        if (structs.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = memcpy_addr.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto var = m->named("var");
            if (!structs.contains(var) || local_zeroed(fn.body, var)) continue;
            auto key = std::pair{var, i};
            if (seen.contains(key)) continue;
            seen.insert(key);
            lint_add(out, rel, fn.name, start + i, "INFOLEAK-PAD",
                     "memcpy copies local struct " + var + " without zeroing padding first", lines);
        }
    }
}

void intent_mismatch(const std::vector<std::string>& orig_lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex intent_inc(
        R"((?i)returns\s+\w+\s*\+\s*1|return\s+\w+\s*\+\s*1|result\s+is\s+\w+\s*\+\s*1|adds\s+one|increment)");
    static Regex bare(R"(\breturn\s+(?!\*&)([A-Za-z_]\w*)\s*;)");
    for (auto& fn : funcs) {
        if (fn.kind != "SCALAR" && fn.kind != "VOID") continue;
        int fn_line = fn.line ? fn.line : fn.span.first;
        auto preamble = comment_preamble(orig_lines, std::max(0, fn_line - 1));
        if (preamble.empty() || !intent_inc.search(preamble)) continue;
        int start = fn.span.first;
        bool seen = false;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            auto m = bare.search_match(ln);
            if (!m) {
                ++i;
                continue;
            }
            auto var = m->group(1);
            if (kKw.contains(var) || has_increment_on(var, fn.body) || seen) {
                ++i;
                continue;
            }
            seen = true;
            lint_add(out, rel, fn.name, start + i, "INTENT",
                     "comment claims increment but " + fn.name + "() returns " + var + " unchanged",
                     orig_lines);
            ++i;
        }
    }
}

void api_precondition(const std::vector<std::string>& orig_lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex ifg(R"(\bif\s*\(\s*(\w+))");
    for (auto& fn : funcs) {
        if (fn.kind != "SCALAR") continue;
        int fn_line = fn.line ? fn.line : fn.span.first;
        auto preamble = comment_preamble(orig_lines, std::max(0, fn_line - 1));
        if (preamble.empty()) continue;
        auto var = precond_var_from_preamble(preamble);
        if (!var) continue;
        bool guarded = false;
        for (auto& gm : ifg.finditer(fn.body))
            if (gm.group(1) == *var) guarded = true;
        if (guarded) continue;
        lint_add(out, rel, fn.name, fn_line, "API-PRECONDITION",
                 "comment requires " + *var + ">0 but " + fn.name + "() never guards " + *var,
                 orig_lines);
    }
}

void trust_unvalidated_input(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto parsed = parsed_input_vars(fn.body);
        if (parsed.empty()) continue;
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        std::vector<std::string> vars(parsed.begin(), parsed.end());
        std::sort(vars.begin(), vars.end());
        for (auto& var : vars) {
            if (has_if_guard(var, fn.body)) continue;
            for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                if (!unvalidated_index_use(var, body_lines[static_cast<std::size_t>(i)])) continue;
                lint_add(out, rel, fn.name, start + i, "TRUST-UNVALIDATED-INPUT",
                         var + " from parsed input used as index or divisor without an if (" + var +
                             " …) guard",
                         lines);
                break;
            }
        }
    }
}

void crypto_srand(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"(\b(?:srand|srandom)\s*\(\s*(?:\([^)]*\)\s*)*time\s*\()");
    for (auto& fn : funcs) {
        int start = fn.span.first;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (re.search(ln))
                lint_add(out, rel, fn.name, start + i, "CRYPTO-SRAND",
                         "srand(time()) is not a CSPRNG seed", lines);
            ++i;
        }
    }
}

void mem_realloc_zero(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto old = realloc_zero_old_ptr(body_lines[static_cast<std::size_t>(i)]);
            if (!old) continue;
            for (int j = i + 1; j < static_cast<int>(body_lines.size()); ++j) {
                auto& later = body_lines[static_cast<std::size_t>(j)];
                if (Regex("\\b" + re_escape(*old) + "\\s*=").search(later)) break;
                if (stale_ptr_use(*old, later)) {
                    lint_add(out, rel, fn.name, start + j, "MEM-REALLOC-ZERO",
                             *old + " used after realloc(" + *old + ", 0)", lines);
                    break;
                }
            }
        }
    }
}

void crypto_misuse(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex crypto_fn(R"((?i)(?:key|crypto))");
    static Regex key_rand(R"(\bkey\s*\[[^\]]+\]\s*=\s*(?:rand|random)\s*\()");
    static Regex rand_call(R"(\b(?:rand|random)\s*\()");
    static Regex rand_local(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:rand|random)\s*\()");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        bool reported = false;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!key_rand.search(body_lines[static_cast<std::size_t>(i)])) continue;
            lint_add(out, rel, fn.name, start + i, "CRYPTO-MISUSE", "rand() used to fill key[]",
                     lines);
            reported = true;
            break;
        }
        if (reported) continue;
        if (!crypto_fn.search(fn.name)) continue;
        auto ptrs = pointer_param_names(fn);
        if (ptrs.empty()) continue;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!rand_call.search(body_lines[static_cast<std::size_t>(i)])) continue;
            for (auto& p : ptrs) {
                if (!ptr_rand_store(p, body_lines[static_cast<std::size_t>(i)])) continue;
                lint_add(out, rel, fn.name, start + i, "CRYPTO-MISUSE",
                         "rand() assigned into pointer parameter " + p, lines);
                reported = true;
                break;
            }
            if (reported) break;
        }
        if (reported) continue;
        std::unordered_set<std::string> rand_locals;
        for (auto& ln : body_lines)
            for (auto& m : rand_local.finditer(ln)) rand_locals.insert(m.named("var"));
        if (rand_locals.empty()) continue;
        for (auto& p : ptrs) {
            for (auto& local : rand_locals) {
                if (!ptr_local_store(p, local, fn.body)) continue;
                for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                    auto& ln = body_lines[static_cast<std::size_t>(i)];
                    if (ln.find(local) == std::string::npos || ln.find(p) == std::string::npos)
                        continue;
                    if (!ptr_local_store(p, local, ln)) continue;
                    lint_add(out, rel, fn.name, start + i, "CRYPTO-MISUSE",
                             "rand() in " + local + " copied into " + p, lines);
                    reported = true;
                    break;
                }
                if (reported) break;
            }
            if (reported) break;
        }
    }
}

void lock_order(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex call(R"(\b(\w*lock)\s*\(\s*&?([A-Za-z_]\w*))");
    std::map<std::string, std::pair<std::string, std::string>> orders;
    std::map<std::string, int> first_line;
    for (auto& fn : funcs) {
        auto pair = first_two_locks(fn.body);
        if (!pair) continue;
        orders[fn.name] = *pair;
        int start = fn.span.first;
        std::unordered_set<std::string> seen;
        int i = 0;
        for (auto& ln : split_lines(fn.body)) {
            for (auto& m : call.finditer(ln)) {
                if (to_lower_copy(m.group(1)).find("unlock") != std::string::npos) continue;
                auto name = m.group(2);
                if (seen.contains(name)) continue;
                seen.insert(name);
                first_line[fn.name] = start + i;
                break;
            }
            if (first_line.contains(fn.name)) break;
            ++i;
        }
    }
    std::unordered_set<std::string> reported;
    std::vector<std::string> names;
    for (auto& [n, _] : orders) names.push_back(n);
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto [x1, y1] = orders[names[i]];
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            auto [x2, y2] = orders[names[j]];
            std::set<std::string> a{x1, y1}, b{x2, y2};
            if (a != b) continue;
            if (x1 == x2 && y1 == y2) continue;
            for (auto* fn_name : {&names[i], &names[j]}) {
                if (reported.contains(*fn_name)) continue;
                reported.insert(*fn_name);
                auto [x_ord, y_ord] = orders[*fn_name];
                int line = first_line.contains(*fn_name)
                               ? first_line[*fn_name]
                               : (funcs.empty() ? 1 : funcs[0].span.first);
                lint_add(out, rel, *fn_name, line, "LOCK-ORDER",
                         "locks " + x_ord + " then " + y_ord +
                             "; another function locks them in the opposite order",
                         lines);
            }
        }
    }
}

void conc_toctou(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex check(R"(\b(?:access|stat|lstat)\s*\()");
    static Regex use(R"(\b(?:open|fopen|unlink|remove|chmod)\s*\()");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::optional<int> check_at, use_at;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!check_at && check.search(body_lines[static_cast<std::size_t>(i)])) check_at = i;
            else if (check_at && i > *check_at && use.search(body_lines[static_cast<std::size_t>(i)])) {
                use_at = i;
                break;
            }
        }
        if (!check_at || !use_at) continue;
        lint_add(out, rel, fn.name, fn.span.first + *use_at, "CONC-TOCTOU",
                 "path checked then opened or modified without holding authority across the gap",
                 lines);
    }
}

void conc_atomicity(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    auto globals_ = file_scope_int_globals(lines);
    if (globals_.empty()) return;
    std::set<std::pair<std::string, std::string>> reported;
    for (auto& fn : funcs) {
        if (fn_has_lock_call(fn.body)) continue;
        for (auto& g : globals_) {
            if (!global_rmw(fn.body, g)) continue;
            auto key = std::pair{fn.name, g};
            if (reported.contains(key)) continue;
            reported.insert(key);
            auto v = re_escape(g);
            int line = fn.span.first;
            int i = 0;
            for (auto& ln : split_lines(fn.body)) {
                if (Regex("\\b" + v + "\\s*=\\s*" + v + "\\s*\\+").search(ln) ||
                    Regex("\\b" + v + "\\+\\+").search(ln) ||
                    Regex("\\+\\+\\s*" + v + "\\b").search(ln) ||
                    Regex("\\b" + v + "\\s*\\+=").search(ln)) {
                    line = fn.span.first + i;
                    break;
                }
                ++i;
            }
            lint_add(out, rel, fn.name, line, "CONC-ATOMICITY",
                     "global '" + g + "' incremented without mutex protection", lines);
        }
    }
}

void wcs_unbounded(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto locals_arr = local_wchar_arrays(fn);
        if (locals_arr.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& [fname, dst_idx] : kWcsUnboundedFn) {
                auto args = find_call_args(chunk[static_cast<std::size_t>(i)], fname);
                if (!args || static_cast<int>(args->size()) <= dst_idx) continue;
                auto dst = strip_ws((*args)[static_cast<std::size_t>(dst_idx)]);
                if (!locals_arr.contains(dst)) continue;
                auto key = std::pair{fname, i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "STR-WCSCPY",
                         fname + "() into local " + dst + "[…] with no bound", lines);
            }
        }
    }
}

void int_clz_zero(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static const char* clz_fns[] = {"__builtin_clzll", "__builtin_ctzll", "__builtin_clz",
                                    "__builtin_ctz"};
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto* fname : clz_fns) {
                auto args = find_call_args(chunk[static_cast<std::size_t>(i)], fname);
                if (!args || args->empty()) continue;
                auto arg = strip((*args)[0]);
                auto lit = parse_int_literal(arg);
                bool bad = false;
                if (lit && *lit == 0) bad = true;
                else if (is_ident(arg)) {
                    auto before = join_range(chunk, 0, static_cast<std::size_t>(i + 1));
                    bad = !nonzero_guard_for(arg, before);
                }
                if (!bad || seen.contains(i)) continue;
                seen.insert(i);
                lint_add(out, rel, fn.name, start + i, "INT-CLZ-ZERO",
                         std::string(fname) + "() on 0 is undefined", lines);
                break;
            }
        }
    }
}

void str_strncpy_nul(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto arrays = local_char_array_sizes(fn);
        if (arrays.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto args = find_call_args(chunk[static_cast<std::size_t>(i)], "strncpy");
            if (!args || args->empty()) continue;
            auto dst = strip_ws((*args)[0]);
            if (!arrays.contains(dst)) continue;
            auto following = join_range(chunk, static_cast<std::size_t>(i + 1), chunk.size());
            if (explicit_nul_store_for(dst, following) || seen.contains(i)) continue;
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "STR-STRNCPY-NUL",
                     "strncpy(" + dst + ", …) with no following explicit NUL store", lines);
        }
    }
}

void str_snprintf(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto arrays = local_char_array_sizes(fn);
        if (arrays.empty()) continue;
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto args = find_call_args(chunk[static_cast<std::size_t>(i)], "snprintf");
            if (!args || args->size() < 2) continue;
            auto dst = strip_ws((*args)[0]);
            if (!arrays.contains(dst)) continue;
            auto n_arg = strip((*args)[1]);
            if (sizeof_dst(n_arg, dst)) continue;
            auto n = parse_int_literal(n_arg);
            int msz = arrays[dst];
            if (!n || *n <= msz || seen.contains(i)) continue;
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "STR-SNPRINTF",
                     "snprintf(" + dst + ", " + std::to_string(*n) + ", …) exceeds local " + dst +
                         "[" + std::to_string(msz) + "]",
                     lines);
        }
    }
}

void mem_bcopy(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::unordered_set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto args = find_call_args(chunk[static_cast<std::size_t>(i)], "bcopy");
            if (!args || args->size() < 2) continue;
            auto src = strip_ws((*args)[0]);
            auto dst = strip_ws((*args)[1]);
            if (src != dst || !is_ident(src) || seen.contains(i)) continue;
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "MEM-BCOPY",
                     "bcopy(" + src + ", " + src + ", …) has overlapping source and destination",
                     lines);
        }
    }
}

void api_gets(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"((?<![A-Za-z0-9_])gets\s*\()");
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (re.search(ln))
                lint_add(out, rel, fn.name, start + i, "API-GETS",
                         "gets() is unbounded (removed from C11)", lines);
            ++i;
        }
    }
}

void api_strtok(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"((?<![A-Za-z0-9_])strtok\s*\()");
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (re.search(ln))
                lint_add(out, rel, fn.name, start + i, "API-STRTOK-REENTRANT",
                         "strtok() is not reentrant; strtok_r/strtok_s keep no static cursor",
                         lines);
            ++i;
        }
    }
}

void api_mkstemp(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        std::set<std::pair<std::string, int>> seen;
        for (auto& ln : split_lines(fn.body)) {
            for (auto* fname : {"mkstemp", "mkstemps"}) {
                auto args = find_call_args(ln, fname);
                if (!args || args->empty() || !is_string_literal((*args)[0])) continue;
                auto key = std::pair{std::string(fname), i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "API-MKSTEMP",
                         std::string(fname) + "() template is a string literal (not writable)",
                         lines);
            }
            ++i;
        }
    }
}

void api_tmpnam(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"((?<![A-Za-z0-9_])(?:tmpnam_r|tempnam|tmpnam)\s*\()");
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (re.search(ln))
                lint_add(out, rel, fn.name, start + i, "API-TMPNAM",
                         "tmpnam/tempnam is predictable; use mkstemp/mkdtemp", lines);
            ++i;
        }
    }
}

void api_mktemp(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"((?<![A-Za-z0-9_])mktemp\s*\()");
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto& ln : split_lines(fn.body)) {
            if (re.search(ln))
                lint_add(out, rel, fn.name, start + i, "API-MKTEMP",
                         "mktemp() is insecure; use mkstemp/mkdtemp", lines);
            ++i;
        }
    }
}

void api_signal(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto& ln : split_lines(fn.body)) {
            auto args = find_call_args(ln, "signal");
            if (args && args->size() >= 2) {
                auto handler = strip((*args)[1]);
                if (handler != "SIG_DFL" && handler != "SIG_IGN")
                    lint_add(out, rel, fn.name, start + i, "API-SIGNAL",
                             "signal() installs a handler; prefer sigaction()", lines);
            }
            ++i;
        }
    }
}

void api_system(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto& ln : split_lines(fn.body)) {
            auto args = find_call_args(ln, "system");
            if (args && !args->empty() && !is_string_literal((*args)[0]))
                lint_add(out, rel, fn.name, start + i, "API-SYSTEM",
                         "system() command is not a string literal", lines);
            ++i;
        }
    }
}

void api_getenv_null(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& var : getenv_assign_vars(chunk[static_cast<std::size_t>(i)])) {
                auto key = std::pair{var, i};
                if (seen.contains(key)) continue;
                if (getenv_in_truthy_if(chunk[static_cast<std::size_t>(i)], var)) continue;
                if (getenv_only_returned(var, chunk, i)) continue;
                std::optional<int> use_j;
                for (int j = i + 1; j < static_cast<int>(chunk.size()); ++j)
                    if (getenv_uses(var, chunk[static_cast<std::size_t>(j)])) {
                        use_j = j;
                        break;
                    }
                if (!use_j) continue;
                if (null_test_for(var, join_range(chunk, static_cast<std::size_t>(i),
                                                  static_cast<std::size_t>(*use_j))))
                    continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "API-GETENV-NULL",
                         var + " from getenv() used without a NULL test", lines);
            }
        }
    }
}

void api_strdup_null(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            for (auto& var : strdup_assign_vars(chunk[static_cast<std::size_t>(i)])) {
                auto key = std::pair{var, i};
                if (seen.contains(key)) continue;
                if (strdup_in_truthy_if(chunk[static_cast<std::size_t>(i)], var)) continue;
                if (strdup_only_returned(var, chunk, i)) continue;
                std::optional<int> use_j;
                for (int j = i + 1; j < static_cast<int>(chunk.size()); ++j)
                    if (getenv_uses(var, chunk[static_cast<std::size_t>(j)])) {
                        use_j = j;
                        break;
                    }
                if (!use_j) continue;
                if (null_test_for(var, join_range(chunk, static_cast<std::size_t>(i),
                                                  static_cast<std::size_t>(*use_j))))
                    continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "API-STRDUP-NULL",
                         var + " from strdup() used without a NULL test", lines);
            }
        }
    }
}

void api_chroot(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"((?<![A-Za-z0-9_])chroot\s*\()");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!re.search(body_lines[static_cast<std::size_t>(i)])) continue;
            if (chroot_followed_by_chdir(body_lines, i)) continue;
            lint_add(out, rel, fn.name, start + i, "API-CHROOT",
                     "chroot() without chdir(/) into the new root", lines);
        }
    }
}

void api_umask(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex re(R"((?<![A-Za-z0-9_])umask\s*\()");
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        int start = fn.span.first;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!re.search(body_lines[static_cast<std::size_t>(i)])) continue;
            auto args = find_call_args(body_lines[static_cast<std::size_t>(i)], "umask");
            if (!args || args->size() != 1 || !umask_arg_unsafe((*args)[0])) continue;
            lint_add(out, rel, fn.name, start + i, "API-UMASK",
                     "umask(0) leaves world-writable default", lines);
        }
    }
}

}  // namespace






void checkers_core(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, const std::filesystem::path& path,
                   std::string_view raw_text, std::vector<Finding>& out) {
    std::vector<std::string> orig_lines;
    {
        std::istringstream ls{std::string(raw_text)};
        std::string line;
        while (std::getline(ls, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            orig_lines.push_back(std::move(line));
        }
    }
    auto ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool c_only = ext != ".cpp" && ext != ".cc" && ext != ".cxx";

    shift31_realloc(lines, rel, funcs, out);
    masked_switch(lines, rel, funcs, out);
    null_branch(lines, rel, funcs, out);
    lock_balance(lines, rel, funcs, out);
    lock_double_unlock(lines, rel, funcs, out);
    lock_double_lock(lines, rel, funcs, out);
    lock_missing_init(lines, rel, funcs, out);
    div_zero_const(lines, rel, funcs, out);
    onesided_index(lines, rel, funcs, out);
    capacity_first(lines, rel, funcs, out);
    mem_overlap(lines, rel, funcs, out);
    unchecked_alloc(lines, rel, funcs, out);
    nowait_alloc(lines, rel, funcs, out);
    noreturn_fatal(lines, rel, funcs, out);
    mem_lifetime(lines, rel, funcs, out);
    fmt_string(lines, rel, funcs, out);
    memset_swap(lines, rel, funcs, out);
    taut_bound(lines, rel, funcs, out);
    bool_as_bit(lines, rel, funcs, out);
    wrap_alloc(lines, rel, funcs, out);
    int_trunc(lines, rel, funcs, out);
    int_sign_conv(lines, rel, funcs, out);
    stack_escape(lines, rel, funcs, out);
    unbounded_copy(lines, rel, funcs, out);
    str_sprintf(lines, rel, funcs, out);
    missing_return(lines, rel, funcs, out);
    fallthrough(lines, rel, funcs, out);
    dead_guard(lines, rel, funcs, out);
    empty_infinite(lines, rel, funcs, out);
    uninit_return(lines, rel, funcs, out);
    ptr_uninit(lines, rel, funcs, out);
    uninit_branch(lines, rel, funcs, out);
    off_by_one(lines, rel, funcs, out);
    str_missing_nul(lines, rel, funcs, out);
    sibling_asymmetry(lines, rel, funcs, out);
    ignored_error(lines, rel, funcs, out);
    scanf_unchecked(lines, rel, funcs, out);
    api_gets(lines, rel, funcs, out);
    api_strtok(lines, rel, funcs, out);
    api_mkstemp(lines, rel, funcs, out);
    api_tmpnam(lines, rel, funcs, out);
    api_mktemp(lines, rel, funcs, out);
    api_signal(lines, rel, funcs, out);
    fmt_percent_n(lines, rel, funcs, out);
    api_system(lines, rel, funcs, out);
    api_getenv_null(lines, rel, funcs, out);
    api_strdup_null(lines, rel, funcs, out);
    api_chroot(lines, rel, funcs, out);
    api_umask(lines, rel, funcs, out);
    if (c_only) {
        wcs_unbounded(lines, rel, funcs, out);
        int_clz_zero(lines, rel, funcs, out);
        mem_bcopy(lines, rel, funcs, out);
    }
    str_strncpy_nul(lines, rel, funcs, out);
    str_snprintf(lines, rel, funcs, out);
    int_atoi(lines, rel, funcs, out);
    enum_hole(lines, rel, funcs, out);
    fd_leak(lines, rel, funcs, out);
    popen_leak(lines, rel, funcs, out);
    mem_leak(lines, rel, funcs, out);
    mismatched_free(lines, rel, funcs, out);
    str_null_arg(lines, rel, funcs, out);
    ptr_arith(lines, rel, funcs, out);
    float_ub(lines, rel, funcs, out);
    vla_size(lines, rel, funcs, out);
    mem_alloca(lines, rel, funcs, out);
    mem_sizeof_ptr(lines, rel, funcs, out);
    mem_flex_array(join_all(lines), lines, rel, funcs, out);
    intent_mismatch(orig_lines, rel, funcs, out);
    infoleak_pad(lines, rel, funcs, out);
    api_precondition(orig_lines, rel, funcs, out);
    trust_unvalidated_input(lines, rel, funcs, out);
    crypto_misuse(lines, rel, funcs, out);
    crypto_srand(lines, rel, funcs, out);
    mem_realloc_zero(lines, rel, funcs, out);
    lock_order(lines, rel, funcs, out);
    conc_toctou(lines, rel, funcs, out);
    conc_atomicity(lines, rel, funcs, out);
}

}  // namespace prism
