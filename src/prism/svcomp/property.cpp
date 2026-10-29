// SV-COMP property files and the ILP32 width check (`prism svcomp`).
#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>

namespace prism::svcomp {

using namespace detail;

namespace {

// Types whose width differs between ILP32 and LP64 (bmc uses Config::data_model).
const std::vector<std::string> WIDTH_TYPES = {"long", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t"};

std::string sub(const Regex& re, const std::string& text, const std::string& repl) {
    std::string out;
    std::size_t last = 0;
    for (const auto& m : re.finditer(text)) {
        auto a = static_cast<std::size_t>(m.spans[0].first);
        auto b = static_cast<std::size_t>(m.spans[0].second);
        out.append(text, last, a - last);
        out += repl;
        last = b;
    }
    out.append(text, last, std::string::npos);
    return out;
}

std::string strip_comments(const std::string& src) {
    static const Regex block(R"(/\*.*?\*/)", false, true);
    static const Regex line(R"(//[^\n]*)");
    static const Regex str(R"("(\\.|[^"\\])*")");
    return sub(str, sub(line, sub(block, src, " "), " "), "\"\"");
}

// Bodies of top-level function definitions (`) {` at brace depth 0).
std::vector<std::string> function_bodies(const std::string& src) {
    std::vector<std::string> bodies;
    int depth = 0;
    long start = -1;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char ch = src.at(i);
        if (ch == '{') {
            if (depth == 0) {
                std::size_t from = i >= 200 ? i - 200 : 0;
                auto window = rstrip(std::string_view(src).substr(from, i - from));
                if (!window.empty() && window.back() == ')') start = static_cast<long>(i);
            }
            ++depth;
        } else if (ch == '}') {
            depth = std::max(0, depth - 1);
            if (depth == 0 && start >= 0) {
                bodies.push_back(src.substr(static_cast<std::size_t>(start), i + 1 - static_cast<std::size_t>(start)));
                start = -1;
            }
        }
    }
    return bodies;
}

Regex word_re(const std::vector<std::string>& words) {
    std::string pat = "\\b(";
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i) pat += '|';
        pat += re_escape(words.at(i));
    }
    pat += ")\\b";
    return Regex(pat);
}

}  // namespace

std::string parse_property(const std::string& prp_text) {
    static const std::vector<std::pair<std::string, Regex>> specs = [] {
        std::vector<std::pair<std::string, Regex>> v;
        v.emplace_back("no-overflow", Regex(R"(LTL\(\s*G\s*!\s*overflow\s*\))"));
        v.emplace_back("unreach-call", Regex(R"(LTL\(\s*G\s*!\s*call\(\s*reach_error\(\)\s*\)\s*\))"));
        v.emplace_back("valid-memsafety", Regex(R"(LTL\(\s*G\s*valid-(free|deref|memtrack)\s*\))"));
        return v;
    }();
    std::set<std::string> found;
    for (const auto& [name, rx] : specs)
        if (rx.search(prp_text)) found.insert(name);
    return found.size() == 1 ? *found.begin() : "unsupported";
}

bool supported_property(const std::string& prop) {
    return prop == "no-overflow" || prop == "unreach-call" || prop == "valid-memsafety";
}

std::optional<std::string> neutralize_error_calls(const std::string& src) {
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    auto skip_ws = [&](std::size_t i) {
        while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
        return i;
    };
    // the identifier ending just before position i (after spaces), or the punctuation char
    auto prev_token = [&](std::size_t i) -> std::string {
        while (i > 0 && std::isspace(static_cast<unsigned char>(src[i - 1]))) --i;
        if (i == 0) return ";";
        if (!ident(src[i - 1])) return std::string(1, src[i - 1]);
        std::size_t b = i;
        while (b > 0 && ident(src[b - 1])) --b;
        return src.substr(b, i - b);
    };
    std::string out;
    out.reserve(src.size());
    bool changed = false;
    std::size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '"' || src[i] == '\'') {  // literals copied as is
            char q = src[i];
            std::size_t j = i + 1;
            while (j < src.size() && src[j] != q) j += src[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, src.size());
            out.append(src, i, j - i);
            i = j;
            continue;
        }
        if (!ident(src[i]) || (i > 0 && ident(src[i - 1]))) {
            out += src[i++];
            continue;
        }
        std::size_t e = i;
        while (e < src.size() && ident(src[e])) ++e;
        const std::string name = src.substr(i, e - i);
        if (name == "exit") {
            // a definition of exit(): do not rewrite anything
            std::size_t p = skip_ws(e);
            if (p < src.size() && src[p] == '(') {
                std::size_t c = src.find(')', p);
                if (c != std::string::npos) {
                    std::size_t n = skip_ws(c + 1);
                    if (n < src.size() && src[n] == '{') return std::nullopt;
                }
            }
        }
        if (name == "reach_error" || name == "__VERIFIER_error" || name == "abort") {
            std::size_t p = skip_ws(e);
            std::size_t q = p < src.size() && src[p] == '(' ? skip_ws(p + 1) : std::string::npos;
            if (q != std::string::npos && q < src.size() && src[q] == ')') {
                std::size_t n = skip_ws(q + 1);
                const std::string prev = prev_token(i);
                // a call statement: `;` follows and a statement boundary precedes
                bool stmt = n < src.size() && src[n] == ';' &&
                            (prev == ";" || prev == "{" || prev == "}" || prev == ")" || prev == ":" ||
                             prev == "else" || prev == "do");
                if (stmt) {
                    out += "exit(0)";
                    changed = true;
                    i = q + 1;
                    continue;
                }
            }
        }
        out.append(src, i, e - i);
        i = e;
    }
    if (!changed) return std::nullopt;
    return "extern void exit(int); " + out;
}

bool width_dependent_code(const std::string& source) {
    std::string src = strip_comments(sanitize_utf8(source));
    // glibc's assert() expands to `(void) sizeof (cond)`: its value is discarded.
    static const Regex void_sizeof(R"(\(\s*void\s*\)\s*sizeof\b)");
    src = sub(void_sizeof, src, "(void)");
    static const Regex typedef_re(R"(\btypedef\b([^;]*?)\b(\w+)\s*(?:\[[^\]]*\])?\s*;)", false, true);
    static const Regex aggregate_re(R"(\b(?:struct|union)\s+(\w+)\s*\{([^{}]*)\})", false, true);
    std::vector<std::pair<std::string, std::string>> typedefs;    // (body, name)
    std::vector<std::pair<std::string, std::string>> aggregates;  // (tag, body)
    for (const auto& m : typedef_re.finditer(src)) typedefs.emplace_back(m.group(1), m.group(2));
    for (const auto& m : aggregate_re.finditer(src)) aggregates.emplace_back(m.group(1), m.group(2));
    std::set<std::string> names;
    auto words = [&](bool with_sizeof) {
        std::vector<std::string> w = WIDTH_TYPES;
        if (with_sizeof) w.emplace_back("sizeof");
        w.insert(w.end(), names.begin(), names.end());  // std::set: sorted
        return w;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        Regex pat = word_re(words(false));
        for (const auto& [body, name] : typedefs)
            if (!names.contains(name) && pat.search(body)) {
                names.insert(name);
                changed = true;
            }
        for (const auto& [tag, body] : aggregates)
            if (!names.contains(tag) && pat.search(body)) {
                names.insert(tag);
                changed = true;
            }
    }
    Regex pat = word_re(words(true));
    for (const auto& b : function_bodies(src))
        if (pat.search(b)) return true;
    return false;
}

}  // namespace prism::svcomp
