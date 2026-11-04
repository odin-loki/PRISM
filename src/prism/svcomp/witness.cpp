// SV-COMP witness format 2.0 writer (include/prism/svcomp_witness.hpp).
#include "internal.hpp"

#include "prism/config.hpp"
#include "prism/wide_int.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <random>

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

std::string int_text(i128 v) { return prism::format_i128(v); }

std::string float_repr(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string sci(buf, res.ptr);  // "-1.2345e+17": the shortest digits that read back as v
    std::string sign;
    if (sci.front() == '-') {
        sign = "-";
        sci.erase(0, 1);
    }
    auto e = sci.find('e');
    std::string mant = sci.substr(0, e);
    int exp = std::stoi(sci.substr(e + 1));
    std::string digits;
    for (char c : mant)
        if (c != '.') digits += c;
    // repr(float): positional for 1e-4 <= |v| < 1e16, else d.ddde+XX
    if (exp >= -4 && exp < 16) {
        std::string out;
        if (exp >= 0) {
            std::string ip = digits.substr(0, std::min<std::size_t>(digits.size(), static_cast<std::size_t>(exp) + 1));
            while (ip.size() < static_cast<std::size_t>(exp) + 1) ip += '0';
            std::string fp = digits.size() > static_cast<std::size_t>(exp) + 1 ? digits.substr(static_cast<std::size_t>(exp) + 1) : "0";
            out = ip + "." + fp;
        } else {
            out = "0." + std::string(static_cast<std::size_t>(-exp - 1), '0') + digits;
        }
        return sign + out;
    }
    std::string out = digits.substr(0, 1);
    if (digits.size() > 1) out += "." + digits.substr(1);
    char eb[16];
    std::snprintf(eb, sizeof eb, "e%c%02d", exp < 0 ? '-' : '+', std::abs(exp));
    return sign + out + eb;
}

std::string acsl_literal(const Num& v) { return v.is_float ? float_repr(v.f) : int_text(v.i); }

ojson Location::as_dict() const {
    ojson d = ojson::object();
    d["file_name"] = file_name;
    d["line"] = line;
    if (column) d["column"] = std::max(1L, *column);
    if (function && !function->empty()) d["function"] = *function;
    return d;
}

namespace {

std::string uuid4() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    unsigned char b[16];
    for (int i = 0; i < 16; i += 8) {
        auto x = gen();
        for (int k = 0; k < 8; ++k) b[i + k] = static_cast<unsigned char>(x >> (8 * k));
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    char out[37];
    std::snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out, sizeof out - 1);
}

std::string now_utc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    if (!detail::utc_tm(t, &tm)) return "1970-01-01T00:00:00Z";
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return n ? std::string(buf, n) : "1970-01-01T00:00:00Z";
}

ojson metadata(const WitnessMeta& m) {
    std::ifstream in(m.input_file, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ojson task = ojson::object();
    task["input_files"] = ojson::array({m.input_file_name});
    task["input_file_hashes"] = ojson::object({{m.input_file_name, sha256_hex(data)}});
    task["specification"] = strip(m.specification);
    task["data_model"] = m.data_model;
    task["language"] = m.language;
    ojson md = ojson::object();
    md["format_version"] = WITNESS_FORMAT_VERSION;
    md["uuid"] = m.uuid.value_or(uuid4());
    md["creation_time"] = m.creation_time.value_or(now_utc());
    ojson producer = ojson::object();
    producer["name"] = WITNESS_PRODUCER;
    producer["version"] = m.producer_version;
    md["producer"] = producer;
    md["task"] = task;
    return md;
}

ojson segment(const ojson& waypoint) {
    ojson wp = ojson::object();
    wp["waypoint"] = waypoint;
    ojson seg = ojson::object();
    seg["segment"] = ojson::array({wp});
    return seg;
}

}  // namespace

