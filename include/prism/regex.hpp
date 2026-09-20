#pragma once

#include "prism/export.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

struct Match {
    std::string text;
    std::vector<std::optional<std::string>> groups;  // [0]=full, then 1..
    std::vector<std::pair<int, int>> spans;          // start,end per group
    std::map<std::string, std::string> named_groups;
    std::string named(std::string_view name) const;
    std::string group(std::size_t i) const;
};

// PCRE2 wrapper. Python `(?P<name>...)`, `(?m)`, lookaround all work.
class PRISM_API Regex {
public:
    Regex() = default;
    explicit Regex(std::string pattern, bool multiline = false, bool dotall = false);
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;
    Regex(Regex&&) noexcept;
    Regex& operator=(Regex&&) noexcept;
    ~Regex();

    bool valid() const { return code_ != nullptr; }
    const std::string& pattern() const { return pattern_; }

    bool search(std::string_view s) const;
    std::optional<Match> search_match(std::string_view s, std::size_t offset = 0) const;
    std::vector<Match> finditer(std::string_view s) const;
    bool match_line(std::string_view s) const;  // like Python re.match (start of string)

private:
    std::string pattern_;
    void* code_ = nullptr;  // pcre2_code*
};

PRISM_API bool re_search(std::string_view pattern, std::string_view s, bool multiline = false);
PRISM_API std::optional<Match> re_search_match(std::string_view pattern, std::string_view s);

}  // namespace prism
