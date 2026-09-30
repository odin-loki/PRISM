// TOML 1.0.0 syntax checker for the polyglot stage (json-syntax /
// toml-syntax rows run in process; no interpreter is needed). It builds no
// values, only enough of the key tree to enforce the redefinition rules:
//   - a key is defined once;
//   - a [table] header is given once, and never for a table that dotted keys
//     or an inline table already made;
//   - dotted keys extend only tables that dotted keys made in the same table;
//   - inline tables and arrays are closed: nothing may be added to them later;
//   - [[array]] appends only to an array of tables.
// Numbers are checked for form, not for 64-bit range (a syntax check, not a
// loader).

#include "prism/polyglot.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::polyglot {
namespace {

struct TomlError {
    std::size_t pos;
    std::string msg;
};

struct Node {
    enum Kind { Table, Aot, Value } kind = Table;
    bool explicit_ = false;  // given by a [header]
    bool dotted = false;     // made by dotted keys in a key/value line
    bool frozen = false;     // inline table: closed
    std::map<std::string, std::unique_ptr<Node>> kids;
    std::vector<std::unique_ptr<Node>> items;  // array-of-tables elements
};

constexpr int MAX_DEPTH = 256;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_bare(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}
bool is_control(unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7f; }

void put_utf8(std::string& out, std::uint32_t cp) {
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

// Offset of the first byte that is not well-formed UTF-8, or npos.
std::size_t bad_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int n;
        std::uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) n = 1, cp = c & 0x1F, min = 0x80;
        else if ((c & 0xF0) == 0xE0) n = 2, cp = c & 0x0F, min = 0x800;
        else if ((c & 0xF8) == 0xF0) n = 3, cp = c & 0x07, min = 0x10000;
        else return i;
        if (i + static_cast<std::size_t>(n) >= s.size()) return i;
        for (int k = 1; k <= n; ++k) {
            auto d = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((d & 0xC0) != 0x80) return i;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return i;
        i += static_cast<std::size_t>(n) + 1;
    }
    return std::string_view::npos;
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    void run() {
        if (s_.starts_with("\xEF\xBB\xBF")) i_ = 3;
        while (!eof()) {
            ws();
            if (newline()) continue;
            if (eof()) break;
            if (peek() == '#') {
                comment();
            } else if (peek() == '[') {
                header();
            } else {
                keyval(*cur_);
            }
            end_of_line();
        }
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;
    Node root_;
    Node* cur_ = &root_;
    int depth_ = 0;

    [[noreturn]] void fail(std::string m, std::size_t at) const { throw TomlError{at, std::move(m)}; }
    [[noreturn]] void fail(std::string m) const { fail(std::move(m), i_); }
    bool eof() const { return i_ >= s_.size(); }
    char peek(std::size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }

    void ws() {
        while (!eof() && (s_[i_] == ' ' || s_[i_] == '\t')) ++i_;
    }
    bool newline() {
        if (peek() == '\n') {
            ++i_;
            return true;
        }
        if (peek() == '\r' && peek(1) == '\n') {
            i_ += 2;
            return true;
        }
        return false;
    }
    void comment() {
        if (peek() != '#') return;
        ++i_;
        while (!eof() && s_[i_] != '\n') {
            if (s_[i_] == '\r' && peek(1) == '\n') break;
            if (is_control(static_cast<unsigned char>(s_[i_]))) fail("control character in comment");
            ++i_;
        }
    }
    void end_of_line() {
        ws();
        comment();
        if (eof() || newline()) return;
        fail("expected a newline after the statement");
    }

    // ---- keys ------------------------------------------------------------
    struct KeyPart {
        std::string name;
        std::size_t at;
    };

    std::vector<KeyPart> key() {
        std::vector<KeyPart> parts;
        for (;;) {
            ws();
            auto at = i_;
            parts.push_back({simple_key(), at});
            ws();
            if (peek() != '.') break;
            ++i_;
        }
        return parts;
    }

    std::string simple_key() {
        char c = peek();
        if (c == '"') {
            if (peek(1) == '"' && peek(2) == '"') fail("a multi-line string cannot be a key");
            return basic_string();
        }
        if (c == '\'') {
            if (peek(1) == '\'' && peek(2) == '\'') fail("a multi-line string cannot be a key");
            return literal_string();
        }
        auto b = i_;
        while (!eof() && is_bare(s_[i_])) ++i_;
        if (i_ == b) fail("invalid key");
        return std::string(s_.substr(b, i_ - b));
    }

    static std::string dotted_name(const std::vector<KeyPart>& parts, std::size_t upto) {
        std::string o;
        for (std::size_t k = 0; k <= upto && k < parts.size(); ++k) {
            if (k) o += '.';
            o += parts[k].name;
        }
        return o;
    }

    void keyval(Node& table) {
        auto parts = key();
        ws();
        if (peek() != '=') fail("expected '=' after a key");
        ++i_;
        ws();
        Node* t = &table;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            auto& slot = t->kids[parts[k].name];
            if (!slot) {
                slot = std::make_unique<Node>();
                slot->dotted = true;
            } else if (slot->kind != Node::Table || slot->frozen || !slot->dotted) {
                fail("cannot add to '" + dotted_name(parts, k) + "' with dotted keys", parts[k].at);
            }
            t = slot.get();
        }
        auto& last = parts.back();
        if (t->kids.contains(last.name))
            fail("key '" + dotted_name(parts, parts.size() - 1) + "' defined twice", last.at);
        auto v = value();
        t->kids[last.name] = std::move(v);
    }

