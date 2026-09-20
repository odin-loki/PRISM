#include "prism/checkers.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace prism {
namespace {

bool ident_char(unsigned char c) { return std::isalnum(c) || c == '_'; }

std::size_t call_open_paren(std::string_view ln, std::string_view name) {
    std::size_t pos = 0;
    while (pos + name.size() <= ln.size()) {
        auto p = ln.find(name, pos);
        if (p == std::string_view::npos) return std::string_view::npos;
        bool left_ok = p == 0 || !ident_char(static_cast<unsigned char>(ln[p - 1]));
        auto after = p + name.size();
        if (left_ok) {
            while (after < ln.size() && std::isspace(static_cast<unsigned char>(ln[after]))) ++after;
            if (after < ln.size() && ln[after] == '(') return after + 1;
        }
        pos = p + 1;
    }
    return std::string_view::npos;
}

bool has_call(std::string_view ln, std::string_view name) {
    return call_open_paren(ln, name) != std::string_view::npos;
}

std::string which_callee(std::string_view ln, std::initializer_list<std::string_view> names) {
    std::string last;
    for (auto n : names) {
        last = std::string(n);
        if (has_call(ln, n)) return last;
    }
    return last;
}

bool int_literal_is_zero(std::string_view raw) {
    std::string s(raw);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    static const Regex lit(
        R"(^(?P<n>-?(?:0x[0-9a-fA-F]+|0[0-7]*|\d+))(?:[uUlL]{0,3})?$)");
    auto m = lit.search_match(s);
    if (!m) return false;
    auto n = m->named("n");
    if (n.size() >= 2 && (n[0] == '0' && (n[1] == 'x' || n[1] == 'X'))) {
        try {
            return std::stoll(n, nullptr, 16) == 0;
        } catch (...) {
            return false;
        }
    }
    if (n.size() > 1 && n[0] == '0' && n[1] >= '0' && n[1] <= '7') {
        try {
            return std::stoll(n, nullptr, 8) == 0;
        } catch (...) {
            return false;
        }
    }
    try {
        return std::stoll(n, nullptr, 10) == 0;
    } catch (...) {
        return false;
    }
}

std::string first_call_arg(std::string_view ln, std::string_view name) {
    auto start = call_open_paren(ln, name);
    if (start == std::string_view::npos) return {};
    int depth = 1;
    std::size_t i = start;
    std::size_t arg_end = start;
    for (; i < ln.size() && depth > 0; ++i) {
        if (ln[i] == '(') ++depth;
        else if (ln[i] == ')') --depth;
        else if (ln[i] == ',' && depth == 1) {
            arg_end = i;
            break;
        }
        if (depth == 0) arg_end = i;
    }
    if (depth != 0 && arg_end == start) return {};
    return std::string(ln.substr(start, arg_end - start));
}

void lint_discarded_fn(const std::vector<FunctionInfo>& funcs,
                       const std::vector<std::string>& lines,
                       std::string_view rel,
                       const Regex& re,
                       std::string_view cls,
                       std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        std::size_t i = 0;
        for (auto part : fn.body | std::views::split('\n')) {
            std::string s(part.begin(), part.end());
            if (re.match_line(s)) {
                if (auto m = re.search_match(s)) {
                    auto name = m->named("fn");
                    if (name.empty()) name = m->group(1);
                    lint_add(out, rel, fn.name, start + static_cast<int>(i), cls,
                             name + "() return is discarded", lines);
                }
            }
            ++i;
        }
    }
}

void lint_discarded_which(const std::vector<FunctionInfo>& funcs,
                          const std::vector<std::string>& lines,
                          std::string_view rel,
                          const Regex& re,
                          std::string_view cls,
                          std::initializer_list<std::string_view> names,
                          std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        std::size_t i = 0;
        for (auto part : fn.body | std::views::split('\n')) {
            std::string s(part.begin(), part.end());
            if (re.match_line(s)) {
                auto fname = which_callee(s, names);
                if (!fname.empty())
                    lint_add(out, rel, fn.name, start + static_cast<int>(i), cls,
                             fname + "() return is discarded", lines);
            }
            ++i;
        }
    }
}

void lint_discarded_zero(const std::vector<FunctionInfo>& funcs,
                         const std::vector<std::string>& lines,
                         std::string_view rel,
                         const Regex& re,
                         std::string_view cls,
                         std::initializer_list<std::string_view> names,
                         std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        std::size_t i = 0;
        for (auto part : fn.body | std::views::split('\n')) {
            std::string s(part.begin(), part.end());
            if (!re.match_line(s)) {
                ++i;
                continue;
            }
            for (auto n : names) {
                if (!has_call(s, n)) continue;
                auto arg = first_call_arg(s, n);
                if (!int_literal_is_zero(arg)) continue;
                lint_add(out, rel, fn.name, start + static_cast<int>(i), cls,
                         std::string(n) + "(0) return is discarded", lines);
                break;
            }
            ++i;
        }
    }
}

std::string strip(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::string strip_ws(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    return out;
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

bool fullmatch(const Regex& re, std::string_view s) {
    auto m = re.search_match(s);
    return m && !m->spans.empty() && m->spans[0].first == 0 &&
           m->spans[0].second == static_cast<int>(s.size());
}

bool is_ident(std::string_view s) {
    static Regex re(R"(^[A-Za-z_]\w*$)");
    return fullmatch(re, s);
}

std::vector<std::string> chunk_of(const std::vector<std::string>& lines, const FunctionInfo& fn) {
    int start = fn.span.first, end = fn.span.second;
    if (start < 1) return {};
    auto a = static_cast<std::size_t>(start - 1);
    auto b = static_cast<std::size_t>(end < 0 ? 0 : end);
    if (b > lines.size()) b = lines.size();
    if (a >= lines.size() || a >= b) return {};
    return {lines.begin() + static_cast<std::ptrdiff_t>(a),
            lines.begin() + static_cast<std::ptrdiff_t>(b)};
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
    auto start = call_open_paren(line, fn);
    if (start == std::string_view::npos) return std::nullopt;
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

const std::unordered_set<std::string> kKw = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default",
};

bool skip_has(std::initializer_list<std::string_view> skip, std::string_view name) {
    for (auto s : skip)
        if (s == name) return true;
    return false;
}

bool var_in_call_args(std::string_view var, std::string_view ln,
                      std::initializer_list<std::string_view> skip) {
    static Regex call(R"(\b([A-Za-z_]\w*)\s*\()");
    for (auto& m : call.finditer(ln)) {
        auto name = m.group(1);
        if (kKw.contains(name) || skip_has(skip, name)) continue;
        auto args = find_call_args(ln, name);
        if (!args) continue;
        for (auto& a : *args)
            if (strip_ws(a) == var) return true;
    }
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
        auto& ln = chunk[static_cast<std::size_t>(j)];
        for (auto& m : sz.finditer(ln)) {
            auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
            stripped.append(ln, off, a - off);
            off = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        }
        stripped.append(ln, off, std::string::npos);
        if (arrow.search(stripped) || star.search(stripped) || idx.search(stripped)) return j;
    }
    return std::nullopt;
}

std::optional<int> first_ptr_or_arg_use(std::string_view var, const std::vector<std::string>& chunk,
                                        int start, std::initializer_list<std::string_view> skip) {
    auto ptr = first_ptr_use(var, chunk, start);
    std::optional<int> arg;
    for (int j = start + 1; j < static_cast<int>(chunk.size()); ++j) {
        if (var_in_call_args(var, chunk[static_cast<std::size_t>(j)], skip)) {
            arg = j;
            break;
        }
    }
    if (ptr && arg) return std::min(*ptr, *arg);
    if (ptr) return ptr;
    return arg;
}

bool lt_zero_test_for(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    if (Regex(v + "\\s*<\\s*0\\b").search(text)) return true;
    static Regex call(
        R"((?:accept4|accept|dup3|dup2|dup|pipe2|pipe|openat2|openat|shm_open|)"
        R"(memfd_secret|memfd_create|inotify_init1|inotify_init|pidfd_open|fanotify_init|)"
        R"(userfaultfd|landlock_create_ruleset|signalfd|perf_event_open|)"
        R"(pkey_alloc|fsopen|mq_open|shmget|semget|msgget))"
        R"(\s*\([^;]*\)\s*\)*\s*<\s*0\b)");
    return call.search(text);
}

std::optional<int> first_fd_use(std::string_view var, const std::vector<std::string>& chunk,
                                int start, std::initializer_list<std::string_view> skip) {
    auto v = re_escape(var);
    Regex ret("\\breturn\\s+" + v + "\\s*;");
    Regex arith("\\b" + v + "\\s*[+\\-*/%]");
    for (int j = start + 1; j < static_cast<int>(chunk.size()); ++j) {
        auto& ln = chunk[static_cast<std::size_t>(j)];
        if (var_in_call_args(var, ln, skip)) return j;
        if (ret.search(ln)) return j;
        if (arith.search(ln)) return j;
    }
    return std::nullopt;
}

bool map_failed_test_for(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    std::string failed = R"((?:MAP_FAILED|\(\s*void\s*\*\s*\)\s*-\s*1))";
    Regex re("(?:" + v + "\\s*(?:==|!=)\\s*" + failed + "|" + failed + "\\s*(?:==|!=)\\s*" + v +
             ")");
    return re.search(text);
}

bool mq_error_test_for(std::string_view var, std::string_view text) {
    if (lt_zero_test_for(var, text)) return true;
    auto v = re_escape(var);
    std::string neg1 = R"((?:\(\s*mqd_t\s*\)\s*)?-\s*1)";
    if (Regex(v + "\\s*(?:==|!=)\\s*" + neg1).search(text)) return true;
    if (Regex(neg1 + "\\s*(?:==|!=)\\s*" + v).search(text)) return true;
    if (Regex(v + "\\s*<\\s*\\(\\s*mqd_t\\s*\\)\\s*0\\b").search(text)) return true;
    Regex call(R"(\bmq_open\s*\([^;]*\)\s*\)*\s*(?:==|!=|<)\s*(?:\(\s*mqd_t\s*\)\s*)?(?:0|)" +
               neg1 + ")");
    return call.search(text);
}

bool fmt_literal_has_percent_s(std::string_view s) {
    auto t = strip(s);
    std::string inner;
    if (t.size() >= 3 && t.starts_with("L\"") && t.back() == '"')
        inner = t.substr(2, t.size() - 3);
    else if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        inner = t.substr(1, t.size() - 2);
    else
        return false;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '%') {
            if (i + 1 < inner.size() && inner[i + 1] == '%') {
                ++i;
                continue;
            }
            if (i + 1 < inner.size() && inner[i + 1] == 's') return true;
        }
    }
    return false;
}

