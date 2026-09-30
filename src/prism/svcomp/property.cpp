// SV-COMP property files and the ILP32 width check (`prism svcomp`).
#include "internal.hpp"

#include <algorithm>
#include <set>

namespace prism::svcomp {

using namespace detail;

namespace {

// Types whose width differs between ILP32 and LP64: PRISM's encoders are LP64.
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
        char ch = src[i];
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
        pat += re_escape(words[i]);
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
