// YAML subset reader for task definitions (include/prism/taskdef.hpp).
#include "prism/taskdef.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace prism::taskdef {

using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

struct Line {
    int indent = 0;
    std::string text;  // without indentation and comment
    int number = 0;    // 1-based source line
};

struct YamlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const Line& l, const std::string& why) {
    throw YamlError("line " + std::to_string(l.number) + ": " + why);
}

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return rtrim(s.substr(a));
}

// Cut a comment: `#` at the start or after a blank, outside quotes.
std::string cut_comment(const std::string& s) {
    char q = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (q == '\'' && c == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') ++i;
                else q = 0;
            } else if (q == '"' && c == '\\') {
                ++i;
            } else if (q == '"' && c == '"') {
                q = 0;
            }
            continue;
        }
        if ((c == '\'' || c == '"') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '[' ||
                                        s[i - 1] == '{' || s[i - 1] == ',' || s[i - 1] == ':' || s[i - 1] == '-'))
            q = c;
        else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
    }
    return s;
}

// YAML 1.1 plain scalar typing (PyYAML's resolver, the common cases).
ojson plain_scalar(const std::string& s) {
    if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") return nullptr;
    static const char* trues[] = {"true", "True", "TRUE", "yes", "Yes", "YES", "on", "On", "ON"};
    static const char* falses[] = {"false", "False", "FALSE", "no", "No", "NO", "off", "Off", "OFF"};
    for (auto* t : trues)
        if (s == t) return true;
    for (auto* f : falses)
        if (s == f) return false;
    std::string d;
    for (char c : s)
        if (c != '_') d += c;
    std::size_t i = (d[0] == '+' || d[0] == '-') ? 1 : 0;
    auto all = [&](std::size_t from, auto pred) {
        if (from >= d.size()) return false;
        for (std::size_t k = from; k < d.size(); ++k)
            if (!pred(static_cast<unsigned char>(d[k]))) return false;
        return true;
    };
    if (d.size() > i + 2 && d[i] == '0' && (d[i + 1] == 'x' || d[i + 1] == 'X') && all(i + 2, ::isxdigit)) {
        try {
            return std::stoll(d, nullptr, 16);  // sign and 0x prefix included
        } catch (const std::out_of_range&) {
            return s;
        }
    }
    if (all(i, ::isdigit)) {
        try {
            if (d.size() > i + 1 && d[i] == '0') {
                if (all(i, [](unsigned char c) { return c >= '0' && c <= '7'; })) {
                    return std::stoll(d, nullptr, 8);
                }
                return s;  // "09": a string for YAML 1.1
            }
            return std::stoll(d);  // with its sign: -9223372036854775808 fits
        } catch (const std::out_of_range&) {
            return s;
        }
    }
    // float: a dot is required, an exponent needs its sign (YAML 1.1)
    {
        std::size_t k = i;
        bool digits = false;
        while (k < d.size() && std::isdigit(static_cast<unsigned char>(d[k]))) {
            ++k;
            digits = true;
        }
        if (k < d.size() && d[k] == '.') {
            ++k;
            while (k < d.size() && std::isdigit(static_cast<unsigned char>(d[k]))) {
                ++k;
                digits = true;
            }
            if (k + 1 < d.size() && (d[k] == 'e' || d[k] == 'E') && (d[k + 1] == '+' || d[k + 1] == '-')) {
                std::size_t e = k + 2;
                while (e < d.size() && std::isdigit(static_cast<unsigned char>(d[e]))) ++e;
                if (e == d.size() && e > k + 2) k = e;
            }
            if (k == d.size() && digits) return std::strtod(d.c_str(), nullptr);
        }
    }
    std::string low = s;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == ".inf" || low == "+.inf") return std::numeric_limits<double>::infinity();
    if (low == "-.inf") return -std::numeric_limits<double>::infinity();
    if (low == ".nan") return std::numeric_limits<double>::quiet_NaN();
    return s;
}

