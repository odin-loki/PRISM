#include "yaml_subset.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace prism::qa::yaml {

namespace {

struct Line {
    int no = 0;       // 1-based line number in the file
    int indent = 0;   // leading spaces
    std::string body; // content after the indent, comment and trailing blanks removed
};

[[noreturn]] void fail(const std::string& where, int line, const std::string& why) {
    throw Error(where + ":" + std::to_string(line) + ": " + why);
}

std::string rstrip(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string strip(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

// Remove a `#` comment (at the start or after a blank, outside quotes).
std::string strip_comment(const std::string& s, const std::string& where, int no) {
    char q = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q == '\'') {
            if (c == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') ++i;
                else q = 0;
            }
            continue;
        }
        if (q == '"') {
            if (c == '\\') ++i;
            else if (c == '"') q = 0;
            continue;
        }
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) return rstrip(s.substr(0, i));
        // a quote opens a quoted scalar only where a scalar starts
        if ((c == '\'' || c == '"')) {
            std::size_t j = i;
            while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
            if (j == 0 || s[j - 1] == ':' || s[j - 1] == '-' || s[j - 1] == '[' || s[j - 1] == ',' ||
                s[j - 1] == '{')
                q = c;
        }
    }
    if (q) fail(where, no, "unterminated quoted scalar (multi-line scalars are not supported)");
    return rstrip(s);
}

// Position of the `:` that ends a mapping key (followed by a blank or the
// end), outside quotes and brackets; npos when the text is not `key: ...`.
std::size_t key_colon(const std::string& s) {
    char q = 0;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q == '\'') {
            if (c == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') ++i;
                else q = 0;
            }
            continue;
        }
        if (q == '"') {
            if (c == '\\') ++i;
            else if (c == '"') q = 0;
            continue;
        }
        if ((c == '\'' || c == '"') && (i == 0)) {
            q = c;
            continue;
        }
        if (c == '[' || c == '{') ++depth;
        if (c == ']' || c == '}') --depth;
        if (c == ':' && depth == 0 && (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) return i;
    }
    return std::string::npos;
}

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
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

Node scalar(std::string text, bool quoted) {
    Node n;
    n.kind = Node::Kind::Scalar;
    n.text = std::move(text);
    n.quoted = quoted;
    return n;
}

// One scalar token (the whole of `s`): quoted or plain.
Node parse_scalar(const std::string& raw, const std::string& where, int no) {
    std::string s = strip(raw);
    if (s.empty()) return Node{};
    if (s[0] == '\'') {
        std::string out;
        std::size_t i = 1;
        for (; i < s.size(); ++i) {
            if (s[i] == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') {
                    out += '\'';
                    ++i;
                    continue;
                }
                break;
            }
            out += s[i];
        }
        if (i >= s.size() || i + 1 != s.size()) fail(where, no, "bad single-quoted scalar: " + s);
        return scalar(out, true);
    }
    if (s[0] == '"') {
        std::string out;
        std::size_t i = 1;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (++i >= s.size()) break;
            char e = s[i];
            switch (e) {
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                case ' ': out += ' '; break;
                case 'x':
                case 'u':
                case 'U': {
                    std::size_t len = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                    if (i + len >= s.size()) fail(where, no, "bad escape in " + s);
                    unsigned cp = static_cast<unsigned>(std::stoul(s.substr(i + 1, len), nullptr, 16));
                    append_utf8(out, cp);
                    i += len;
                    break;
                }
                default: fail(where, no, std::string("unsupported escape \\") + e);
            }
        }
        if (i >= s.size() || i + 1 != s.size()) fail(where, no, "bad double-quoted scalar: " + s);
        return scalar(out, true);
    }
    char c0 = s[0];
    if (c0 == '|' || c0 == '>') fail(where, no, "block scalars are not supported");
    if (c0 == '&' || c0 == '*' || c0 == '!') fail(where, no, "anchors, aliases and tags are not supported");
    if (c0 == '{') {
        if (strip(s.substr(1)) == "}") {
            Node n;
            n.kind = Node::Kind::Map;
            return n;
        }
        fail(where, no, "flow mappings are not supported");
    }
    if (key_colon(s) != std::string::npos) fail(where, no, "mapping values are not allowed here: " + s);
    if (s == "~" || s == "null" || s == "Null" || s == "NULL") return Node{};
    return scalar(s, false);
}