bool getenv_uses(std::string_view var, std::string_view ln) {
    auto v = re_escape(var);
    if (Regex("\\breturn\\s+" + v + "\\s*;").search(ln)) return false;
    if (Regex("\\b" + v + "\\s*\\[").search(ln)) return true;
    if (Regex("\\*\\s*" + v + "\\b").search(ln)) return true;
    static const std::pair<const char*, int> callees[] = {
        {"strlen", 0}, {"strcpy", 1}, {"strcat", 1}, {"strcmp", 0}, {"atoi", 0},
        {"atol", 0}, {"atoll", 0},
    };
    for (auto [callee, idx] : callees) {
        auto args = find_call_args(ln, callee);
        if (!args) continue;
        if (callee == std::string_view("strcmp")) {
            for (int i : {0, 1})
                if (i < static_cast<int>(args->size()) &&
                    strip((*args)[static_cast<std::size_t>(i)]) == var)
                    return true;
            continue;
        }
        if (idx < static_cast<int>(args->size()) &&
            strip((*args)[static_cast<std::size_t>(idx)]) == var)
            return true;
    }
    static const std::pair<const char*, int> fmts[] = {
        {"printf", 0}, {"fprintf", 1}, {"sprintf", 1}, {"snprintf", 2},
        {"warn", 0}, {"err", 0}, {"syslog", 0},
    };
    for (auto [fname, fmt_idx] : fmts) {
        auto args = find_call_args(ln, fname);
        if (!args || static_cast<int>(args->size()) <= fmt_idx) continue;
        if (!fmt_literal_has_percent_s((*args)[static_cast<std::size_t>(fmt_idx)])) continue;
        for (std::size_t i = static_cast<std::size_t>(fmt_idx) + 1; i < args->size(); ++i)
            if (strip((*args)[i]) == var) return true;
    }
    return false;
}

std::string pick_fname(std::string_view ln, std::initializer_list<std::string_view> names) {
    for (auto n : names)
        if (find_call_args(ln, n)) return std::string(n);
    if (names.size()) return std::string(*names.begin());
    return {};
}

enum class UseKind { Ptr, PtrOrArg, PtrOrGetenv, PtrOrArgOrGetenv, Fd, FdOrVoidCast };
enum class GuardKind { Null, LtZero, MapFailed, Mq };

bool has_guard(std::string_view var, std::string_view text, GuardKind g) {
    switch (g) {
        case GuardKind::Null: return null_test_for(var, text);
        case GuardKind::LtZero: return lt_zero_test_for(var, text);
        case GuardKind::MapFailed: return map_failed_test_for(var, text);
        case GuardKind::Mq: return mq_error_test_for(var, text);
    }
    return false;
}

std::string unguarded_msg(std::string_view var, std::string_view fname, GuardKind g) {
    std::string f(fname);
    switch (g) {
        case GuardKind::Null:
            return std::string(var) + " from " + f + "() used without a NULL test";
        case GuardKind::LtZero:
            return std::string(var) + " from " + f + "() used without a < 0 test";
        case GuardKind::MapFailed:
            return std::string(var) + " from " + f + "() used without a MAP_FAILED test";
        case GuardKind::Mq:
            return std::string(var) + " from " + f + "() used without a (mqd_t)-1 / < 0 test";
    }
    return {};
}

void api_assign_unguarded(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                          const Regex& assign, std::string_view cls,
                          std::initializer_list<std::string_view> fnames,
                          std::initializer_list<std::string_view> skip, UseKind use,
                          GuardKind guard) {
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto& ln = chunk[static_cast<std::size_t>(i)];
            auto m = assign.search_match(ln);
            if (!m) continue;
            auto var = m->named("var");
            auto key = std::pair{var, i};
            if (seen.contains(key)) continue;
            if ((guard == GuardKind::LtZero || guard == GuardKind::Mq) && has_guard(var, ln, guard))
                continue;
            std::optional<int> use_j;
            switch (use) {
                case UseKind::Ptr:
                    use_j = first_ptr_use(var, chunk, i);
                    break;
                case UseKind::PtrOrArg:
                    use_j = first_ptr_or_arg_use(var, chunk, i, skip);
                    break;
                case UseKind::PtrOrGetenv:
                    use_j = first_ptr_use(var, chunk, i);
                    if (!use_j) {
                        for (int j = i + 1; j < static_cast<int>(chunk.size()); ++j)
                            if (getenv_uses(var, chunk[static_cast<std::size_t>(j)])) {
                                use_j = j;
                                break;
                            }
                    }
                    break;
                case UseKind::PtrOrArgOrGetenv:
                    use_j = first_ptr_or_arg_use(var, chunk, i, skip);
                    if (!use_j) {
                        for (int j = i + 1; j < static_cast<int>(chunk.size()); ++j)
                            if (getenv_uses(var, chunk[static_cast<std::size_t>(j)])) {
                                use_j = j;
                                break;
                            }
                    }
                    break;
                case UseKind::Fd:
                    use_j = first_fd_use(var, chunk, i, skip);
                    break;
                case UseKind::FdOrVoidCast:
                    use_j = first_fd_use(var, chunk, i, skip);
                    if (!use_j) {
                        auto v = re_escape(var);
                        Regex vc("\\(\\s*void\\s*\\)\\s*" + v + "\\b");
                        for (int j = i + 1; j < static_cast<int>(chunk.size()); ++j)
                            if (vc.search(chunk[static_cast<std::size_t>(j)])) {
                                use_j = j;
                                break;
                            }
                    }
                    break;
            }
            if (!use_j) continue;
            auto between = join_range(chunk, static_cast<std::size_t>(i),
                                      static_cast<std::size_t>(*use_j + 1));
            if (has_guard(var, between, guard)) continue;
            seen.insert(key);
            auto fname = pick_fname(ln, fnames);
            lint_add(out, rel, fn.name, start + i, cls, unguarded_msg(var, fname, guard), lines);
        }
    }
}

void api_exec(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static const char* kExec[] = {"execlp", "execle", "execl", "execvpe", "execvp", "execve",
                                  "execv"};
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        std::set<std::pair<std::string, int>> seen;
        for (auto part : fn.body | std::views::split('\n')) {
            std::string ln(part.begin(), part.end());
            for (auto fname : kExec) {
                auto args = find_call_args(ln, fname);
                if (!args || args->empty()) continue;
                if (is_string_literal((*args)[0])) continue;
                auto key = std::pair{std::string(fname), i};
                if (seen.contains(key)) continue;
                seen.insert(key);
                lint_add(out, rel, fn.name, start + i, "API-EXEC",
                         std::string(fname) + "() path is not a string literal", lines);
                break;
            }
            ++i;
        }
    }
}

void api_chmod_world(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    constexpr int kWorld = 0777;
    for (auto& fn : funcs) {
        int start = fn.span.first, i = 0;
        for (auto part : fn.body | std::views::split('\n')) {
            std::string ln(part.begin(), part.end());
            for (auto fname : {"chmod", "fchmod"}) {
                auto args = find_call_args(ln, fname);
                if (!args || args->size() < 2) continue;
                auto mode = parse_int_literal((*args)[1]);
                if (mode != kWorld) continue;
                lint_add(out, rel, fn.name, start + i, "API-CHMOD-WORLD",
                         std::string(fname) + "() mode is world-writable 0777", lines);
                break;
            }
            ++i;
        }
    }
}

