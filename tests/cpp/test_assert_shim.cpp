// PIR's Windows <assert.h> is a C header. Read it as text; do not compile it.
// A failed assert calls __assert_fail. A function body that returns after that
// call would fall through, and this case fails if the macro is replaced by one.

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void skip_string(std::string_view s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"') return;
    ++i;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) i += 2;
        else ++i;
    }
    if (i < s.size()) ++i;
}

bool has_word(std::string_view s, std::string_view word) {
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '"') {
            skip_string(s, i);
            continue;
        }
        if (ident_char(s[i])) {
            const auto start = i;
            while (i < s.size() && ident_char(s[i])) ++i;
            if (s.substr(start, i - start) == word) return true;
            continue;
        }
        ++i;
    }
    return false;
}

std::string strip_comments(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == '"') {
            const auto start = i;
            skip_string(in, i);
            out.append(in.substr(start, i - start));
            continue;
        }
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            if (i + 1 < in.size()) i += 2;
            out.push_back(' ');
            continue;
        }
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') ++i;
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

// Replacement list of `#define assert`, with backslash-newline spliced out.
// Empty when the header no longer defines that macro.
std::string assert_macro(std::string_view code) {
    constexpr std::string_view key = "#define assert";
    const auto at = code.find(key);
    if (at == std::string_view::npos) return {};
    const auto next = at + key.size();
    if (next < code.size()) {
        const unsigned char c = static_cast<unsigned char>(code[next]);
        if (!std::isspace(c) && code[next] != '(') return {};
    }
    std::size_t start = next;
    if (start < code.size() && code[start] == '(') {
        int depth = 0;
        for (std::size_t j = start; j < code.size(); ++j) {
            if (code[j] == '(') ++depth;
            else if (code[j] == ')') {
                --depth;
                if (depth == 0) {
                    start = j + 1;
                    break;
                }
            }
        }
    }
    std::string body;
    for (std::size_t k = start; k < code.size(); ++k) {
        if (code[k] == '\\' && k + 1 < code.size() && (code[k + 1] == '\n' || code[k + 1] == '\r')) {
            body.push_back(' ');
            ++k;
            if (k + 1 < code.size() && code[k] == '\r' && code[k + 1] == '\n') ++k;
            continue;
        }
        if (code[k] == '\n') break;
        if (code[k] != '\r') body.push_back(code[k]);
    }
    return body;
}

bool calls_assert_fail(std::string_view s) {
    constexpr std::string_view name = "__assert_fail";
    for (std::size_t pos = 0; (pos = s.find(name, pos)) != std::string_view::npos; pos += name.size()) {
        const bool left = pos == 0 || !ident_char(s[pos - 1]);
        auto i = pos + name.size();
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (left && i < s.size() && s[i] == '(') return true;
    }
    return false;
}

std::size_t prev_non_ws(std::string_view s, std::size_t i) {
    while (i > 0) {
        --i;
        if (!std::isspace(static_cast<unsigned char>(s[i]))) return i;
    }
    return std::string_view::npos;
}

std::string_view word_ending_at(std::string_view s, std::size_t last) {
    if (last == std::string_view::npos || !ident_char(s[last])) return {};
    std::size_t start = last;
    while (start > 0 && ident_char(s[start - 1])) --start;
    return s.substr(start, last - start + 1);
}

std::size_t match_brace(std::string_view code, std::size_t open) {
    int depth = 0;
    std::size_t j = open;
    while (j < code.size()) {
        if (code[j] == '"') {
            skip_string(code, j);
            continue;
        }
        if (code[j] == '{') ++depth;
        else if (code[j] == '}') {
            --depth;
            if (depth == 0) return j;
        }
        ++j;
    }
    return std::string_view::npos;
}

std::size_t match_paren_back(std::string_view code, std::size_t close) {
    int depth = 0;
    std::size_t j = close;
    while (true) {
        if (code[j] == ')') ++depth;
        else if (code[j] == '(') {
            --depth;
            if (depth == 0) return j;
        }
        if (j == 0) return std::string_view::npos;
        --j;
    }
}

bool marked_noreturn(std::string_view decl) {
    return has_word(decl, "noreturn") || decl.find("_Noreturn") != std::string_view::npos;
}

// Index of the `)` that closes the `__assert_fail(...)` call at `name_pos`.
std::size_t assert_fail_call_end(std::string_view s, std::size_t name_pos) {
    std::size_t i = name_pos + std::string_view("__assert_fail").size();
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size() || s[i] != '(') return std::string_view::npos;
    int depth = 0;
    while (i < s.size()) {
        if (s[i] == '"') {
            skip_string(s, i);
            continue;
        }
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') {
            --depth;
            if (depth == 0) return i;
        }
        ++i;
    }
    return std::string_view::npos;
}

// A function whose body calls __assert_fail and then returns, including
// falling off the closing brace when the function is not noreturn.
bool function_returns_after_failure(std::string_view code) {
    for (std::size_t i = 0; i < code.size();) {
        if (code[i] == '"') {
            skip_string(code, i);
            continue;
        }
        if (code[i] != '{') {
            ++i;
            continue;
        }
        const auto end = match_brace(code, i);
        if (end == std::string_view::npos) return true;
        const auto before = prev_non_ws(code, i);
        const bool after_paren = before != std::string_view::npos && code[before] == ')';
        std::string_view name;
        if (after_paren) {
            const auto open = match_paren_back(code, before);
            if (open != std::string_view::npos) name = word_ending_at(code, prev_non_ws(code, open));
        }
        const bool function = after_paren && name != "if" && name != "while" && name != "for" && name != "switch";
        if (function) {
            const auto body = code.substr(i + 1, end - i - 1);
            const auto fail = body.find("__assert_fail");
            if (fail != std::string_view::npos) {
                const auto call_end = assert_fail_call_end(body, fail);
                const auto after = call_end == std::string_view::npos ? body.substr(fail) : body.substr(call_end + 1);
                std::size_t decl_at = i;
                while (decl_at > 0 && code[decl_at - 1] != ';' && code[decl_at - 1] != '}') --decl_at;
                const auto decl = code.substr(decl_at, i - decl_at);
                if (has_word(after, "return") || !marked_noreturn(decl)) return true;
            }
        }
        i = end + 1;
    }
    return false;
}

}  // namespace

TEST_CASE("assert shim: failed assert calls __assert_fail and does not return") {
    const auto text = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "shim" / "assert.h");
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("__assert_fail") != std::string::npos);

    const auto code = strip_comments(text);
    const auto macro = assert_macro(code);
    REQUIRE(calls_assert_fail(macro));
    CHECK_FALSE(has_word(macro, "return"));
    CHECK_FALSE(function_returns_after_failure(code));
}
