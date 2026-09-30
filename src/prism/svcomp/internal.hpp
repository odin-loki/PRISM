#pragma once

// Internal to src/prism/svcomp/: text helpers shared by the SV-COMP
// subcommand's translation units. Text is handled as bytes; SV-COMP tasks
// are ASCII, and task text is read with invalid UTF-8 replaced so that the
// (UTF-mode) regular expressions always run.

#include "prism/regex.hpp"
#include "prism/svcomp.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::svcomp::detail {

// File contents with every invalid UTF-8 sequence replaced by U+FFFD
// ("" when unreadable).
std::string read_text(const std::filesystem::path& p);
std::string sanitize_utf8(std::string_view s);
bool write_text(const std::filesystem::path& p, const std::string& text);

// Whitespace as str.strip() sees it (ASCII part).
bool is_space(char c);
std::string strip(std::string_view s);
std::string lstrip(std::string_view s);
std::string rstrip(std::string_view s);
// text.split("\n")
std::vector<std::string> split_nl(std::string_view text);
// text.splitlines() (ASCII line boundaries)
std::vector<std::string> splitlines(std::string_view text);
bool ends_with_any(std::string_view s, std::string_view chars);

// Whole-string match (re.fullmatch); re must end in `\z` so that a
// shorter preferred match cannot hide a full one.
bool fullmatch(const Regex& re, std::string_view s);
// Match anchored at offset (re.match(text, pos)); spans are relative to text.
std::optional<Match> match_at(const Regex& re, std::string_view text, std::size_t pos = 0);
std::string re_escape(std::string_view s);

// str(value) of a JSON value the way the reports are read: a string as is,
// null as "None", anything else as its JSON text.
std::string pystr(const json& v);
// o.get(key) as a string ("" when missing and dflt not given).
std::string get_str(const json& o, const char* key, const std::string& dflt = "");
bool has(const json& o, const char* key);
const json& extra_of(const json& finding);
// Python truthiness of a JSON value.
bool truthy(const json& v);
// int(v): a JSON integer or a decimal string; nullopt otherwise.
std::optional<long long> to_int(const json& v);
// Decimal (optionally signed) integer, saturated at +-2^62.
std::optional<long long> parse_ll(std::string_view s);

}  // namespace prism::svcomp::detail