void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Flow/inline value parser over one line's text.
struct Flow {
    const std::string& s;
    const Line& line;
    std::size_t p = 0;

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    }
    std::string quoted() {
        char q = s[p++];
        std::string out;
        while (p < s.size()) {
            char c = s[p++];
            if (q == '\'') {
                if (c == '\'') {
                    if (p < s.size() && s[p] == '\'') {
                        out += '\'';
                        ++p;
                        continue;
                    }
                    return out;
                }
                out += c;
            } else {
                if (c == '"') return out;
                if (c != '\\') {
                    out += c;
                    continue;
                }
                if (p >= s.size()) break;
                char e = s[p++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '/': out += '/'; break;
                    case ' ': out += ' '; break;
                    case 'x':
                    case 'u':
                    case 'U': {
                        std::size_t n = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                        if (p + n > s.size()) fail(line, "bad escape");
                        append_utf8(out, std::stoul(s.substr(p, n), nullptr, 16));
                        p += n;
                        break;
                    }
                    default: fail(line, std::string("unsupported escape \\") + e);
                }
            }
        }
        fail(line, "unterminated quoted scalar");
    }
    // A plain scalar up to one of the stop characters (in flow context
    // `,]}` end it too; `: ` ends a key).
    std::string plain(bool in_flow, bool key) {
        std::size_t start = p;
        while (p < s.size()) {
            char c = s[p];
            if (in_flow && (c == ',' || c == ']' || c == '}')) break;
            if (key && c == ':' && (p + 1 == s.size() || s[p + 1] == ' ' || (in_flow && (s[p + 1] == ',' || s[p + 1] == '}'))))
                break;
            if (in_flow && c == ':' && (p + 1 == s.size() || s[p + 1] == ' ')) break;
            ++p;
        }
        return rtrim(s.substr(start, p - start));
    }
    ojson value(bool in_flow) {
        ws();
        if (p >= s.size()) return nullptr;
        char c = s[p];
        if (c == '[') {
            ++p;
            ojson arr = ojson::array();
            ws();
            if (p < s.size() && s[p] == ']') {
                ++p;
                return arr;
            }
            for (;;) {
                arr.push_back(value(true));
                ws();
                if (p < s.size() && s[p] == ',') {
                    ++p;
                    ws();
                    if (p < s.size() && s[p] == ']') {
                        ++p;
                        return arr;
                    }
                    continue;
                }
                if (p < s.size() && s[p] == ']') {
                    ++p;
                    return arr;
                }
                fail(line, "unterminated flow sequence");
            }
        }
        if (c == '{') {
            ++p;
            ojson obj = ojson::object();
            ws();
            if (p < s.size() && s[p] == '}') {
                ++p;
                return obj;
            }
            for (;;) {
                ws();
                std::string k = (p < s.size() && (s[p] == '\'' || s[p] == '"')) ? quoted() : plain(true, true);
                ws();
                ojson v = nullptr;
                if (p < s.size() && s[p] == ':') {
                    ++p;
                    v = value(true);
                }
                obj[k] = v;
                ws();
                if (p < s.size() && s[p] == ',') {
                    ++p;
                    continue;
                }
                if (p < s.size() && s[p] == '}') {
                    ++p;
                    return obj;
                }
                fail(line, "unterminated flow mapping");
            }
        }
        if (c == '\'' || c == '"') return quoted();
        if (c == '&' || c == '*' || c == '!' || c == '|' || c == '>' || c == '@' || c == '`' || c == '%')
            fail(line, std::string("unsupported YAML syntax '") + c + "'");
        return plain_scalar(plain(in_flow, false));
    }
};

// `key: rest` of a mapping line; nullopt when the line is not one.
std::optional<std::pair<std::string, std::string>> split_key(const Line& l) {
    const std::string& s = l.text;
    if (s.empty() || s[0] == '[' || s[0] == '{' || s == "-" || s.starts_with("- ")) return std::nullopt;
    std::string key;
    std::size_t p = 0;
    if (s[0] == '\'' || s[0] == '"') {
        Flow f{s, l};
        key = f.quoted();
        p = f.p;
        while (p < s.size() && s[p] == ' ') ++p;
        if (p >= s.size() || s[p] != ':') return std::nullopt;
    } else {
        for (p = 0; p < s.size(); ++p)
            if (s[p] == ':' && (p + 1 == s.size() || s[p + 1] == ' ' || s[p + 1] == '\t')) break;
        if (p >= s.size()) return std::nullopt;
        key = rtrim(s.substr(0, p));
    }
    return std::make_pair(key, trim(s.substr(p + 1)));
}

struct Parser {
    std::vector<Line> lines;
    std::size_t i = 0;

    ojson inline_value(const Line& l, const std::string& text) {
        Flow f{text, l};
        ojson v = f.value(false);
        f.ws();
        if (f.p != text.size()) fail(l, "unexpected text after a value");
        return v;
    }

    // `|` / `>` block scalar: the lines indented deeper than parent_indent.
    ojson block_scalar(const std::string& head, int parent_indent) {
        bool folded = head[0] == '>';
        std::string out;
        int ind = -1;
        std::vector<std::string> parts;
        while (i < lines.size() && lines[i].indent > parent_indent) {
            if (ind < 0) ind = lines[i].indent;
            parts.push_back(std::string(static_cast<std::size_t>(lines[i].indent - ind), ' ') + lines[i].text);
            ++i;
        }
        for (std::size_t k = 0; k < parts.size(); ++k) out += (k ? (folded ? " " : "\n") : "") + parts[k];
        if (!parts.empty() && head.find('-') == std::string::npos) out += "\n";
        return out;
    }

    ojson value_after_key(const Line& l, const std::string& rest, int indent) {
        if (rest.starts_with("|") || rest.starts_with(">")) return block_scalar(rest, indent);
        if (!rest.empty()) return inline_value(l, rest);
        if (i < lines.size() && lines[i].indent > indent) return block(lines[i].indent);
        // a sequence may sit at its key's own indentation
        if (i < lines.size() && lines[i].indent == indent && (lines[i].text == "-" || lines[i].text.starts_with("- ")))
            return sequence(indent);
        return nullptr;
    }

