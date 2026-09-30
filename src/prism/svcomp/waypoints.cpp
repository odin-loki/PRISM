// Witness waypoints (`prism svcomp`): where the nondet calls of a
// counterexample and the violation are in the task text.
#include "internal.hpp"

#include <algorithm>

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

namespace {

// `# 12 "file.c"` / `#line 12`: debug lines then name another file's lines
const Regex& line_marker() {
    static const Regex re(R"(^[ \t]*#[ \t]*(line[ \t]+)?\d+)", true);
    return re;
}

// `int f()` / `extern unsigned f()`: a type name right before the
// identifier (a call follows `=`, `(`, `,`, an operator or `return`).
bool is_declaration(const std::string& text, std::size_t start) {
    auto nl = start == 0 ? std::string::npos : text.rfind('\n', start - 1);
    std::size_t from = nl == std::string::npos ? 0 : nl + 1;
    std::string before = rstrip(std::string_view(text).substr(from, start - from));
    static const Regex word(R"((\w+)\s*\**$)");
    auto m = word.search_match(before);
    return m && m->group(1) != "return" && !ends_with_any(before, "=(,");
}

// (line, column) of the character just before offset end (the call's `)`).
LineCol paren_location(const std::string& text, std::size_t end) {
    long line = static_cast<long>(std::count(text.begin(), text.begin() + static_cast<long>(end), '\n')) + 1;
    auto nl = end == 0 ? std::string::npos : text.rfind('\n', end - 1);
    long line_start = nl == std::string::npos ? 0 : static_cast<long>(nl) + 1;
    return {line, static_cast<long>(end) - line_start};
}

}  // namespace