ojson build_violation_witness(const Counterexample& cex, const WitnessMeta& meta) {
    ojson segments = ojson::array();
    for (const auto& nv : cex.nondet) {
        ojson wp = ojson::object();
        wp["type"] = "function_return";
        wp["action"] = "follow";
        wp["location"] = nv.location.as_dict();
        // format 2.0: `\result <op> <constant>` in ACSL (not a C expression)
        ojson constraint = ojson::object();
        constraint["value"] = "\\result == " + acsl_literal(nv.value);
        constraint["format"] = "acsl_expression";
        wp["constraint"] = constraint;
        segments.push_back(segment(wp));
    }
    ojson target = ojson::object();
    target["type"] = "target";
    target["action"] = "follow";
    target["location"] = cex.target.as_dict();
    segments.push_back(segment(target));
    ojson entry = ojson::object();
    entry["entry_type"] = "violation_sequence";
    entry["metadata"] = metadata(meta);
    entry["content"] = segments;
    return ojson::array({entry});
}

ojson build_correctness_witness(const std::vector<Invariant>& invariants, const WitnessMeta& meta) {
    ojson content = ojson::array();
    for (const auto& inv : invariants) {
        ojson i = ojson::object();
        i["type"] = inv.kind;
        i["location"] = inv.location.as_dict();
        i["value"] = inv.value;
        i["format"] = "c_expression";
        ojson e = ojson::object();
        e["invariant"] = i;
        content.push_back(e);
    }
    ojson entry = ojson::object();
    entry["entry_type"] = "invariant_set";
    entry["metadata"] = metadata(meta);
    entry["content"] = content;
    return ojson::array({entry});
}

// ---------------------------------------------------------------- YAML

namespace {

// Bytes of a well-formed UTF-8 sequence at i, or 0. Overlongs and
// surrogates are not well-formed. Same acceptance as sanitize_utf8.
std::size_t utf8_seq_len(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i >= n) return 0;
    const auto b = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) {
        return k < n && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
    };
    if (b < 0x80) return 1;
    if (b >= 0xC2 && b <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (b >= 0xE0 && b <= 0xEF) {
        if (!(cont(i + 1) && cont(i + 2))) return 0;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if (b == 0xE0 && b1 < 0xA0) return 0;
        if (b == 0xED && b1 > 0x9F) return 0;
        return 3;
    }
    if (b >= 0xF0 && b <= 0xF4) {
        if (!(cont(i + 1) && cont(i + 2) && cont(i + 3))) return 0;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if (b == 0xF0 && b1 < 0x90) return 0;
        if (b == 0xF4 && b1 > 0x8F) return 0;
        return 4;
    }
    return 0;
}

char32_t utf8_cp(const std::string& s, std::size_t i, std::size_t len) {
    // Callers pass a length from utf8_seq_len; never read past the string.
    if (i >= s.size() || len > s.size() - i) return 0;
    auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    if (len == 1) return b(0);
    if (len == 2) return (static_cast<char32_t>(b(0) & 0x1F) << 6) | (b(1) & 0x3F);
    if (len == 3)
        return (static_cast<char32_t>(b(0) & 0x0F) << 12) | (static_cast<char32_t>(b(1) & 0x3F) << 6) | (b(2) & 0x3F);
    if (len == 4)
        return (static_cast<char32_t>(b(0) & 0x07) << 18) | (static_cast<char32_t>(b(1) & 0x3F) << 12) |
               (static_cast<char32_t>(b(2) & 0x3F) << 6) | (b(3) & 0x3F);
    return 0;
}

void append_hex_byte(std::string& out, unsigned char c) {
    static constexpr char hex[] = "0123456789abcdef";
    out += "\\x";
    out += hex[c >> 4];
    out += hex[c & 0x0F];
}

