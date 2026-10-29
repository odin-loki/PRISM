#pragma once
// Model headers carry their honesty notes as wrapped // or /* */ comments, and
// long calls are wrapped across lines. Tests that look for a sentence or a call
// search the text with comment continuation markers removed and every run of
// whitespace collapsed to one space, so a rewrap does not break the check.
#include <string>

namespace prism_test {
inline std::string unwrap_prose(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    std::size_t line_start = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            std::size_t j = i + 1;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
            const bool line_had_comment = s.substr(line_start, i - line_start).find("//") != std::string::npos;
            line_start = i + 1;
            if (line_had_comment && j + 1 < s.size() && s[j] == '/' && s[j + 1] == '/') {
                j += 2;  // a // comment continued on the next line
            } else if (j < s.size() && s[j] == '*' && (j + 1 >= s.size() || s[j + 1] != '/')) {
                j += 1;  // a /* */ comment continued with a leading " * "
            }
            if (out.empty() || out.back() != ' ') out.push_back(' ');
            i = j;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (out.empty() || out.back() != ' ') out.push_back(' ');
            ++i;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}
}  // namespace prism_test