std::optional<std::vector<std::optional<LineCol>>> nondet_locations(const json& f, std::size_t count) {
    const json& extra = extra_of(f);
    if (!has(extra, "nondet_loc")) return std::nullopt;
    std::string raw = pystr(extra.at("nondet_loc"));
    static const Regex pair_re(R"((\d+):(\d+)\z)");
    std::vector<std::optional<LineCol>> out;
    std::size_t start = 0;
    for (;;) {
        auto comma = raw.find(',', start);
        std::string part = strip(std::string_view(raw).substr(start, comma == std::string::npos ? std::string::npos
                                                                                               : comma - start));
        if (!part.empty()) {
            auto m = pair_re.match_prefix(part);
            if (!m) return std::nullopt;
            long line = static_cast<long>(parse_ll(m->group(1)).value_or(0));
            long col = static_cast<long>(parse_ll(m->group(2)).value_or(0));
            if (line > 0 && col > 0) out.emplace_back(LineCol{line, col});
            else out.emplace_back(std::nullopt);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (out.size() != count) return std::nullopt;
    return out;
}

std::optional<LineCol> call_at(const std::string& text, const std::string& name, long line, long col) {
    auto lines = split_nl(text);
    if (line < 1 || line > static_cast<long>(lines.size()) || col < 1 ||
        col > static_cast<long>(lines[static_cast<std::size_t>(line - 1)].size()))
        return std::nullopt;
    std::size_t start = 0;
    for (long k = 0; k < line - 1; ++k) start += lines[static_cast<std::size_t>(k)].size() + 1;
    start += static_cast<std::size_t>(col - 1);
    if (start > 0) {
        unsigned char c = static_cast<unsigned char>(text[start - 1]);
        if (std::isalnum(c) || c == '_') return std::nullopt;
    }
    Regex re(re_escape(name) + R"(\s*\(\s*\))");
    auto m = match_at(re, text, start);
    if (!m) return std::nullopt;
    return paren_location(text, static_cast<std::size_t>(m->spans[0].second));
}

std::vector<NondetValue> nondet_waypoints(const fs::path& task, const std::vector<std::pair<std::string, Num>>& trace,
                                          std::optional<std::vector<std::optional<LineCol>>> locs, bool physical) {
    const std::string text = read_text(task);
    if (locs && (locs->size() != trace.size() || (!physical && line_marker().search(text)))) locs.reset();
    std::vector<NondetValue> out;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const auto& [name, value] = trace[i];
        std::optional<LineCol> at = locs ? (*locs)[i] : std::nullopt;
        std::optional<LineCol> pos = at ? call_at(text, name, at->first, at->second) : std::nullopt;
        if (!pos) {
            Regex re("\\b" + re_escape(name) + R"(\s*\(\s*\))");
            std::vector<Match> sites;
            for (auto& m : re.finditer(text))
                if (!is_declaration(text, static_cast<std::size_t>(m.spans[0].first))) sites.push_back(std::move(m));
            if (sites.size() != 1) break;
            // just past ')': format 2.0 points at the closing parenthesis
            pos = paren_location(text, static_cast<std::size_t>(sites[0].spans[0].second));
        }
        out.push_back(NondetValue{Location{task.filename().string(), pos->first, pos->second, std::nullopt}, value});
    }
    return out;
}

std::optional<LineCol> reach_error_call_site(const fs::path& src, std::optional<long> hint) {
    static const Regex call(R"(\breach_error\s*\(\s*\))");
    static const Regex definition(R"(\breach_error\s*\(\s*(void)?\s*\)\s*\{)");
    std::vector<LineCol> sites;
    auto lines = splitlines(read_text(src));
    for (std::size_t n = 0; n < lines.size(); ++n) {
        const auto& text = lines[n];
        for (const auto& m : call.finditer(text)) {
            auto start = static_cast<std::size_t>(m.spans[0].first);
            if (definition.search(std::string_view(text).substr(start))) continue;  // the definition
            sites.emplace_back(static_cast<long>(n) + 1, static_cast<long>(start) + 1);
        }
    }
    if (sites.size() == 1) return sites.front();
    std::vector<LineCol> same;
    for (const auto& s : sites)
        if (hint && s.first == *hint) same.push_back(s);
    if (same.size() == 1) return same.front();
    return std::nullopt;
}

long first_code_column(const fs::path& src, long line) {
    auto lines = splitlines(read_text(src));
    if (line >= 1 && line <= static_cast<long>(lines.size())) {
        const auto& s = lines[static_cast<std::size_t>(line - 1)];
        return static_cast<long>(s.size() - lstrip(s).size()) + 1;
    }
    return 1;
}

// `int x`, `unsigned long *p`, `struct s v[3]`: a declarator, not an lvalue like `y`, `*p`, `a[i]`
bool decl_head(const std::string& text) {
    static const Regex re(R"(^[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*[\s*]+[A-Za-z_]\w*\s*(?:\[[^\]]*\]\s*)*$)");
    return re.match_prefix(text).has_value();
}

// Format 2.0 puts the target at the first character of the statement or
// full expression whose evaluation the violation ends: an expression
// statement's start, a declaration's initializer, the controlling
// expression of if/while/switch, the expression of return. Without a
// column the target is "the first statement or full expression in that
// line", so a statement that is the first on its line gets no column:
// validators differ on parenthesised starts (UAutomizer 0.3.1 matches
// `int x = (a + 1) - 2;` at `a` or without a column, not at `(`). A later
// statement on the line gets its start column (past any opening
// parentheses). Anything this cannot place on one line (for headers,
// several declarators, a statement that begins on an earlier line) keeps
// the sanitizer's column.
std::optional<long> target_column(const fs::path& src, long line, long col) {
    auto lines = split_nl(read_text(src));
    if (line < 1 || line > static_cast<long>(lines.size())) return col;
    const std::string& text = lines[static_cast<std::size_t>(line - 1)];
    const std::size_t p = static_cast<std::size_t>(std::min<long>(std::max<long>(col - 1, 0), static_cast<long>(text.size())));
    static const Regex for_re(R"(\bfor\s*\()");
    const std::string head = text.substr(0, p);
    for (const auto& fm : for_re.finditer(head)) {
        auto seg = std::string_view(text).substr(static_cast<std::size_t>(fm.spans[0].second) - 1,
                                                 p - (static_cast<std::size_t>(fm.spans[0].second) - 1));
        if (std::count(seg.begin(), seg.end(), '(') > std::count(seg.begin(), seg.end(), ')'))
            return col;  // inside a for header: its ';' are not statement ends
    }
    long cut = -1;
    for (char ch : std::string_view(";{}")) {
        auto k = p == 0 ? std::string::npos : text.rfind(ch, p - 1);
        if (k != std::string::npos) cut = std::max(cut, static_cast<long>(k));
    }
    if (cut < 0) {
        // the statement starts on this line only if the previous code line ends one
        std::string prev;
        for (long k = line - 2; k >= 0; --k) {
            auto s = strip(lines[static_cast<std::size_t>(k)]);
            if (!s.empty()) {
                prev = s;
                break;
            }
        }
        if (!prev.empty() && !ends_with_any(prev, ";{}") && prev.front() != '#') return col;
    }
    std::size_t s = static_cast<std::size_t>(cut + 1);
    while (s < p && (text[s] == ' ' || text[s] == '\t')) ++s;
    const std::string stmt = text.substr(s, p > s ? p - s : 0);
    static const Regex for_start(R"(for\s*\()");
    if (stmt.starts_with("for") && for_start.match_prefix(stmt)) return col;
    if (strip(std::string_view(text).substr(0, s)).empty()) return std::nullopt;  // the first statement on its line
    static const Regex control(R"(^(if|while|switch)\s*\()");
    static const Regex ret(R"(return\b)");
    static const Regex assign(R"((?<![=!<>+\-*/%&|^])=(?!=))");
    if (auto m = control.match_prefix(stmt)) {
        s += static_cast<std::size_t>(m->spans[0].second);
    } else if (ret.match_prefix(stmt)) {
        s += 6;
    } else if (auto eq = assign.search_match(stmt)) {
        auto a = static_cast<std::size_t>(eq->spans[0].first);
        auto b = static_cast<std::size_t>(eq->spans[0].second);
        std::string lhs = stmt.substr(0, a);
        if (decl_head(strip(lhs)) && lhs.find(',') == std::string::npos) {
            if (stmt.find(',', b) != std::string::npos) return col;  // several declarators, or a comma inside the initializer
            s += b;
        }
    }
    while (s < p && (text[s] == ' ' || text[s] == '\t' || text[s] == '(')) ++s;  // past opening parentheses
    return static_cast<long>(s) + 1;
}

}  // namespace prism::svcomp