// Plain identifiers only. A colon, quote, newline, backslash, leading
// indicator (anything other than a letter or `_`), or a non-ASCII byte is
// not safe to emit unquoted even if a regex would accept it.
bool yaml_plain_safe(const std::string& s) {
    if (s.empty()) return false;
    const auto c0 = static_cast<unsigned char>(s[0]);
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) return false;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == ':' || c == '"' || c == '\\' || c == '\n' || c == '\r' || c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// Double-quoted YAML. Newlines stay `\n`. CR, tab and the Unicode line
// breaks are escaped too: a raw break inside quotes is a folded line, so
// the witness line would split. Well-formed UTF-8 otherwise passes through.
// A byte that is not well-formed UTF-8 is `\xHH` (the task reader accepts
// that escape). Nothing is dropped.
std::string quoted_scalar(const std::string& s) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '\\') {
            out += "\\\\";
            ++i;
            continue;
        }
        if (c == '"') {
            out += "\\\"";
            ++i;
            continue;
        }
        if (c == '\n') {
            out += "\\n";
            ++i;
            continue;
        }
        if (c == '\r') {
            out += "\\r";
            ++i;
            continue;
        }
        if (c == '\t') {
            out += "\\t";
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            append_hex_byte(out, c);
            ++i;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        const std::size_t len = utf8_seq_len(s, i);
        if (len == 0) {
            append_hex_byte(out, c);
            ++i;
            continue;
        }
        const char32_t cp = utf8_cp(s, i, len);
        if (cp == 0x85 || cp == 0x2028 || cp == 0x2029) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(cp));
            out += buf;
        } else {
            out.append(s.data() + i, len);
        }
        i += len;
    }
    out += '"';
    return out;
}

std::string scalar(const ojson& v) {
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return float_repr(v.get<double>());
    std::string s;
    if (v.is_string()) s = v.get<std::string>();
    else if (v.is_null()) s = "None";
    else if (v.is_object()) s = "{}";
    else if (v.is_array()) s = "[]";
    static const Regex plain(R"([A-Za-z_][A-Za-z0-9_.\-/]*\z)");
    static const std::vector<std::string> reserved = {"true", "false", "null", "yes", "no", "on", "off", "~", "y", "n"};
    if (yaml_plain_safe(s) && fullmatch(plain, s)) {
        std::string low;
        for (char c : s) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::ranges::find(reserved, low) == reserved.end()) return s;
    }
    return quoted_scalar(s);
}

bool nonempty_container(const ojson& v) { return (v.is_object() || v.is_array()) && !v.empty(); }

void emit(const ojson& v, int indent, std::vector<std::string>& out, bool in_list = false) {
    const std::string pad(static_cast<std::size_t>(2 * indent), ' ');
    if (v.is_object()) {
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            const std::string lead = (in_list && first) ? "" : pad;
            first = false;
            const std::string key = scalar(ojson(it.key()));
            const ojson& val = it.value();
            if (nonempty_container(val)) {
                out.push_back(lead + key + ":");
                emit(val, indent + 1, out);
            } else if (val.is_object()) {
                out.push_back(lead + key + ": {}");
            } else if (val.is_array()) {
                out.push_back(lead + key + ": []");
            } else {
                out.push_back(lead + key + ": " + scalar(val));
            }
        }
    } else if (v.is_array()) {
        for (const auto& item : v) {
            if (nonempty_container(item)) {
                std::vector<std::string> sub;
                emit(item, indent + 1, sub, true);
                out.push_back(pad + "- " + sub.front());
                out.insert(out.end(), sub.begin() + 1, sub.end());
            } else {
                out.push_back(pad + "- " + scalar(item));
            }
        }
    } else {
        out.push_back(pad + scalar(v));
    }
}

}  // namespace

std::string to_yaml(const ojson& doc) {
    std::vector<std::string> out;
    emit(doc, 0, out);
    std::string text;
    for (std::size_t i = 0; i < out.size(); ++i) text += (i ? "\n" : "") + out.at(i);
    return text + "\n";
}

bool write_witness(const ojson& doc, const fs::path& path) { return write_text(path, to_yaml(doc)); }

}  // namespace prism::svcomp