Node parse_flow_seq(const std::string& s, const std::string& where, int no) {
    // s starts with '[' ; elements are scalars (no nesting)
    std::string t = strip(s);
    if (t.size() < 2 || t.back() != ']') fail(where, no, "flow sequence must end the line: " + t);
    std::string inner = t.substr(1, t.size() - 2);
    Node n;
    n.kind = Node::Kind::Seq;
    if (strip(inner).empty()) return n;
    std::string cur;
    char q = 0;
    auto flush = [&] {
        std::string e = strip(cur);
        if (e.empty()) fail(where, no, "empty flow sequence element");
        if (e[0] == '[' || e[0] == '{') fail(where, no, "nested flow collections are not supported");
        n.seq.push_back(parse_scalar(e, where, no));
        cur.clear();
    };
    for (std::size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (q) {
            cur += c;
            if (q == '"' && c == '\\' && i + 1 < inner.size()) cur += inner[++i];
            else if (c == q) q = 0;
            continue;
        }
        if (c == '\'' || c == '"') q = c;
        if (c == ',') {
            flush();
            continue;
        }
        cur += c;
    }
    if (q) fail(where, no, "unterminated quote in flow sequence");
    flush();
    return n;
}

Node parse_value(const std::string& s, const std::string& where, int no) {
    std::string t = strip(s);
    if (!t.empty() && t[0] == '[') return parse_flow_seq(t, where, no);
    return parse_scalar(t, where, no);
}

bool is_seq_item(const Line& l) { return l.body == "-" || l.body.rfind("- ", 0) == 0; }

struct Parser {
    std::vector<Line> lines;
    std::string where;
    std::size_t pos = 0;

    Node block(int indent) {
        if (pos >= lines.size()) return Node{};
        if (is_seq_item(lines[pos])) return sequence(indent);
        return mapping(indent);
    }

    // value of `key:` with nothing after the colon: a nested block (more
    // indented, or a sequence at the key's own indent) or null
    Node nested(int key_indent) {
        if (pos >= lines.size()) return Node{};
        const Line& n = lines[pos];
        if (n.indent > key_indent) return block(n.indent);
        if (n.indent == key_indent && is_seq_item(n)) return sequence(key_indent);
        return Node{};
    }

    Node mapping(int indent) {
        Node m;
        m.kind = Node::Kind::Map;
        while (pos < lines.size()) {
            const Line l = lines[pos];
            if (l.indent < indent) break;
            if (l.indent > indent) fail(where, l.no, "unexpected indentation");
            if (is_seq_item(l)) break;
            std::size_t c = key_colon(l.body);
            if (c == std::string::npos) fail(where, l.no, "expected `key: value`: " + l.body);
            Node k = parse_scalar(l.body.substr(0, c), where, l.no);
            if (!k.is_scalar()) fail(where, l.no, "unsupported mapping key");
            std::string rest = strip(l.body.substr(c + 1));
            ++pos;
            Node v = rest.empty() ? nested(indent) : parse_value(rest, where, l.no);
            std::string key = k.quoted ? k.text : k.py_str();
            bool replaced = false;
            for (auto& [kk, vv] : m.map)
                if (kk == key) {
                    vv = std::move(v);
                    replaced = true;
                    break;
                }
            if (!replaced) m.map.push_back(Entry{key, std::move(v)});
        }
        return m;
    }

    Node sequence(int indent) {
        Node s;
        s.kind = Node::Kind::Seq;
        while (pos < lines.size()) {
            Line& l = lines[pos];
            if (l.indent < indent) break;
            if (l.indent > indent) fail(where, l.no, "unexpected indentation");
            if (!is_seq_item(l)) break;
            std::string rest = l.body.size() > 1 ? l.body.substr(2) : std::string();
            std::size_t lead = 0;
            while (lead < rest.size() && rest[lead] == ' ') ++lead;
            rest = rest.substr(lead);
            if (rest.empty()) {
                ++pos;
                s.seq.push_back(nested(indent));
                continue;
            }
            if (rest[0] != '[' && rest[0] != '\'' && rest[0] != '"' && key_colon(rest) != std::string::npos) {
                // `- key: value` opens a mapping whose keys sit at the column of `key`
                int col = indent + 2 + static_cast<int>(lead);
                l.indent = col;
                l.body = rest;
                s.seq.push_back(mapping(col));
                continue;
            }
            if (rest[0] == '-' && (rest.size() == 1 || rest[1] == ' '))
                fail(where, l.no, "nested inline sequences are not supported");
            ++pos;
            s.seq.push_back(parse_value(rest, where, l.no));
        }
        return s;
    }
};

}  // namespace

bool Node::is_null() const { return kind == Kind::Null; }

const Node* Node::get(std::string_view key) const {
    if (kind != Kind::Map) return nullptr;
    for (const auto& [k, v] : map)
        if (k == key) return &v;
    return nullptr;
}

