#define PCRE2_CODE_UNIT_WIDTH 8
#include "prism/regex.hpp"

#include <pcre2.h>

#include <cstdint>

namespace prism {

static void free_code(void* p) {
    if (p) pcre2_code_free(static_cast<pcre2_code*>(p));
}

Regex::Regex(std::string pattern, bool multiline, bool dotall) : pattern_(std::move(pattern)) {
    int opts = PCRE2_UTF | PCRE2_UCP;
    if (multiline) opts |= PCRE2_MULTILINE;
    if (dotall) opts |= PCRE2_DOTALL;
    int err = 0;
    PCRE2_SIZE erroff = 0;
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern_.c_str()),
                          pattern_.size(), opts, &err, &erroff, nullptr);
}

Regex::Regex(Regex&& o) noexcept : pattern_(std::move(o.pattern_)), code_(o.code_) {
    o.code_ = nullptr;
}

Regex& Regex::operator=(Regex&& o) noexcept {
    if (this != &o) {
        free_code(code_);
        pattern_ = std::move(o.pattern_);
        code_ = o.code_;
        o.code_ = nullptr;
    }
    return *this;
}

Regex::~Regex() { free_code(code_); }

static void fill_named(Match& m, pcre2_code* code, pcre2_match_data* md) {
    uint32_t namecount = 0;
    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &namecount);
    if (!namecount) return;
    uint32_t entry_size = 0;
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
    PCRE2_SPTR table = nullptr;
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &table);
    for (uint32_t i = 0; i < namecount; ++i) {
        auto* entry = table + i * entry_size;
        int n = (entry[0] << 8) | entry[1];
        const char* name = reinterpret_cast<const char*>(entry + 2);
        PCRE2_UCHAR* buf = nullptr;
        PCRE2_SIZE len = 0;
        if (pcre2_substring_get_bynumber(md, static_cast<uint32_t>(n), &buf, &len) == 0) {
            m.named_groups[name] = std::string(reinterpret_cast<char*>(buf), len);
            pcre2_substring_free(buf);
        }
    }
}

static Match from_ovector(std::string_view s, pcre2_match_data* md, pcre2_code* code) {
    Match m;
    auto* ovec = pcre2_get_ovector_pointer(md);
    uint32_t n = pcre2_get_ovector_count(md);
    m.groups.resize(n);
    m.spans.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto a = ovec[2 * i];
        auto b = ovec[2 * i + 1];
        m.spans[i] = {static_cast<int>(a == PCRE2_UNSET ? -1 : a),
                      static_cast<int>(b == PCRE2_UNSET ? -1 : b)};
        if (a == PCRE2_UNSET) {
            m.groups[i] = std::nullopt;
        } else {
            m.groups[i] = std::string(s.substr(static_cast<std::size_t>(a),
                                               static_cast<std::size_t>(b - a)));
        }
    }
    if (!m.groups.empty() && m.groups[0]) m.text = *m.groups[0];
    fill_named(m, code, md);
    return m;
}

std::string Match::named(std::string_view name) const {
    auto it = named_groups.find(std::string(name));
    return it == named_groups.end() ? std::string{} : it->second;
}

std::string Match::group(std::size_t i) const {
    if (i >= groups.size() || !groups[i]) return {};
    return *groups[i];
}

bool Regex::search(std::string_view s) const { return search_match(s).has_value(); }

std::optional<Match> Regex::search_match(std::string_view s, std::size_t offset) const {
    if (!code_ || offset > s.size()) return std::nullopt;
    auto* code = static_cast<pcre2_code*>(code_);
    auto* md = pcre2_match_data_create_from_pattern(code, nullptr);
    int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(),
                         offset, 0, md, nullptr);
    std::optional<Match> out;
    if (rc >= 0) out = from_ovector(s, md, code);
    pcre2_match_data_free(md);
    return out;
}

std::vector<Match> Regex::finditer(std::string_view s) const {
    std::vector<Match> out;
    if (!code_) return out;
    std::size_t off = 0;
    while (off <= s.size()) {
        auto m = search_match(s, off);
        if (!m) break;
        out.push_back(*m);
        auto end = static_cast<std::size_t>(m->spans.empty() ? off + 1 : std::max(0, m->spans[0].second));
        if (end <= off) end = off + 1;
        off = end;
        if (off > s.size()) break;
    }
    return out;
}

bool Regex::match_line(std::string_view s) const {
    auto m = search_match(s, 0);
    return m && !m->spans.empty() && m->spans[0].first == 0;
}

bool re_search(std::string_view pattern, std::string_view s, bool multiline) {
    Regex re{std::string(pattern), multiline};
    return re.search(s);
}

std::optional<Match> re_search_match(std::string_view pattern, std::string_view s) {
    Regex re{std::string(pattern)};
    return re.search_match(s);
}

}  // namespace prism
