// SV-COMP witness format 2.0 writer (include/prism/svcomp_witness.hpp).
#include "internal.hpp"

#include "prism/config.hpp"

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

std::string int_text(__int128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? static_cast<unsigned __int128>(-(v + 1)) + 1 : static_cast<unsigned __int128>(v);
    std::string s;
    while (u) {
        s += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (neg) s += '-';
    return {s.rbegin(), s.rend()};
}

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
    return out;
}

std::string now_utc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
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
    std::string low;
    for (char c : s) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::vector<std::string> reserved = {"true", "false", "null", "yes", "no", "on", "off", "~", "y", "n"};
    if (fullmatch(plain, s) && std::find(reserved.begin(), reserved.end(), low) == reserved.end()) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
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
    for (std::size_t i = 0; i < out.size(); ++i) text += (i ? "\n" : "") + out[i];
    return text + "\n";
}

bool write_witness(const ojson& doc, const fs::path& path) { return write_text(path, to_yaml(doc)); }

}  // namespace prism::svcomp