bool pipe_compared_in_stmt(std::string_view ln) {
    static Regex a(R"((?:pipe2|pipe)\s*\([^;]*\)\s*(?:==|!=|<|>|<=|>=))");
    static Regex b(R"((?:==|!=|<|>|<=|>=)\s*(?:pipe2|pipe)\s*\()");
    return a.search(ln) || b.search(ln);
}

bool pipe_fds_used(std::string_view name, const std::vector<std::string>& chunk, int start) {
    auto v = re_escape(name);
    Regex idx("\\b" + v + "\\s*\\[");
    Regex ret("\\breturn\\s+" + v + "\\s*;");
    for (int j = start + 1; j < static_cast<int>(chunk.size()); ++j) {
        auto& ln = chunk[static_cast<std::size_t>(j)];
        if (idx.search(ln)) return true;
        if (var_in_call_args(name, ln, {"pipe", "pipe2"})) return true;
        if (ret.search(ln)) return true;
    }
    return false;
}

void api_pipe(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex call(R"((?<![A-Za-z0-9_])(?:pipe2|pipe)\s*\()");
    static Regex discarded(R"(^\s*(?:pipe2|pipe)\s*\([^;]*\)\s*;\s*$)");
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:pipe2|pipe)\s*\()");
    for (auto& fn : funcs) {
        auto chunk = chunk_of(lines, fn);
        int start = fn.span.first;
        std::set<int> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto& ln = chunk[static_cast<std::size_t>(i)];
            if (!call.search(ln)) continue;
            if (pipe_compared_in_stmt(ln)) continue;
            auto fname = find_call_args(ln, "pipe2") ? "pipe2" : "pipe";
            auto args = find_call_args(ln, fname);
            if (!args || args->empty()) continue;
            auto fds = strip_ws((*args)[0]);
            if (!is_ident(fds)) continue;
            if (seen.contains(i)) continue;
            if (discarded.match_line(ln)) {
                if (!pipe_fds_used(fds, chunk, i)) continue;
                seen.insert(i);
                lint_add(out, rel, fn.name, start + i, "API-PIPE",
                         fds + " from " + fname + "() used without a < 0 test", lines);
                continue;
            }
            auto m = assign.search_match(ln);
            if (!m) continue;
            auto var = m->named("var");
            if (lt_zero_test_for(var, ln)) continue;
            auto use_j = first_fd_use(var, chunk, i, {"pipe", "pipe2"});
            bool fds_used = pipe_fds_used(fds, chunk, i);
            if (!use_j && !fds_used) continue;
            if (use_j) {
                auto between = join_range(chunk, static_cast<std::size_t>(i),
                                          static_cast<std::size_t>(*use_j + 1));
                if (lt_zero_test_for(var, between)) continue;
            }
            if (fds_used) {
                auto rest = join_range(chunk, static_cast<std::size_t>(i), chunk.size());
                if (lt_zero_test_for(var, rest)) continue;
            }
            seen.insert(i);
            lint_add(out, rel, fn.name, start + i, "API-PIPE",
                     std::string(fname) + "() result used without a < 0 test", lines);
        }
    }
}

void api_mmap(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?mmap\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-MMAP", {"mmap"}, {}, UseKind::Ptr,
                         GuardKind::MapFailed);
}
void api_getcwd(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?getcwd\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-GETCWD", {"getcwd"}, {},
                         UseKind::PtrOrGetenv, GuardKind::Null);
}
void api_dlopen(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?dlopen\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-DLOPEN", {"dlopen"}, {"dlopen"},
                         UseKind::PtrOrArg, GuardKind::Null);
}
void api_accept(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:accept4|accept)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-ACCEPT", {"accept4", "accept"},
                         {"accept", "accept4"}, UseKind::Fd, GuardKind::LtZero);
}
void api_realpath(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?realpath\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-REALPATH", {"realpath"}, {"realpath"},
                         UseKind::PtrOrArgOrGetenv, GuardKind::Null);
}
void api_socket(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?socket\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-SOCKET", {"socket"},
                         {"accept", "accept4"}, UseKind::Fd, GuardKind::LtZero);
}
void api_dup(const std::vector<std::string>& lines, std::string_view rel,
             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:dup3|dup2|dup)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-DUP", {"dup3", "dup2", "dup"},
                         {"dup", "dup2", "dup3", "accept", "accept4"}, UseKind::Fd,
                         GuardKind::LtZero);
}
void api_openat(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?openat\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-OPENAT", {"openat"}, {"openat"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_opendir(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?opendir\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-OPENDIR", {"opendir"}, {"opendir"},
                         UseKind::PtrOrArg, GuardKind::Null);
}
void api_getpwuid(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:getpwuid|getpwnam)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-GETPWUID", {"getpwnam", "getpwuid"},
                         {"getpwuid", "getpwnam"}, UseKind::PtrOrArg, GuardKind::Null);
}
void api_shm_open(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?shm_open\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-SHM-OPEN", {"shm_open"}, {"shm_open"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_memfd(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?memfd_create\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-MEMFD", {"memfd_create"},
                         {"memfd_create"}, UseKind::Fd, GuardKind::LtZero);
}
void api_getlogin(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:getlogin|ttyname)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-GETLOGIN", {"ttyname", "getlogin"},
                         {"getlogin", "ttyname"}, UseKind::PtrOrArg, GuardKind::Null);
}
void api_inotify(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:inotify_init1|inotify_init)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-INOTIFY",
                         {"inotify_init1", "inotify_init"}, {"inotify_init", "inotify_init1"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_ptsname(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?ptsname\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-PTSNAME", {"ptsname"}, {"ptsname"},
                         UseKind::PtrOrArg, GuardKind::Null);
}
void api_fmemopen(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:open_memstream|fmemopen)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-FMEMOPEN",
                         {"open_memstream", "fmemopen"}, {"fmemopen", "open_memstream"},
                         UseKind::PtrOrArg, GuardKind::Null);
}
void api_pidfd(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?pidfd_open\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-PIDFD", {"pidfd_open"}, {"pidfd_open"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_fanotify(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?fanotify_init\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-FANOTIFY", {"fanotify_init"},
                         {"fanotify_init"}, UseKind::Fd, GuardKind::LtZero);
}
void api_getgrnam(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(?:getgrnam|getgrgid|getspnam)\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-GETGRNAM",
                         {"getspnam", "getgrgid", "getgrnam"},
                         {"getgrnam", "getgrgid", "getspnam"}, UseKind::PtrOrArg, GuardKind::Null);
}
void api_userfaultfd(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?userfaultfd\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-USERFAULTFD", {"userfaultfd"},
                         {"userfaultfd"}, UseKind::Fd, GuardKind::LtZero);
}
void api_getpass(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?getpass\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-GETPASS", {"getpass"}, {"getpass"},
                         UseKind::PtrOrArg, GuardKind::Null);
}
void api_openat2(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?openat2\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-OPENAT2", {"openat2"}, {"openat2"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_landlock(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(
        R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?landlock_create_ruleset\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-LANDLOCK",
                         {"landlock_create_ruleset"}, {"landlock_create_ruleset"}, UseKind::Fd,
                         GuardKind::LtZero);
}
void api_signalfd(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?signalfd\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-SIGNALFD", {"signalfd"}, {"signalfd"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_perf_event(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?perf_event_open\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-PERF-EVENT", {"perf_event_open"},
                         {"perf_event_open"}, UseKind::Fd, GuardKind::LtZero);
}
void api_pkey(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?pkey_alloc\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-PKEY", {"pkey_alloc"}, {"pkey_alloc"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_fsopen(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bfsopen\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-FSOPEN", {"fsopen"}, {"fsopen"},
                         UseKind::Fd, GuardKind::LtZero);
}
void api_mq_open(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bmq_open\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-MQ-OPEN", {"mq_open"}, {"mq_open"},
                         UseKind::FdOrVoidCast, GuardKind::Mq);
}
void api_memfd_secret(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static Regex assign(R"(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?\bmemfd_secret\s*\()");
    api_assign_unguarded(lines, rel, funcs, out, assign, "API-MEMFD-SECRET", {"memfd_secret"},
                         {"memfd_secret"}, UseKind::Fd, GuardKind::LtZero);
}

}  // namespace