    void header() {
        const auto at = i_;
        const bool aot = peek(1) == '[';
        i_ += aot ? 2 : 1;
        auto parts = key();
        ws();
        if (aot) {
            if (peek() != ']' || peek(1) != ']') fail("expected ']]' after an array-of-tables header");
            i_ += 2;
        } else {
            if (peek() != ']') fail("expected ']' after a table header");
            ++i_;
        }
        Node* t = &root_;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            auto& slot = t->kids[parts[k].name];
            if (!slot) slot = std::make_unique<Node>();
            Node* n = slot.get();
            if (n->kind == Node::Aot) {
                t = n->items.back().get();
            } else if (n->kind == Node::Table && !n->frozen) {
                t = n;
            } else {
                fail("'" + dotted_name(parts, k) + "' is not a table", parts[k].at);
            }
        }
        const auto name = dotted_name(parts, parts.size() - 1);
        auto& slot = t->kids[parts.back().name];
        if (!aot) {
            if (!slot) {
                slot = std::make_unique<Node>();
            } else if (slot->kind != Node::Table || slot->frozen || slot->explicit_ || slot->dotted) {
                fail("table '" + name + "' defined twice", at);
            }
            slot->explicit_ = true;
            cur_ = slot.get();
            return;
        }
        if (!slot) {
            slot = std::make_unique<Node>();
            slot->kind = Node::Aot;
        } else if (slot->kind != Node::Aot) {
            fail("'" + name + "' is already defined and is not an array of tables", at);
        }
        slot->items.push_back(std::make_unique<Node>());
        cur_ = slot->items.back().get();
    }

    // ---- values ----------------------------------------------------------
    std::unique_ptr<Node> value() {
        if (++depth_ > MAX_DEPTH) fail("values nested too deeply");
        auto n = std::make_unique<Node>();
        n->kind = Node::Value;
        char c = peek();
        if (c == '"') {
            if (peek(1) == '"' && peek(2) == '"') ml_basic_string();
            else basic_string();
        } else if (c == '\'') {
            if (peek(1) == '\'' && peek(2) == '\'') ml_literal_string();
            else literal_string();
        } else if (s_.substr(i_).starts_with("true")) {
            i_ += 4;
        } else if (s_.substr(i_).starts_with("false")) {
            i_ += 5;
        } else if (c == '[') {
            array();
        } else if (c == '{') {
            n = inline_table();
        } else if (is_digit(c) || c == '+' || c == '-' || c == 'i' || c == 'n') {
            number_or_date();
        } else {
            fail("invalid value");
        }
        --depth_;
        return n;
    }

    void escape(std::string& out) {
        // at the backslash
        const auto at = i_;
        ++i_;
        char c = peek();
        switch (c) {
            case 'b': out += '\b'; ++i_; return;
            case 't': out += '\t'; ++i_; return;
            case 'n': out += '\n'; ++i_; return;
            case 'f': out += '\f'; ++i_; return;
            case 'r': out += '\r'; ++i_; return;
            case '"': out += '"'; ++i_; return;
            case '\\': out += '\\'; ++i_; return;
            case 'u':
            case 'U': {
                const int n = c == 'u' ? 4 : 8;
                ++i_;
                std::uint32_t cp = 0;
                for (int k = 0; k < n; ++k) {
                    char h = peek();
                    if (!is_hex(h)) fail("invalid unicode escape", at);
                    cp = cp * 16 + static_cast<std::uint32_t>(
                                       is_digit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
                    ++i_;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    fail("unicode escape is not a scalar value", at);
                put_utf8(out, cp);
                return;
            }
            default:
                fail("invalid escape sequence", at);
        }
    }

    std::string basic_string() {
        const auto at = i_;
        ++i_;
        std::string out;
        for (;;) {
            if (eof() || peek() == '\n' || (peek() == '\r' && peek(1) == '\n'))
                fail("unterminated string", at);
            char c = s_[i_];
            if (c == '"') {
                ++i_;
                return out;
            }
            if (c == '\\') {
                escape(out);
                continue;
            }
            if (is_control(static_cast<unsigned char>(c))) fail("control character in string");
            out += c;
            ++i_;
        }
    }

    std::string literal_string() {
        const auto at = i_;
        ++i_;
        auto b = i_;
        for (;;) {
            if (eof() || peek() == '\n' || (peek() == '\r' && peek(1) == '\n'))
                fail("unterminated string", at);
            char c = s_[i_];
            if (c == '\'') {
                ++i_;
                return std::string(s_.substr(b, i_ - 1 - b));
            }
            if (is_control(static_cast<unsigned char>(c))) fail("control character in string");
            ++i_;
        }
    }

    // Closing delimiter of a multi-line string: 3 quotes, up to 2 more
    // quotes of content right before it.
    bool ml_close(char q, std::size_t at) {
        std::size_t n = 0;
        while (peek(n) == q) ++n;
        if (n < 3) {
            i_ += n;
            return false;
        }
        if (n > 5) fail("too many quotes at the end of a multi-line string", at);
        i_ += n;
        return true;
    }

    void ml_basic_string() {
        const auto at = i_;
        i_ += 3;
        newline();  // a newline right after the delimiter is trimmed
        std::string sink;
        for (;;) {
            if (eof()) fail("unterminated multi-line string", at);
            char c = s_[i_];
            if (c == '"') {
                if (ml_close('"', at)) return;
                continue;
            }
            if (c == '\\') {
                // line-ending backslash: optional blanks, then a newline
                auto j = i_ + 1;
                while (j < s_.size() && (s_[j] == ' ' || s_[j] == '\t')) ++j;
                if (j < s_.size() && (s_[j] == '\n' || (s_[j] == '\r' && j + 1 < s_.size() && s_[j + 1] == '\n'))) {
                    i_ = j;
                    for (;;) {
                        if (peek() == ' ' || peek() == '\t') ++i_;
                        else if (!newline()) break;
                    }
                    continue;
                }
                escape(sink);
                continue;
            }
            if (newline()) continue;
            if (is_control(static_cast<unsigned char>(c))) fail("control character in string");
            ++i_;
        }
    }

    void ml_literal_string() {
        const auto at = i_;
        i_ += 3;
        newline();
        for (;;) {
            if (eof()) fail("unterminated multi-line string", at);
            char c = s_[i_];
            if (c == '\'') {
                if (ml_close('\'', at)) return;
                continue;
            }
            if (newline()) continue;
            if (is_control(static_cast<unsigned char>(c))) fail("control character in string");
            ++i_;
        }
    }

    void array_ws() {
        for (;;) {
            ws();
            if (newline()) continue;
            if (peek() == '#') {
                comment();
                continue;
            }
            return;
        }
    }

    void array() {
        const auto at = i_;
        ++i_;
        for (;;) {
            array_ws();
            if (eof()) fail("unterminated array", at);
            if (peek() == ']') {
                ++i_;
                return;
            }
            value();
            array_ws();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == ']') {
                ++i_;
                return;
            }
            fail("expected ',' or ']' in an array");
        }
    }

    std::unique_ptr<Node> inline_table() {
        ++i_;
        auto n = std::make_unique<Node>();
        ws();
        if (peek() == '}') {
            ++i_;
            n->frozen = true;
            return n;
        }
        for (;;) {
            keyval(*n);
            ws();
            if (peek() == ',') {
                ++i_;
                ws();
                if (peek() == '}') fail("trailing comma in an inline table");
                continue;
            }
            if (peek() == '}') {
                ++i_;
                break;
            }
            fail("expected ',' or '}' in an inline table (inline tables are one line)");
        }
        n->frozen = true;
        return n;
    }

    // ---- numbers and dates -----------------------------------------------
    int two(std::size_t at) const {
        if (!is_digit(peek(at)) || !is_digit(peek(at + 1))) return -1;
        return (peek(at) - '0') * 10 + (peek(at + 1) - '0');
    }

    void time_part(std::size_t at) {
        int h = two(0), m = -1, sec = -1;
        if (h >= 0 && peek(2) == ':') m = two(3);
        if (m >= 0 && peek(5) == ':') sec = two(6);
        if (h < 0 || m < 0 || sec < 0) fail("invalid time (HH:MM:SS)", at);
        if (h > 23 || m > 59 || sec > 60) fail("time out of range", at);
        i_ += 8;
        if (peek() == '.') {
            ++i_;
            if (!is_digit(peek())) fail("invalid fractional seconds", at);
            while (is_digit(peek())) ++i_;
        }
    }

    void date_time() {
        const auto at = i_;
        int y0 = two(0), y1 = two(2);
        int mo = peek(4) == '-' ? two(5) : -1;
        int d = mo >= 0 && peek(7) == '-' ? two(8) : -1;
        if (y0 < 0 || y1 < 0 || mo < 0 || d < 0) fail("invalid date (YYYY-MM-DD)", at);
        int y = y0 * 100 + y1;
        static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        if (mo < 1 || mo > 12) fail("month out of range", at);
        int maxd = kDays[mo - 1] + (mo == 2 && leap ? 1 : 0);
        if (d < 1 || d > maxd) fail("day out of range", at);
        i_ += 10;
        bool timed = false;
        if ((peek() == 'T' || peek() == 't') && is_digit(peek(1))) {
            ++i_;
            timed = true;
        } else if (peek() == ' ' && is_digit(peek(1)) && is_digit(peek(2)) && peek(3) == ':') {
            ++i_;
            timed = true;
        } else if (peek() == 'T' || peek() == 't') {
            fail("invalid date-time", at);
        }
        if (!timed) return;
        time_part(at);
        if (peek() == 'Z' || peek() == 'z') {
            ++i_;
        } else if (peek() == '+' || peek() == '-') {
            int oh = two(1), om = peek(3) == ':' ? two(4) : -1;
            if (oh < 0 || om < 0 || oh > 23 || om > 59) fail("invalid time offset", at);
            i_ += 6;
        }
    }

    // digit ( '_'? digit )* for the given digit class
    template <class P>
    bool digits(std::string_view t, std::size_t& k, P ok) const {
        if (k >= t.size() || !ok(t[k])) return false;
        ++k;
        while (k < t.size()) {
            if (ok(t[k])) {
                ++k;
            } else if (t[k] == '_' && k + 1 < t.size() && ok(t[k + 1])) {
                k += 2;
            } else {
                break;
            }
        }
        return true;
    }

    void number_or_date() {
        const auto at = i_;
        if (is_digit(peek()) && is_digit(peek(1)) && is_digit(peek(2)) && is_digit(peek(3)) &&
            peek(4) == '-') {
            date_time();
            return;
        }
        if (is_digit(peek()) && is_digit(peek(1)) && peek(2) == ':') {
            time_part(at);
            return;
        }
        auto b = i_;
        while (!eof() && (is_bare(s_[i_]) || s_[i_] == '+' || s_[i_] == '.')) ++i_;
        auto t = s_.substr(b, i_ - b);
        if (!valid_number(t)) fail("invalid number '" + std::string(t) + "'", at);
    }

    bool valid_number(std::string_view t) const {
        for (auto w : {"inf", "+inf", "-inf", "nan", "+nan", "-nan"})
            if (t == w) return true;
        auto dec = [](char c) { return is_digit(c); };
        if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b')) {
            std::size_t k = 2;
            bool ok = t[1] == 'x' ? digits(t, k, [](char c) { return is_hex(c); })
                      : t[1] == 'o' ? digits(t, k, [](char c) { return c >= '0' && c <= '7'; })
                                    : digits(t, k, [](char c) { return c == '0' || c == '1'; });
            return ok && k == t.size();
        }
        std::size_t k = 0;
        if (k < t.size() && (t[k] == '+' || t[k] == '-')) ++k;
        if (k >= t.size()) return false;
        if (t[k] == '0') {
            ++k;
            if (k < t.size() && (is_digit(t[k]) || t[k] == '_')) return false;  // leading zero
        } else if (!digits(t, k, dec)) {
            return false;
        }
        if (k < t.size() && t[k] == '.') {
            ++k;
            if (!digits(t, k, dec)) return false;
        }
        if (k < t.size() && (t[k] == 'e' || t[k] == 'E')) {
            ++k;
            if (k < t.size() && (t[k] == '+' || t[k] == '-')) ++k;
            if (!digits(t, k, dec)) return false;
        }
        return k == t.size();
    }
};

SyntaxIssue at_offset(std::string_view s, std::size_t pos, std::string msg) {
    SyntaxIssue out;
    out.line = 1;
    std::size_t bol = 0;
    for (std::size_t k = 0; k < pos && k < s.size(); ++k)
        if (s[k] == '\n') {
            ++out.line;
            bol = k + 1;
        }
    int col = 1;
    for (std::size_t k = bol; k < pos && k < s.size(); ++k)
        if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80) ++col;
    out.col = col;
    out.msg = std::move(msg);
    return out;
}

}  // namespace

std::optional<SyntaxIssue> toml_syntax(std::string_view text) {
    if (auto bad = bad_utf8(text); bad != std::string_view::npos)
        return at_offset(text, bad, "TOML: invalid UTF-8");
    try {
        Parser(text).run();
    } catch (const TomlError& e) {
        return at_offset(text, e.pos, "TOML: " + e.msg);
    }
    return std::nullopt;
}

}  // namespace prism::polyglot