std::optional<bool> Node::as_bool() const {
    if (kind != Kind::Scalar || quoted) return std::nullopt;
    static const char* const yes[] = {"yes", "Yes", "YES", "true", "True", "TRUE", "on", "On", "ON"};
    static const char* const no[] = {"no", "No", "NO", "false", "False", "FALSE", "off", "Off", "OFF"};
    for (const char* y : yes)
        if (text == y) return true;
    for (const char* n : no)
        if (text == n) return false;
    return std::nullopt;
}

std::optional<__int128> Node::as_int() const {
    if (kind != Kind::Scalar || quoted || text.empty()) return std::nullopt;
    std::string t;
    for (char c : text)
        if (c != '_') t += c;
    bool neg = false;
    std::size_t i = 0;
    if (t[0] == '-' || t[0] == '+') {
        neg = t[0] == '-';
        i = 1;
    }
    if (i >= t.size()) return std::nullopt;
    int base = 10;
    if (t.size() > i + 1 && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
        base = 16;
        i += 2;
    } else if (t.size() > i + 1 && t[i] == '0' && t[i + 1] == 'b') {
        base = 2;
        i += 2;
    } else if (t.size() > i + 1 && t[i] == '0' && t[i + 1] == 'o') {
        base = 8;
        i += 2;
    } else if (t.size() > i + 1 && t[i] == '0') {
        base = 8;  // YAML 1.1: a leading 0 is octal
        i += 1;
    }
    if (i >= t.size()) return std::nullopt;
    unsigned __int128 v = 0;
    for (; i < t.size(); ++i) {
        char c = t[i];
        int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                         : 99;
        if (d >= base) return std::nullopt;
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        if (v > (static_cast<unsigned __int128>(1) << 126)) return std::nullopt;
    }
    __int128 r = static_cast<__int128>(v);
    return neg ? -r : r;
}

bool Node::is_float() const {
    if (kind != Kind::Scalar || quoted || as_int()) return false;
    std::string t;
    for (char c : text)
        if (c != '_') t += c;
    if (t == ".inf" || t == "-.inf" || t == "+.inf" || t == ".nan" || t == ".NaN" || t == ".Inf") return true;
    // [-+]?([0-9][0-9_]*)?\.[0-9.]*([eE][-+][0-9]+)?
    std::size_t i = 0;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
    if (i >= t.size() || t[i] != '.') return false;
    ++i;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) ++i;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        ++i;
        if (i >= t.size() || (t[i] != '-' && t[i] != '+')) return false;
        ++i;
        if (i >= t.size()) return false;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
    }
    return i == t.size();
}

namespace {
std::string i128_str(__int128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? static_cast<unsigned __int128>(-(v + 1)) + 1 : static_cast<unsigned __int128>(v);
    std::string s;
    while (u) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(u % 10)));
        u /= 10;
    }
    return neg ? "-" + s : s;
}
}  // namespace

std::string Node::py_str() const {
    switch (kind) {
        case Kind::Null: return "None";
        case Kind::Map: return "{...}";
        case Kind::Seq: return "[...]";
        case Kind::Scalar: break;
    }
    if (auto b = as_bool()) return *b ? "True" : "False";
    if (auto i = as_int()) return i128_str(*i);
    return text;
}

bool Node::truthy_label() const {
    if (auto b = as_bool()) return *b;
    std::string s = strip(py_str());
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "true";
}

Node parse(std::string_view text, const std::string& where) {
    Parser p;
    p.where = where;
    std::string all(text);
    std::istringstream in(all);
    std::string raw;
    int no = 0;
    while (std::getline(in, raw)) {
        ++no;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::size_t ind = 0;
        while (ind < raw.size() && raw[ind] == ' ') ++ind;
        if (ind < raw.size() && raw[ind] == '\t') fail(where, no, "tab indentation");
        std::string body = strip_comment(raw.substr(ind), where, no);
        if (body.empty()) continue;
        if (ind == 0 && (body == "---" || body.rfind("--- ", 0) == 0)) {
            if (!p.lines.empty()) fail(where, no, "multi-document streams are not supported");
            continue;
        }
        if (ind == 0 && body == "...") continue;
        if (body[0] == '%') fail(where, no, "YAML directives are not supported");
        p.lines.push_back(Line{no, static_cast<int>(ind), body});
    }
    if (p.lines.empty()) return Node{};
    const Line& first = p.lines.front();
    Node root;
    if (!is_seq_item(first) && key_colon(first.body) == std::string::npos) {
        if (p.lines.size() != 1) fail(where, p.lines[1].no, "a scalar document has one line");
        root = parse_value(first.body, where, first.no);
        p.pos = 1;
    } else {
        root = p.block(first.indent);
    }
    if (p.pos != p.lines.size()) fail(where, p.lines[p.pos].no, "unexpected indentation");
    return root;
}

Node load_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error(path.string() + ": cannot read");
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), path.string());
}

}  // namespace prism::qa::yaml
