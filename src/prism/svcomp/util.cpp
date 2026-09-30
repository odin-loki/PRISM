#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace prism::svcomp::detail {

namespace fs = std::filesystem;

std::string sanitize_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto cont = [&](std::size_t k) { return k < n && (p[k] & 0xC0) == 0x80; };
    while (i < n) {
        unsigned char c = p[i];
        std::size_t len = 0;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = cont(i + 1) ? 2 : 0;
        else if (c >= 0xE0 && c <= 0xEF) {
            bool ok = cont(i + 1) && cont(i + 2);
            if (ok && c == 0xE0 && p[i + 1] < 0xA0) ok = false;  // overlong
            if (ok && c == 0xED && p[i + 1] > 0x9F) ok = false;  // surrogate
            len = ok ? 3 : 0;
        } else if (c >= 0xF0 && c <= 0xF4) {
            bool ok = cont(i + 1) && cont(i + 2) && cont(i + 3);
            if (ok && c == 0xF0 && p[i + 1] < 0x90) ok = false;
            if (ok && c == 0xF4 && p[i + 1] > 0x8F) ok = false;
            len = ok ? 4 : 0;
        }
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return sanitize_utf8(ss.str());
}

bool write_text(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
           (c >= '\x1c' && c <= '\x1f');
}

std::string lstrip(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() && is_space(s[a])) ++a;
    return std::string(s.substr(a));
}

std::string rstrip(std::string_view s) {
    std::size_t b = s.size();
    while (b > 0 && is_space(s[b - 1])) --b;
    return std::string(s.substr(0, b));
}

std::string strip(std::string_view s) { return lstrip(rstrip(s)); }

std::vector<std::string> split_nl(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        auto nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            return out;
        }
        out.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
}

std::vector<std::string> splitlines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0, i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= '\x1c' && c <= '\x1e')) {
            out.emplace_back(text.substr(start, i - start));
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = i + 1;
        }
        ++i;
    }
    if (start < text.size()) out.emplace_back(text.substr(start));
    return out;
}

bool ends_with_any(std::string_view s, std::string_view chars) {
    return !s.empty() && chars.find(s.back()) != std::string_view::npos;
}

bool fullmatch(const Regex& re, std::string_view s) {
    auto m = re.match_prefix(s);
    return m && !m->spans.empty() && static_cast<std::size_t>(m->spans[0].second) == s.size();
}

std::optional<Match> match_at(const Regex& re, std::string_view text, std::size_t pos) {
    if (pos > text.size()) return std::nullopt;
    auto m = re.match_prefix(text.substr(pos));
    if (!m) return std::nullopt;
    for (auto& sp : m->spans)
        if (sp.first >= 0) {
            sp.first += static_cast<int>(pos);
            sp.second += static_cast<int>(pos);
        }
    return m;
}

std::string re_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) out += '\\';
        out += c;
    }
    return out;
}

std::string pystr(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    return v.dump();
}

bool has(const json& o, const char* key) { return o.is_object() && o.contains(key); }

std::string get_str(const json& o, const char* key, const std::string& dflt) {
    if (!has(o, key)) return dflt;
    return pystr(o.at(key));
}

const json& extra_of(const json& finding) {
    static const json empty = json::object();
    if (!has(finding, "extra")) return empty;
    const auto& e = finding.at("extra");
    return e.is_object() ? e : empty;
}

bool truthy(const json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<long long>() != 0;
    if (v.is_number_unsigned()) return v.get<unsigned long long>() != 0;
    if (v.is_number_float()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.empty();
}

std::optional<long long> parse_ll(std::string_view s) {
    std::string t = strip(s);
    if (t.empty()) return std::nullopt;
    std::size_t i = 0;
    bool neg = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = t[0] == '-';
        i = 1;
    }
    if (i >= t.size()) return std::nullopt;
    constexpr long long cap = 1LL << 62;
    long long v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return std::nullopt;
        if (v < cap) v = std::min(cap, v * 10 + (t[i] - '0'));
    }
    return neg ? -v : v;
}

std::optional<long long> to_int(const json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (!std::isfinite(d)) return std::nullopt;
        return static_cast<long long>(std::trunc(d));
    }
    if (v.is_string()) return parse_ll(v.get<std::string>());
    return std::nullopt;
}

}  // namespace prism::svcomp::detail