    ojson mapping(int indent) {
        ojson obj = ojson::object();
        while (i < lines.size() && lines[i].indent == indent) {
            const Line l = lines[i];
            auto kv = split_key(l);
            if (!kv) break;
            ++i;
            if (obj.contains(kv->first)) fail(l, "duplicate key " + kv->first);
            obj[kv->first] = value_after_key(l, kv->second, indent);
        }
        return obj;
    }

    ojson sequence(int indent) {
        ojson arr = ojson::array();
        while (i < lines.size() && lines[i].indent == indent &&
               (lines[i].text == "-" || lines[i].text.starts_with("- "))) {
            Line l = lines[i];
            std::string rest = l.text == "-" ? "" : l.text.substr(2);
            std::size_t extra = 0;
            while (extra < rest.size() && rest[extra] == ' ') ++extra;
            rest = rest.substr(extra);
            if (rest.empty()) {
                ++i;
                if (i < lines.size() && lines[i].indent > indent) arr.push_back(block(lines[i].indent));
                else arr.push_back(nullptr);
                continue;
            }
            Line inner{indent + 2 + static_cast<int>(extra), rest, l.number};
            if (split_key(inner) || rest == "-" || rest.starts_with("- ")) {
                lines[i] = inner;  // the item is a block collection starting on the dash's line
                arr.push_back(block(inner.indent));
                continue;
            }
            ++i;
            arr.push_back(inline_value(l, rest));
        }
        return arr;
    }

    ojson block(int indent) {
        const Line& l = lines[i];
        if (l.text == "-" || l.text.starts_with("- ")) return sequence(indent);
        if (split_key(l)) return mapping(indent);
        ++i;
        return inline_value(l, l.text);
    }
};

}  // namespace

YamlResult parse_yaml(const std::string& text) {
    Parser ps;
    std::istringstream in(text);
    std::string raw;
    int n = 0;
    bool seen_content = false;
    while (std::getline(in, raw)) {
        ++n;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::size_t ind = 0;
        while (ind < raw.size() && raw[ind] == ' ') ++ind;
        if (ind < raw.size() && raw[ind] == '\t') return {std::nullopt, "line " + std::to_string(n) + ": tab indentation"};
        std::string body = rtrim(cut_comment(raw.substr(ind)));
        if (body.empty()) continue;
        if (ind == 0 && (body == "---" || body.starts_with("--- "))) {
            if (seen_content) return {std::nullopt, "line " + std::to_string(n) + ": several documents"};
            continue;
        }
        if (ind == 0 && body == "...") break;
        if (ind == 0 && body.starts_with("%")) return {std::nullopt, "line " + std::to_string(n) + ": directives"};
        seen_content = true;
        ps.lines.push_back(Line{static_cast<int>(ind), body, n});
    }
    if (ps.lines.empty()) return {ojson(nullptr), ""};
    try {
        ojson v = ps.block(ps.lines[0].indent);
        if (ps.i != ps.lines.size()) fail(ps.lines[ps.i], "unexpected indentation or content");
        return {v, ""};
    } catch (const YamlError& e) {
        return {std::nullopt, e.what()};
    } catch (const std::exception& e) {
        return {std::nullopt, std::string("unreadable: ") + e.what()};
    }
}

YamlResult load_yaml(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {std::nullopt, "cannot read " + path.string()};
    std::ostringstream ss;
    ss << in.rdbuf();
    auto r = parse_yaml(ss.str());
    if (!r.value) r.error = path.string() + ": " + r.error;
    return r;
}

namespace {

std::string as_text(const ojson& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    return v.dump();
}

}  // namespace

std::optional<TaskDef> parse_taskdef(const ojson& doc, std::string* error) {
    auto bad = [&](const std::string& why) -> std::optional<TaskDef> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!doc.is_object()) return bad("not a mapping");
    if (!doc.contains("input_files")) return bad("no input_files");
    TaskDef t;
    if (doc.contains("format_version")) t.format_version = as_text(doc.at("format_version"));
    const auto& inp = doc.at("input_files");
    if (inp.is_array()) {
        for (const auto& x : inp) t.input_files.push_back(as_text(x));
    } else {
        t.input_files.push_back(as_text(inp));
    }
    if (doc.contains("properties") && doc.at("properties").is_array()) {
        for (const auto& p : doc.at("properties")) {
            if (!p.is_object()) continue;
            Property pr;
            if (p.contains("property_file")) pr.property_file = as_text(p.at("property_file"));
            if (p.contains("expected_verdict")) {
                const auto& ev = p.at("expected_verdict");
                if (ev.is_boolean()) pr.expected_verdict = ev.get<bool>();
                else pr.expected_verdict = !as_text(ev).empty();  // bool() of a non-empty value
            }
            if (p.contains("subproperty")) pr.subproperty = as_text(p.at("subproperty"));
            t.properties.push_back(std::move(pr));
        }
    }
    if (doc.contains("options") && doc.at("options").is_object()) {
        const auto& o = doc.at("options");
        if (o.contains("language")) t.language = as_text(o.at("language"));
        if (o.contains("data_model")) t.data_model = as_text(o.at("data_model"));
    }
    return t;
}

}  // namespace prism::taskdef