void checkers_api(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    static const Regex fork_discarded(R"(^\s*(?:vfork|fork)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, fork_discarded, "API-FORK", "fork()/vfork() result unused (not compared to 0/-1)", out);
    api_exec(lines, rel, funcs, out);
    api_mmap(lines, rel, funcs, out);
    api_getcwd(lines, rel, funcs, out);
    static const Regex ioctl_discarded(R"(^\s*ioctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, ioctl_discarded, "API-IOCTL", "ioctl() return is discarded", out);
    api_dlopen(lines, rel, funcs, out);
    api_accept(lines, rel, funcs, out);
    api_realpath(lines, rel, funcs, out);
    api_chmod_world(lines, rel, funcs, out);
    static const Regex setuid_discarded(R"(^\s*(?:setuid|seteuid|setgid)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_zero(funcs, lines, rel, setuid_discarded, "API-SETUID", {"setuid", "seteuid", "setgid"}, out);
    api_socket(lines, rel, funcs, out);
    static const Regex bind_discarded(R"(^\s*bind\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, bind_discarded, "API-BIND", "bind() return is discarded", out);
    static const Regex listen_discarded(R"(^\s*listen\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, listen_discarded, "API-LISTEN", "listen() return is discarded", out);
    static const Regex connect_discarded(R"(^\s*connect\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, connect_discarded, "API-CONNECT", "connect() return is discarded", out);
    api_pipe(lines, rel, funcs, out);
    api_dup(lines, rel, funcs, out);
    static const Regex fcntl_discarded(R"(^\s*fcntl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, fcntl_discarded, "API-FCNTL", "fcntl() return is discarded", out);
    static const Regex wait_discarded(R"(^\s*(?:waitpid|waitid|wait)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, wait_discarded, "API-WAIT", {"waitpid", "waitid", "wait"}, out);
    static const Regex select_discarded(R"(^\s*(?:epoll_wait|select|poll)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, select_discarded, "API-SELECT", {"epoll_wait", "poll", "select"}, out);
    static const Regex send_discarded(R"(^\s*(?:sendto|recvfrom|send|recv)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, send_discarded, "API-SEND", {"sendto", "recvfrom", "send", "recv"}, out);
    static const Regex shutdown_discarded(R"(^\s*shutdown\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, shutdown_discarded, "API-SHUTDOWN", "shutdown() return is discarded", out);
    static const Regex kill_discarded(R"(^\s*(?:kill|raise)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, kill_discarded, "API-KILL", {"kill", "raise"}, out);
    static const Regex getaddrinfo_discarded(R"(^\s*getaddrinfo\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getaddrinfo_discarded, "API-GETADDRINFO", "getaddrinfo() result used without a == 0 test", out);
    static const Regex pthread_join_discarded(R"(^\s*(?:pthread_join|pthread_detach)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, pthread_join_discarded, "API-PTHREAD-JOIN", {"pthread_join", "pthread_detach"}, out);
    static const Regex thrd_join_discarded(R"(^\s*(?:thrd_join|thrd_detach)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, thrd_join_discarded, "API-THRD-JOIN", {"thrd_join", "thrd_detach"}, out);
    static const Regex sem_wait_discarded(R"(^\s*(?:sem_wait|sem_post)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, sem_wait_discarded, "API-SEM-WAIT", {"sem_wait", "sem_post"}, out);
    api_openat(lines, rel, funcs, out);
    static const Regex flock_discarded(R"(^\s*flock\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, flock_discarded, "API-FLOCK", "flock() return is discarded", out);
    static const Regex chown_discarded(R"(^\s*(?:fchown|lchown|chown)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, chown_discarded, "API-CHOWN", {"fchown", "lchown", "chown"}, out);
    static const Regex symlink_discarded(R"(^\s*(?:symlink|readlink)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, symlink_discarded, "API-SYMLINK", {"symlink", "readlink"}, out);
    api_opendir(lines, rel, funcs, out);
    static const Regex setrlimit_discarded(R"(^\s*(?:setrlimit|getrlimit)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, setrlimit_discarded, "API-SETRLIMIT", {"setrlimit", "getrlimit"}, out);
    static const Regex getsockopt_discarded(R"(^\s*(?:getsockopt|setsockopt)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, getsockopt_discarded, "API-GETSOCKOPT", {"getsockopt", "setsockopt"}, out);
    static const Regex stat_discarded(R"(^\s*(?:lstat|fstat|stat)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, stat_discarded, "API-STAT", {"lstat", "fstat", "stat"}, out);
    static const Regex mkdir_discarded(R"(^\s*(?:mkdir|rmdir)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, mkdir_discarded, "API-MKDIR", {"mkdir", "rmdir"}, out);
    api_getpwuid(lines, rel, funcs, out);
    static const Regex clock_gettime_discarded(R"(^\s*(?:clock_gettime|gettimeofday)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, clock_gettime_discarded, "API-CLOCK-GETTIME", {"gettimeofday", "clock_gettime"}, out);
    api_shm_open(lines, rel, funcs, out);
    static const Regex posix_spawn_discarded(R"(^\s*(?:posix_spawnp|posix_spawn)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, posix_spawn_discarded, "API-POSIX-SPAWN", {"posix_spawnp", "posix_spawn"}, out);
    static const Regex glob_discarded(R"(^\s*glob\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, glob_discarded, "API-GLOB", "glob() return is discarded", out);
    static const Regex fseek_discarded(R"(^\s*(?:fseek|ftell)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, fseek_discarded, "API-FSEEK", {"ftell", "fseek"}, out);
    static const Regex access_discarded(R"(^\s*access\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, access_discarded, "API-ACCESS", "access() return is discarded", out);
    static const Regex getopt_discarded(R"(^\s*(?:getopt_long|getopt)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, getopt_discarded, "API-GETOPT", {"getopt_long", "getopt"}, out);
    static const Regex uname_discarded(R"(^\s*(?:uname|gethostname)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, uname_discarded, "API-UNAME", {"gethostname", "uname"}, out);
    static const Regex sendfile_discarded(R"(^\s*(?:sendfile|copy_file_range)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, sendfile_discarded, "API-SENDFILE", {"copy_file_range", "sendfile"}, out);
    api_memfd(lines, rel, funcs, out);
    static const Regex prctl_discarded(R"(^\s*(?:prctl|ptrace)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, prctl_discarded, "API-PRCTL", {"ptrace", "prctl"}, out);
    static const Regex tcgetattr_discarded(R"(^\s*(?:tcsetattr|tcgetattr)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, tcgetattr_discarded, "API-TCGETATTR", {"tcsetattr", "tcgetattr"}, out);
    static const Regex sysconf_discarded(R"(^\s*(?:pathconf|sysconf)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, sysconf_discarded, "API-SYSCONF", {"pathconf", "sysconf"}, out);
    static const Regex getrusage_discarded(R"(^\s*getrusage\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getrusage_discarded, "API-GETRUSAGE", "getrusage() return is discarded", out);
    static const Regex nftw_discarded(R"(^\s*(?:nftw|ftw)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, nftw_discarded, "API-NFTW", {"nftw", "ftw"}, out);
    static const Regex wordexp_discarded(R"(^\s*wordexp\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, wordexp_discarded, "API-WORDEXP", "wordexp() return is discarded", out);
    api_getlogin(lines, rel, funcs, out);
    static const Regex inet_pton_discarded(R"(^\s*(?:inet_pton|inet_aton)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, inet_pton_discarded, "API-INET-PTON", {"inet_aton", "inet_pton"}, out);
    static const Regex mlock_discarded(R"(^\s*(?:munlock|mlock)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, mlock_discarded, "API-MLOCK", {"munlock", "mlock"}, out);
    static const Regex splice_discarded(R"(^\s*splice\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, splice_discarded, "API-SPLICE", "splice() return is discarded", out);
    api_inotify(lines, rel, funcs, out);
    static const Regex fsync_discarded(R"(^\s*(?:fdatasync|fsync)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, fsync_discarded, "API-FSYNC", {"fdatasync", "fsync"}, out);
    static const Regex getrandom_discarded(R"(^\s*(?:getrandom|getentropy)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, getrandom_discarded, "API-GETRANDOM", {"getentropy", "getrandom"}, out);
    static const Regex getline_discarded(R"(^\s*(?:getdelim|getline)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, getline_discarded, "API-GETLINE", {"getdelim", "getline"}, out);
    static const Regex asprintf_discarded(R"(^\s*(?:vasprintf|asprintf)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, asprintf_discarded, "API-ASPRINTF", {"vasprintf", "asprintf"}, out);
    static const Regex strlcpy_discarded(R"(^\s*(?:strlcpy|strlcat)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, strlcpy_discarded, "API-STRLCPY", {"strlcat", "strlcpy"}, out);
    static const Regex isatty_discarded(R"(^\s*isatty\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, isatty_discarded, "API-ISATTY", "isatty() return is discarded", out);
    api_ptsname(lines, rel, funcs, out);
    static const Regex mount_discarded(R"(^\s*(?:umount|mount)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, mount_discarded, "API-MOUNT", {"umount", "mount"}, out);
    api_fmemopen(lines, rel, funcs, out);
    static const Regex scandir_discarded(R"(^\s*scandir\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, scandir_discarded, "API-SCANDIR", "scandir() return is discarded", out);
    static const Regex setxattr_discarded(R"(^\s*(?:listxattr|getxattr|setxattr)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, setxattr_discarded, "API-SETXATTR", {"listxattr", "getxattr", "setxattr"}, out);
    static const Regex sched_affinity_discarded(R"(^\s*(?:sched_setaffinity|sched_getaffinity)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, sched_affinity_discarded, "API-SCHED-AFFINITY", {"sched_getaffinity", "sched_setaffinity"}, out);
    static const Regex aio_discarded(R"(^\s*(?:aio_read|aio_write)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, aio_discarded, "API-AIO", {"aio_write", "aio_read"}, out);
    static const Regex statx_discarded(R"(^\s*statx\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, statx_discarded, "API-STATX", "statx() return is discarded", out);
    api_pidfd(lines, rel, funcs, out);
    static const Regex capset_discarded(R"(^\s*(?:capset|capget)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, capset_discarded, "API-CAPSET", {"capget", "capset"}, out);
    api_fanotify(lines, rel, funcs, out);
    static const Regex seccomp_discarded(R"(^\s*seccomp\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, seccomp_discarded, "API-SECCOMP", "seccomp() return is discarded", out);
    api_getgrnam(lines, rel, funcs, out);
    static const Regex fallocate_discarded(R"(^\s*(?:posix_fallocate|fallocate)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, fallocate_discarded, "API-FALLOCATE", {"posix_fallocate", "fallocate"}, out);
    static const Regex close_range_discarded(R"(^\s*close_range\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, close_range_discarded, "API-CLOSE-RANGE", "close_range() return is discarded", out);
    static const Regex bpf_discarded(R"(^\s*bpf\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, bpf_discarded, "API-BPF", "bpf() return is discarded", out);
    api_userfaultfd(lines, rel, funcs, out);
    api_getpass(lines, rel, funcs, out);
    static const Regex initgroups_discarded(R"(^\s*(?:initgroups|setgroups)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, initgroups_discarded, "API-INITGROUPS", {"setgroups", "initgroups"}, out);
    static const Regex clone_discarded(R"(^\s*(?:clone|unshare|setns)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, clone_discarded, "API-CLONE", {"unshare", "setns", "clone"}, out);
    api_openat2(lines, rel, funcs, out);
    api_landlock(lines, rel, funcs, out);
    static const Regex getpriority_discarded(R"(^\s*(?:getpriority|setpriority)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, getpriority_discarded, "API-GETPRIORITY", {"setpriority", "getpriority"}, out);
    api_signalfd(lines, rel, funcs, out);
    static const Regex sendmmsg_discarded(R"(^\s*(?:sendmmsg|recvmmsg)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, sendmmsg_discarded, "API-SENDMMSG", {"recvmmsg", "sendmmsg"}, out);
    static const Regex personality_discarded(R"(^\s*personality\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, personality_discarded, "API-PERSONALITY", "personality() return is discarded", out);
    static const Regex quotactl_discarded(R"(^\s*quotactl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, quotactl_discarded, "API-QUOTACTL", "quotactl() return is discarded", out);
    static const Regex name_to_handle_discarded(R"(^\s*(?:name_to_handle_at|open_by_handle_at)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, name_to_handle_discarded, "API-NAME-TO-HANDLE", {"open_by_handle_at", "name_to_handle_at"}, out);
    static const Regex process_madvise_discarded(R"(^\s*process_madvise\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, process_madvise_discarded, "API-PROCESS-MADVISE", "process_madvise() return is discarded", out);
    static const Regex pivot_root_discarded(R"(^\s*pivot_root\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pivot_root_discarded, "API-PIVOT-ROOT", "pivot_root() return is discarded", out);
    static const Regex statfs_discarded(R"(^\s*\b(?:fstatfs|statfs)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, statfs_discarded, "API-STATFS", {"fstatfs", "statfs"}, out);
    static const Regex prlimit_discarded(R"(^\s*(?:prlimit64|prlimit)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, prlimit_discarded, "API-PRLIMIT", {"prlimit64", "prlimit"}, out);
    api_perf_event(lines, rel, funcs, out);
    static const Regex membarrier_discarded(R"(^\s*membarrier\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, membarrier_discarded, "API-MEMBARRIER", "membarrier() return is discarded", out);
    api_pkey(lines, rel, funcs, out);
    static const Regex syncfs_discarded(R"(^\s*\bsyncfs\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, syncfs_discarded, "API-SYNCFS", "syncfs() return is discarded", out);
    static const Regex process_vm_discarded(R"(^\s*(?:process_vm_readv|process_vm_writev)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, process_vm_discarded, "API-PROCESS-VM", {"process_vm_writev", "process_vm_readv"}, out);
    static const Regex clone3_discarded(R"(^\s*\bclone3\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, clone3_discarded, "API-CLONE3", "clone3() return is discarded", out);
    static const Regex futex_discarded(R"(^\s*futex\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, futex_discarded, "API-FUTEX", "futex() return is discarded", out);
    static const Regex keyctl_discarded(R"(^\s*keyctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, keyctl_discarded, "API-KEYCTL", "keyctl() return is discarded", out);
    static const Regex kcmp_discarded(R"(^\s*kcmp\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, kcmp_discarded, "API-KCMP", "kcmp() return is discarded", out);
    api_fsopen(lines, rel, funcs, out);
    api_mq_open(lines, rel, funcs, out);
    static const Regex shmget_discarded(R"(^\s*shmget\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, shmget_discarded, "API-SHMGET", "shmget() return is discarded", out);
    static const Regex reboot_discarded(R"(^\s*reboot\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, reboot_discarded, "API-REBOOT", "reboot() return is discarded", out);
    static const Regex adjtimex_discarded(R"(^\s*\badjtimex\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, adjtimex_discarded, "API-ADJTIMEX", "adjtimex() return is discarded", out);
    static const Regex sethostname_discarded(R"(^\s*sethostname\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sethostname_discarded, "API-SETHOSTNAME", "sethostname() return is discarded", out);
    static const Regex swapon_discarded(R"(^\s*\b(?:swapon|swapoff)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, swapon_discarded, "API-SWAPON", {"swapoff", "swapon"}, out);
    static const Regex acct_discarded(R"(^\s*\bacct\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, acct_discarded, "API-ACCT", "acct() return is discarded", out);
    static const Regex ioperm_discarded(R"(^\s*\b(?:ioperm|iopl)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, ioperm_discarded, "API-IOPERM", {"iopl", "ioperm"}, out);
    static const Regex mincore_discarded(R"(^\s*\bmincore\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, mincore_discarded, "API-MINCORE", "mincore() return is discarded", out);
    static const Regex rseq_discarded(R"(^\s*\brseq\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, rseq_discarded, "API-RSEQ", "rseq() return is discarded", out);
    static const Regex timer_create_discarded(R"(^\s*\btimer_create\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, timer_create_discarded, "API-TIMER-CREATE", "timer_create() return is discarded", out);
    static const Regex semget_discarded(R"(^\s*\bsemget\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, semget_discarded, "API-SEMGET", "semget() return is discarded", out);
    static const Regex msgget_discarded(R"(^\s*\bmsgget\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, msgget_discarded, "API-MSGGET", "msgget() return is discarded", out);
    static const Regex klogctl_discarded(R"(^\s*\bklogctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, klogctl_discarded, "API-KLOGCTL", "klogctl() return is discarded", out);
    static const Regex mount_setattr_discarded(R"(^\s*\bmount_setattr\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, mount_setattr_discarded, "API-MOUNT-SETATTR", "mount_setattr() return is discarded", out);
    static const Regex getcpu_discarded(R"(^\s*\bgetcpu\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getcpu_discarded, "API-GETCPU", "getcpu() return is discarded", out);
    static const Regex process_mrelease_discarded(R"(^\s*\bprocess_mrelease\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, process_mrelease_discarded, "API-PROCESS-MRELEASE", "process_mrelease() return is discarded", out);
    api_memfd_secret(lines, rel, funcs, out);
    static const Regex ioprio_discarded(R"(^\s*\b(?:ioprio_set|ioprio_get)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, ioprio_discarded, "API-IOPRIO", {"ioprio_get", "ioprio_set"}, out);
    static const Regex init_module_discarded(R"(^\s*\b(?:init_module|finit_module|delete_module)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, init_module_discarded, "API-INIT-MODULE", {"finit_module", "delete_module", "init_module"}, out);
    static const Regex kexec_discarded(R"(^\s*\b(?:kexec_load|kexec_file_load)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, kexec_discarded, "API-KEXEC", {"kexec_file_load", "kexec_load"}, out);
    static const Regex quotactl_fd_discarded(R"(^\s*\bquotactl_fd\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, quotactl_fd_discarded, "API-QUOTACTL-FD", "quotactl_fd() return is discarded", out);
    static const Regex pkey_free_discarded(R"(^\s*\b(?:pkey_free|pkey_mprotect)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, pkey_free_discarded, "API-PKEY-FREE", {"pkey_mprotect", "pkey_free"}, out);
    static const Regex tgkill_discarded(R"(^\s*\btgkill\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, tgkill_discarded, "API-TGKILL", "tgkill() return is discarded", out);
    static const Regex add_key_discarded(R"(^\s*\badd_key\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, add_key_discarded, "API-ADD-KEY", "add_key() return is discarded", out);
    static const Regex semctl_discarded(R"(^\s*\bsemctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, semctl_discarded, "API-SEMCTL", "semctl() return is discarded", out);
    static const Regex msgctl_discarded(R"(^\s*\bmsgctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, msgctl_discarded, "API-MSGCTL", "msgctl() return is discarded", out);
    static const Regex io_setup_discarded(R"(^\s*\b(?P<fn>io_setup|io_destroy|io_cancel|io_pgetevents)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, io_setup_discarded, "API-IO-SETUP", out);
    static const Regex request_key_discarded(R"(^\s*\brequest_key\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, request_key_discarded, "API-REQUEST-KEY", "request_key() return is discarded", out);
    static const Regex tkill_discarded(R"(^\s*\btkill\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, tkill_discarded, "API-TKILL", "tkill() return is discarded", out);
    static const Regex timer_delete_discarded(R"(^\s*\b(?P<fn>timer_delete|timer_gettime|timer_getoverrun)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, timer_delete_discarded, "API-TIMER-DELETE", out);
    static const Regex mq_unlink_discarded(R"(^\s*\b(?P<fn>mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|mq_getsetattr)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, mq_unlink_discarded, "API-MQ-UNLINK", out);
    static const Regex shmat_discarded(R"(^\s*\b(?P<fn>shmat|shmdt)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, shmat_discarded, "API-SHMAT", out);
    static const Regex semop_discarded(R"(^\s*\b(?P<fn>semop|semtimedop)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, semop_discarded, "API-SEMOP", out);
    static const Regex msgsnd_discarded(R"(^\s*\b(?P<fn>msgsnd|msgrcv)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, msgsnd_discarded, "API-MSGSND", out);
    static const Regex sync_file_range_discarded(R"(^\s*\bsync_file_range\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sync_file_range_discarded, "API-SYNC-FILE-RANGE", "sync_file_range() return is discarded", out);
    static const Regex msync_discarded(R"(^\s*\b(?P<fn>msync|mremap)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, msync_discarded, "API-MSYNC", out);
    static const Regex socketpair_discarded(R"(^\s*\bsocketpair\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, socketpair_discarded, "API-SOCKETPAIR", "socketpair() return is discarded", out);
    static const Regex sysinfo_discarded(R"(^\s*\bsysinfo\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sysinfo_discarded, "API-SYSINFO", "sysinfo() return is discarded", out);
    static const Regex clock_settime_discarded(R"(^\s*\b(?P<fn>clock_settime|clock_adjtime|clock_nanosleep)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, clock_settime_discarded, "API-CLOCK-SETTIME", out);
    static const Regex settimeofday_discarded(R"(^\s*\bsettimeofday\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, settimeofday_discarded, "API-SETTIMEOFDAY", "settimeofday() return is discarded", out);
    static const Regex gettid_discarded(R"(^\s*\bgettid\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, gettid_discarded, "API-GETTID", "gettid() return is discarded", out);
    static const Regex sched_setscheduler_discarded(R"(^\s*\b(?P<fn>sched_setscheduler|sched_getscheduler|sched_setparam|sched_getparam)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sched_setscheduler_discarded, "API-SCHED-SETSCHEDULER", out);
    static const Regex setitimer_discarded(R"(^\s*\b(?P<fn>setitimer|getitimer)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, setitimer_discarded, "API-SETITIMER", out);
    static const Regex nice_discarded(R"(^\s*\bnice\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, nice_discarded, "API-NICE", "nice() return is discarded", out);
    static const Regex arch_prctl_discarded(R"(^\s*\barch_prctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, arch_prctl_discarded, "API-ARCH-PRCTL", "arch_prctl() return is discarded", out);
    static const Regex getdents_discarded(R"(^\s*\b(?P<fn>getdents(?:64)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, getdents_discarded, "API-GETDENTS", out);
    static const Regex utimensat_discarded(R"(^\s*\b(?P<fn>utimensat|futimens|utimes)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, utimensat_discarded, "API-UTIMENSAT", out);
    static const Regex linkat_discarded(R"(^\s*\blinkat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, linkat_discarded, "API-LINKAT", "linkat() return is discarded", out);
    static const Regex mbind_discarded(R"(^\s*\b(?P<fn>mbind|set_mempolicy|get_mempolicy)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, mbind_discarded, "API-MBIND", out);
    static const Regex futex_waitv_discarded(R"(^\s*\bfutex_waitv\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, futex_waitv_discarded, "API-FUTEX-WAITV", "futex_waitv() return is discarded", out);
    static const Regex syslog_discarded(R"(^\s*\bsyslog\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, syslog_discarded, "API-SYSLOG", "syslog() return is discarded", out);
    static const Regex setpgid_discarded(R"(^\s*\b(?P<fn>setpgid|setsid|getsid)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, setpgid_discarded, "API-SETPGID", out);
    static const Regex setreuid_discarded(R"(^\s*\b(?P<fn>setreuid|setregid|setresuid|setresgid)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, setreuid_discarded, "API-SETREUID", out);
    static const Regex getgroups_discarded(R"(^\s*\bgetgroups\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getgroups_discarded, "API-GETGROUPS", "getgroups() return is discarded", out);
    static const Regex epoll_create_discarded(R"(^\s*\b(?P<fn>epoll_create(?:1)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, epoll_create_discarded, "API-EPOLL-CREATE", out);
    static const Regex timerfd_settime_discarded(R"(^\s*\b(?P<fn>timerfd_settime|timerfd_gettime)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, timerfd_settime_discarded, "API-TIMERFD-SETTIME", out);
    static const Regex remap_file_pages_discarded(R"(^\s*\bremap_file_pages\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, remap_file_pages_discarded, "API-REMAP-FILE-PAGES", "remap_file_pages() return is discarded", out);
    static const Regex move_pages_discarded(R"(^\s*\b(?P<fn>migrate_pages|move_pages)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, move_pages_discarded, "API-MOVE-PAGES", out);
    static const Regex cachestat_discarded(R"(^\s*\bcachestat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, cachestat_discarded, "API-CACHESTAT", "cachestat() return is discarded", out);
    static const Regex map_shadow_stack_discarded(R"(^\s*\bmap_shadow_stack\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, map_shadow_stack_discarded, "API-MAP-SHADOW-STACK", "map_shadow_stack() return is discarded", out);
    static const Regex sched_yield_discarded(R"(^\s*\bsched_yield\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sched_yield_discarded, "API-SCHED-YIELD", "sched_yield() return is discarded", out);
    static const Regex setfsuid_discarded(R"(^\s*\b(?P<fn>setfsuid|setfsgid)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, setfsuid_discarded, "API-SETFSUID", out);
    static const Regex wait4_discarded(R"(^\s*\b(?P<fn>wait[34])\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, wait4_discarded, "API-WAIT4", out);
    static const Regex preadv_discarded(R"(^\s*\b(?P<fn>preadv2|pwritev2|preadv|pwritev)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, preadv_discarded, "API-PREADV", out);
    static const Regex sendmsg_discarded(R"(^\s*\b(?P<fn>sendmsg|recvmsg)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sendmsg_discarded, "API-SENDMSG", out);
    static const Regex getsockname_discarded(R"(^\s*\b(?P<fn>getsockname|getpeername)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, getsockname_discarded, "API-GETSOCKNAME", out);
    static const Regex epoll_pwait_discarded(R"(^\s*\b(?P<fn>epoll_pwait2|epoll_pwait)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, epoll_pwait_discarded, "API-EPOLL-PWAIT", out);
    static const Regex inotify_rm_watch_discarded(R"(^\s*\binotify_rm_watch\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, inotify_rm_watch_discarded, "API-INOTIFY-RM-WATCH", "inotify_rm_watch() return is discarded", out);
    static const Regex eventfd_rw_discarded(R"(^\s*\b(?P<fn>eventfd_read|eventfd_write)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, eventfd_rw_discarded, "API-EVENTFD-READ", out);
    static const Regex sched_setattr_discarded(R"(^\s*\b(?P<fn>sched_setattr|sched_getattr)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sched_setattr_discarded, "API-SCHED-SETATTR", out);
    static const Regex renameat2_discarded(R"(^\s*\brenameat2\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, renameat2_discarded, "API-RENAMEAT2", "renameat2() return is discarded", out);
    static const Regex execveat_discarded(R"(^\s*\bexecveat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, execveat_discarded, "API-EXECVEAT", "execveat() return is discarded", out);
    static const Regex mlock2_discarded(R"(^\s*\bmlock2\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, mlock2_discarded, "API-MLOCK2", "mlock2() return is discarded", out);
    static const Regex faccessat2_discarded(R"(^\s*\bfaccessat2\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, faccessat2_discarded, "API-FACCESSAT2", "faccessat2() return is discarded", out);
    static const Regex posix_fadvise_discarded(R"(^\s*\b(?P<fn>posix_fadvise(?:64)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, posix_fadvise_discarded, "API-POSIX-FADVISE", out);
    static const Regex readahead_discarded(R"(^\s*\breadahead\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, readahead_discarded, "API-READAHEAD", "readahead() return is discarded", out);
    static const Regex sigaction_discarded(R"(^\s*\bsigaction\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sigaction_discarded, "API-SIGACTION", "sigaction() return is discarded", out);
    static const Regex sigprocmask_discarded(R"(^\s*\b(?P<fn>sig(?:procmask|suspend))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sigprocmask_discarded, "API-SIGPROCMASK", out);
    static const Regex sem_open_discarded(R"(^\s*\b(?P<fn>sem_(?:open|close|unlink))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sem_open_discarded, "API-SEM-OPEN", out);
    static const Regex rwlock_discarded(R"(^\s*\b(?P<fn>pthread_rwlock_\w+)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, rwlock_discarded, "API-RWLOCK", out);
    static const Regex pthread_cond_discarded(R"(^\s*\b(?P<fn>pthread_cond_\w+)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, pthread_cond_discarded, "API-PTHREAD-COND", out);
    static const Regex sigaltstack_discarded(R"(^\s*\bsigaltstack\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sigaltstack_discarded, "API-SIGALTSTACK", "sigaltstack() return is discarded", out);
    static const Regex renameat_discarded(R"(^\s*\brenameat(?!2)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, renameat_discarded, "API-RENAMEAT", "renameat() return is discarded", out);
    static const Regex faccessat_discarded(R"(^\s*\bfaccessat(?!2)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, faccessat_discarded, "API-FACCESSAT", "faccessat() return is discarded", out);
    static const Regex fchmodat_discarded(R"(^\s*\bfchmodat(?!2)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, fchmodat_discarded, "API-FCHMODAT", "fchmodat() return is discarded", out);
    static const Regex pthread_barrier_discarded(R"(^\s*\b(?P<fn>pthread_barrier_\w+)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, pthread_barrier_discarded, "API-PTHREAD-BARRIER", out);
    static const Regex symlinkat_discarded(R"(^\s*\bsymlinkat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, symlinkat_discarded, "API-SYMLINKAT", "symlinkat() return is discarded", out);
    static const Regex unlinkat_discarded(R"(^\s*\bunlinkat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, unlinkat_discarded, "API-UNLINKAT", "unlinkat() return is discarded", out);
    static const Regex mkdirat_discarded(R"(^\s*\bmkdirat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, mkdirat_discarded, "API-MKDIRAT", "mkdirat() return is discarded", out);
    static const Regex mknodat_discarded(R"(^\s*\bmknodat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, mknodat_discarded, "API-MKNODAT", "mknodat() return is discarded", out);
    static const Regex readlinkat_discarded(R"(^\s*\breadlinkat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, readlinkat_discarded, "API-READLINKAT", "readlinkat() return is discarded", out);
    static const Regex fstatat_discarded(R"(^\s*\bfstatat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, fstatat_discarded, "API-FSTATAT", "fstatat() return is discarded", out);
    static const Regex pthread_spin_discarded(R"(^\s*\b(?P<fn>pthread_spin_\w+)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, pthread_spin_discarded, "API-PTHREAD-SPIN", out);
    static const Regex pthread_key_discarded(R"(^\s*\b(?P<fn>pthread_(?:key_create|key_delete|setspecific|getspecific))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, pthread_key_discarded, "API-PTHREAD-KEY", out);
    static const Regex pthread_cancel_discarded(R"(^\s*\bpthread_cancel\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pthread_cancel_discarded, "API-PTHREAD-CANCEL", "pthread_cancel() return is discarded", out);
    static const Regex pthread_kill_discarded(R"(^\s*\bpthread_kill\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pthread_kill_discarded, "API-PTHREAD-KILL", "pthread_kill() return is discarded", out);
    static const Regex pthread_sigmask_discarded(R"(^\s*\bpthread_sigmask\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pthread_sigmask_discarded, "API-PTHREAD-SIGMASK", "pthread_sigmask() return is discarded", out);
    static const Regex pthread_atfork_discarded(R"(^\s*\bpthread_atfork\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pthread_atfork_discarded, "API-PTHREAD-ATFORK", "pthread_atfork() return is discarded", out);
    static const Regex pledge_discarded(R"(^\s*\bpledge\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pledge_discarded, "API-PLEDGE", "pledge() return is discarded", out);
    static const Regex unveil_discarded(R"(^\s*\bunveil\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, unveil_discarded, "API-UNVEIL", "unveil() return is discarded", out);
    static const Regex sysctl_discarded(R"(^\s*\b(?P<fn>sysctl(?:byname)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sysctl_discarded, "API-SYSCTL", out);
    static const Regex kqueue_discarded(R"(^\s*\bkqueue\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, kqueue_discarded, "API-KQUEUE", "kqueue() return is discarded", out);
    static const Regex kevent_discarded(R"(^\s*\bkevent\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, kevent_discarded, "API-KEVENT", "kevent() return is discarded", out);
    static const Regex pause_discarded(R"(^\s*\bpause\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pause_discarded, "API-PAUSE", "pause() return is discarded", out);
    static const Regex ppoll_discarded(R"(^\s*\bppoll\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, ppoll_discarded, "API-PPOLL", "ppoll() return is discarded", out);
    static const Regex sigwait_discarded(R"(^\s*\b(?P<fn>sigtimedwait|sigwaitinfo|sigpending|sigwait)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sigwait_discarded, "API-SIGWAIT", out);
    static const Regex sigqueue_discarded(R"(^\s*\bsigqueue\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sigqueue_discarded, "API-SIGQUEUE", "sigqueue() return is discarded", out);
    static const Regex ucontext_discarded(R"(^\s*\b(?P<fn>(?:get|set|swap|make)context)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, ucontext_discarded, "API-UCONTEXT", out);
    static const Regex sem_timedwait_discarded(R"(^\s*\bsem_timedwait\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sem_timedwait_discarded, "API-SEM-TIMEDWAIT", "sem_timedwait() return is discarded", out);
    static const Regex pthread_attr_discarded(R"(^\s*\b(?P<fn>pthread_attr_(?:init|destroy|setstack\w*|setdetachstate))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, pthread_attr_discarded, "API-PTHREAD-ATTR", out);
    static const Regex cap_enter_discarded(R"(^\s*\bcap_enter\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, cap_enter_discarded, "API-CAP-ENTER", "cap_enter() return is discarded", out);
    static const Regex cap_rights_discarded(R"(^\s*\b(?P<fn>cap_rights_(?:limit|get))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, cap_rights_discarded, "API-CAP-RIGHTS", out);
    static const Regex pdfork_discarded(R"(^\s*\bpdfork\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pdfork_discarded, "API-PDFORK", "pdfork() return is discarded", out);
    static const Regex procctl_discarded(R"(^\s*\bprocctl\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, procctl_discarded, "API-PROCCTL", "procctl() return is discarded", out);
    static const Regex closefrom_discarded(R"(^\s*\bclosefrom\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, closefrom_discarded, "API-CLOSEFROM", "closefrom() return is discarded", out);
    static const Regex issetugid_discarded(R"(^\s*\bissetugid\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, issetugid_discarded, "API-ISSETUGID", "issetugid() return is discarded", out);
    static const Regex arc4random_discarded(R"(^\s*\b(?P<fn>arc4random(?:_buf|_uniform)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, arc4random_discarded, "API-ARC4RANDOM", out);
    static const Regex chflags_discarded(R"(^\s*\b(?P<fn>(?:f|l)?chflags)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, chflags_discarded, "API-CHFLAGS", out);
    static const Regex getfsstat_discarded(R"(^\s*\bgetfsstat\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getfsstat_discarded, "API-GETFSSTAT", "getfsstat() return is discarded", out);
    static const Regex pthread_yield_discarded(R"(^\s*\bpthread_yield\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, pthread_yield_discarded, "API-PTHREAD-YIELD", "pthread_yield() return is discarded", out);
    static const Regex sem_trywait_discarded(R"(^\s*\b(?P<fn>sem_(?:trywait|getvalue))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sem_trywait_discarded, "API-SEM-TRYWAIT", out);
    static const Regex adjtime_discarded(R"(^\s*\b(?P<fn>(?:ntp_)?adjtime(?!x))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, adjtime_discarded, "API-ADJTIME", out);
    static const Regex revoke_discarded(R"(^\s*\brevoke\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, revoke_discarded, "API-REVOKE", "revoke() return is discarded", out);
    static const Regex ktrace_discarded(R"(^\s*\bktrace\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, ktrace_discarded, "API-KTRACE", "ktrace() return is discarded", out);
    static const Regex rfork_discarded(R"(^\s*\brfork\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, rfork_discarded, "API-RFORK", "rfork() return is discarded", out);
    static const Regex jail_discarded(R"(^\s*\b(?P<fn>jail(?:_attach|_get|_set|_remove)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, jail_discarded, "API-JAIL", out);
    static const Regex setlogin_discarded(R"(^\s*\bsetlogin\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, setlogin_discarded, "API-SETLOGIN", "setlogin() return is discarded", out);
    static const Regex getresuid_discarded(R"(^\s*\b(?P<fn>getres(?:uid|gid))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, getresuid_discarded, "API-GETRESUID", out);
    static const Regex getpeereid_discarded(R"(^\s*\bgetpeereid\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getpeereid_discarded, "API-GETPEEREID", "getpeereid() return is discarded", out);
    static const Regex strtonum_discarded(R"(^\s*\bstrtonum\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, strtonum_discarded, "API-STRTONUM", "strtonum() return is discarded", out);
    static const Regex reallocarray_discarded(R"(^\s*\breallocarray\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, reallocarray_discarded, "API-REALLOCARRAY", "reallocarray() return is discarded", out);
    static const Regex timingsafe_discarded(R"(^\s*\b(?P<fn>timingsafe_(?:bcmp|memcmp))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, timingsafe_discarded, "API-TIMINGSAFE", out);
    static const Regex getprogname_discarded(R"(^\s*\b(?P<fn>(?:get|set)progname)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, getprogname_discarded, "API-GETPROGNAME", out);
    static const Regex daemon_discarded(R"(^\s*\b(?P<fn>daemon|setproctitle)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, daemon_discarded, "API-DAEMON", out);
    static const Regex cap_fcntls_discarded(R"(^\s*\b(?P<fn>cap_(?:fcntls|ioctls)_limit)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, cap_fcntls_discarded, "API-CAP-FCNTLS", out);
    static const Regex pdgetpid_discarded(R"(^\s*\b(?P<fn>pd(?:getpid|wait4))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, pdgetpid_discarded, "API-PDGETPID", out);
    static const Regex kldload_discarded(R"(^\s*\b(?P<fn>kld(?:load|unload|find|sym|stat))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, kldload_discarded, "API-KLDLOAD", out);
    static const Regex extattr_discarded(R"(^\s*\b(?P<fn>extattr_(?:set|get|delete|list)_(?:file|fd|link))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, extattr_discarded, "API-EXTATTR", out);
    static const Regex mac_discarded(R"(^\s*\b(?P<fn>mac_(?:set|get)_(?:proc|fd|file))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, mac_discarded, "API-MAC", out);
    static const Regex audit_discarded(R"(^\s*\b(?P<fn>auditon|getaudit|setaudit|auditctl)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, audit_discarded, "API-AUDIT", out);
    static const Regex kvm_discarded(R"(^\s*\b(?P<fn>kvm_(?:open|openfiles|getprocs|close|nlist))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, kvm_discarded, "API-KVM", out);
    static const Regex reallocf_discarded(R"(^\s*\breallocf\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, reallocf_discarded, "API-REALLOCF", "reallocf() return is discarded", out);
    static const Regex uuidgen_discarded(R"(^\s*\buuidgen\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, uuidgen_discarded, "API-UUIDGEN", "uuidgen() return is discarded", out);
    static const Regex setfib_discarded(R"(^\s*\bsetfib\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, setfib_discarded, "API-SETFIB", "setfib() return is discarded", out);
    static const Regex ntp_gettime_discarded(R"(^\s*\bntp_gettime\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, ntp_gettime_discarded, "API-NTP-GETTIME", "ntp_gettime() return is discarded", out);
    static const Regex crypt_newhash_discarded(R"(^\s*\b(?P<fn>crypt_(?:newhash|checkpass))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, crypt_newhash_discarded, "API-CRYPT-NEWHASH", out);
    static const Regex wait6_discarded(R"(^\s*\bwait6\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, wait6_discarded, "API-WAIT6", "wait6() return is discarded", out);
    static const Regex cpuset_discarded(R"(^\s*\b(?P<fn>cpuset_(?:set|get)affinity)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, cpuset_discarded, "API-CPUSET", out);
    static const Regex rtprio_discarded(R"(^\s*\b(?P<fn>rtprio(?:_thread)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, rtprio_discarded, "API-RTPRIO", out);
    static const Regex kenv_discarded(R"(^\s*\bkenv\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, kenv_discarded, "API-KENV", "kenv() return is discarded", out);
    static const Regex getfh_discarded(R"(^\s*\b(?P<fn>getfh|fhopen|fhstatfs|fhstat|getfhat)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, getfh_discarded, "API-GETFH", out);
    static const Regex getmntinfo_discarded(R"(^\s*\bgetmntinfo\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getmntinfo_discarded, "API-GETMNTINFO", "getmntinfo() return is discarded", out);
    static const Regex nmount_discarded(R"(^\s*\bnmount\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, nmount_discarded, "API-NMOUNT", "nmount() return is discarded", out);
    static const Regex strmode_discarded(R"(^\s*\bstrmode\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, strmode_discarded, "API-STRMODE", "strmode() return is discarded", out);
    static const Regex getosreldate_discarded(R"(^\s*\bgetosreldate\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getosreldate_discarded, "API-GETOSRELDATE", "getosreldate() return is discarded", out);
    static const Regex cap_sandboxed_discarded(R"(^\s*\bcap_sandboxed\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, cap_sandboxed_discarded, "API-CAP-SANDBOXED", "cap_sandboxed() return is discarded", out);
    static const Regex getgrouplist_discarded(R"(^\s*\bgetgrouplist\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getgrouplist_discarded, "API-GETGROUPLIST", "getgrouplist() return is discarded", out);
    static const Regex eaccess_discarded(R"(^\s*\beaccess\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, eaccess_discarded, "API-EACCESS", "eaccess() return is discarded", out);
    static const Regex login_getclass_discarded(R"(^\s*\b(?P<fn>login_getclass|setusercontext)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, login_getclass_discarded, "API-LOGIN-GETCLASS", out);
    static const Regex fflags_discarded(R"(^\s*\b(?P<fn>fflagstostr|strtofflags)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, fflags_discarded, "API-FFLAGS", out);
    static const Regex getdirentries_discarded(R"(^\s*\bgetdirentries\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getdirentries_discarded, "API-GETDIRENTRIES", "getdirentries() return is discarded", out);
    static const Regex kinfo_discarded(R"(^\s*\b(?P<fn>kinfo_get(?:proc|file|vmmap))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, kinfo_discarded, "API-KINFO", out);
    static const Regex umtx_discarded(R"(^\s*\b_umtx_op\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, umtx_discarded, "API-UMTX", "_umtx_op() return is discarded", out);
    static const Regex thr_discarded(R"(^\s*\b(?P<fn>thr_(?:new|kill2|kill|self|exit))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, thr_discarded, "API-THR", out);
    static const Regex modfind_discarded(R"(^\s*\b(?P<fn>mod(?:find|stat|next|fnext))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, modfind_discarded, "API-MODFIND", out);
    static const Regex lpathconf_discarded(R"(^\s*\blpathconf\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, lpathconf_discarded, "API-LPATHCONF", "lpathconf() return is discarded", out);
    static const Regex loginclass_discarded(R"(^\s*\b(?P<fn>(?:get|set)loginclass)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, loginclass_discarded, "API-LOGINCLASS", out);
    static const Regex getfsent_discarded(R"(^\s*\b(?P<fn>(?:get|set|end)fsent)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, getfsent_discarded, "API-GETFSENT", out);
    static const Regex minherit_discarded(R"(^\s*\bminherit\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, minherit_discarded, "API-MINHERIT", "minherit() return is discarded", out);
    static const Regex cap_getmode_discarded(R"(^\s*\bcap_getmode\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, cap_getmode_discarded, "API-CAP-GETMODE", "cap_getmode() return is discarded", out);
    static const Regex nfssvc_discarded(R"(^\s*\bnfssvc\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, nfssvc_discarded, "API-NFSSVC", "nfssvc() return is discarded", out);
    static const Regex sysarch_discarded(R"(^\s*\bsysarch\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, sysarch_discarded, "API-SYSARCH", "sysarch() return is discarded", out);
    static const Regex getpagesizes_discarded(R"(^\s*\bgetpagesizes\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getpagesizes_discarded, "API-GETPAGESIZES", "getpagesizes() return is discarded", out);
    static const Regex sbrk_discarded(R"(^\s*\b(?P<fn>sbrk|brk)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, sbrk_discarded, "API-SBRK", out);
    static const Regex ksem_discarded(R"(^\s*\b(?P<fn>ksem_(?:open|close|unlink|wait|post))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, ksem_discarded, "API-KSEM", out);
    static const Regex cap_getrights_discarded(R"(^\s*\bcap_getrights\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, cap_getrights_discarded, "API-CAP-GETRIGHTS", "cap_getrights() return is discarded", out);
    static const Regex devname_discarded(R"(^\s*\b(?P<fn>devname(?:_r)?)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, devname_discarded, "API-DEVNAME", out);
    static const Regex getbootfile_discarded(R"(^\s*\bgetbootfile\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getbootfile_discarded, "API-GETBOOTFILE", "getbootfile() return is discarded", out);
    static const Regex kldfirstmod_discarded(R"(^\s*\b(?P<fn>kld(?:firstmod|nextmod))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, kldfirstmod_discarded, "API-KLDFIRSTMOD", out);
    static const Regex fhlink_discarded(R"(^\s*\b(?P<fn>fh(?:linkat|link|readlink))\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_fn(funcs, lines, rel, fhlink_discarded, "API-FHLINK", out);
    static const Regex valloc_discarded(R"(^\s*\bvalloc\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, valloc_discarded, "API-VALLOC", "valloc() return is discarded", out);
    static const Regex getdomainname_discarded(R"(^\s*\bgetdomainname\s*\([^;]*\)\s*;\s*$)");
    lint_discarded(funcs, lines, rel, getdomainname_discarded, "API-GETDOMAINNAME", "getdomainname() return is discarded", out);
    static const Regex unlink_discarded(R"(^\s*(?:unlink|remove)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, unlink_discarded, "API-UNLINK", {"unlink", "remove"}, out);
    static const Regex mkfifo_discarded(R"(^\s*(?:mkfifo|mknod)\s*\([^;]*\)\s*;\s*$)");
    lint_discarded_which(funcs, lines, rel, mkfifo_discarded, "API-MKFIFO", {"mkfifo", "mknod"}, out);
}

}  // namespace prism
