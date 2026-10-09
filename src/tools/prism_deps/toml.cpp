// Fail-closed reader for the TOML subset MANIFEST.toml uses (see deps.hpp).
#include "prism/deps.hpp"

#include <cctype>

namespace prism::deps {

const Value* Table::find(std::string_view key) const {
    for (const auto& [k, v] : items)
        if (k == key) return &v;
    return nullptr;
}

bool Table::truthy(std::string_view key) const {
    const Value* v = find(key);
    if (!v) return false;
    if (auto s = std::get_if<std::string>(v)) return !s->empty();
    if (auto l = std::get_if<std::vector<std::string>>(v)) return !l->empty();
    if (auto b = std::get_if<bool>(v)) return *b;
    return std::get<std::int64_t>(*v) != 0;
}

std::string Table::str(std::string_view key) const {
    const Value* v = find(key);
    if (!v) return {};
    if (auto s = std::get_if<std::string>(v)) return *s;
    if (auto i = std::get_if<std::int64_t>(v)) return std::to_string(*i);
    if (auto b = std::get_if<bool>(v)) return *b ? "True" : "False";
    return {};
}

std::vector<std::string> Table::list(std::string_view key) const {
    const Value* v = find(key);
    if (!v) return {};
    if (auto l = std::get_if<std::vector<std::string>>(v)) return *l;
    return {};
}

Table& Table::set(std::string key, Value v) {
    for (auto& [k, old] : items)
        if (k == key) {
            old = std::move(v);
            return *this;
        }
    items.emplace_back(std::move(key), std::move(v));
    return *this;
}

Table& Table::erase(std::string_view key) {
    std::erase_if(items, [&](const auto& kv) { return kv.first == key; });
    return *this;
}

const std::vector<Table>& TomlDoc::array(std::string_view name) const {
    static const std::vector<Table> kEmpty;
    for (const auto& [k, v] : arrays)
        if (k == name) return v;
    return kEmpty;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : s_(t) {}

    TomlDoc run() {
        TomlDoc doc;
        Table* cur = &doc.root;
        while (true) {
            skip_ws_and_newlines();
            if (eof()) break;
            if (peek() == '[') {
                if (!starts("[[")) fail("only [[array]] tables are supported");
                pos_ += 2;
                skip_inline_ws();
                std::string name = bare_key();
                skip_inline_ws();
                if (!starts("]]")) fail("expected ]]");
                pos_ += 2;
                end_of_line();
                std::vector<Table>* arr = nullptr;
                for (auto& [k, v] : doc.arrays)
                    if (k == name) arr = &v;
                if (doc.root.has(name)) fail("key '" + name + "' is both a value and a table");
                if (!arr) {
                    doc.arrays.emplace_back(name, std::vector<Table>{});
                    arr = &doc.arrays.back().second;
                }
                arr->emplace_back();
                cur = &arr->back();
                continue;
            }
            std::string key = bare_key();
            skip_inline_ws();
            if (eof() || peek() != '=') fail("expected '=' after key '" + key + "'");
            ++pos_;
            skip_inline_ws();
            Value v = value();
            end_of_line();
            if (cur->has(key)) fail("duplicate key '" + key + "'");
            if (cur == &doc.root)
                for (auto& [k, _] : doc.arrays)
                    if (k == key) fail("key '" + key + "' is both a value and a table");
            cur->items.emplace_back(std::move(key), std::move(v));
        }
        return doc;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
    int line_ = 1;

    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }
    bool starts(std::string_view p) const { return s_.substr(pos_, p.size()) == p; }

    [[noreturn]] void fail(const std::string& why) const {
        throw TomlError("line " + std::to_string(line_) + ": " + why);
    }

    void skip_inline_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t')) ++pos_;
    }
    void skip_comment() {
        if (!eof() && peek() == '#')
            while (!eof() && peek() != '\n') {
                auto c = static_cast<unsigned char>(peek());
                if (c < 0x20 && c != '\t' && c != '\r') fail("control character in comment");
                ++pos_;
            }
    }
    bool newline() {
        if (starts("\r\n")) {
            pos_ += 2;
            ++line_;
            return true;
        }
        if (!eof() && peek() == '\n') {
            ++pos_;
            ++line_;
            return true;
        }
        return false;
    }
    void skip_ws_and_newlines() {
        while (true) {
            skip_inline_ws();
            skip_comment();
            if (!newline()) return;
        }
    }
    void end_of_line() {
        skip_inline_ws();
        skip_comment();
        if (eof()) return;
        if (!newline()) fail("unexpected text after value");
    }

    std::string bare_key() {
        std::size_t b = pos_;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '-'))
            ++pos_;
        if (pos_ == b) fail("expected a bare key");
        if (!eof() && peek() == '.') fail("dotted keys are not supported");
        return std::string(s_.substr(b, pos_ - b));
    }

    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    std::string string_value() {
        if (starts("\"\"\"") || starts("'''")) fail("multi-line strings are not supported");
        char q = peek();
        ++pos_;
        std::string out;
        while (true) {
            if (eof() || peek() == '\n' || peek() == '\r') fail("unterminated string");
            char c = peek();
            ++pos_;
            if (c == q) return out;
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') fail("control character in string");
            if (q == '\'' || c != '\\') {
                out.push_back(c);
                continue;
            }
            if (eof()) fail("unterminated escape");
            char e = peek();
            ++pos_;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u':
            case 'U': {
                std::size_t n = e == 'u' ? 4 : 8;
                if (pos_ + n > s_.size()) fail("short unicode escape");
                std::uint32_t cp = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    char h = s_[pos_ + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= std::uint32_t(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= std::uint32_t(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= std::uint32_t(h - 'A' + 10);
                    else fail("bad unicode escape");
                }
                pos_ += n;
                if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) fail("bad unicode scalar");
                put_utf8(out, cp);
                break;
            }
            default: fail(std::string("unknown escape \\") + e);
            }
        }
    }

    Value value() {
        if (eof()) fail("missing value");
        char c = peek();
        if (c == '"' || c == '\'') return string_value();
        if (c == '[') {
            ++pos_;
            std::vector<std::string> out;
            while (true) {
                skip_ws_and_newlines();
                if (eof()) fail("unterminated array");
                if (peek() == ']') {
                    ++pos_;
                    return out;
                }
                if (peek() != '"' && peek() != '\'') fail("only arrays of strings are supported");
                out.push_back(string_value());
                skip_ws_and_newlines();
                if (eof()) fail("unterminated array");
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                if (peek() != ']') fail("expected ',' or ']' in array");
            }
        }
        if (starts("true")) {
            pos_ += 4;
            return true;
        }
        if (starts("false")) {
            pos_ += 5;
            return false;
        }
        // Decimal integer; floats, dates, hex/octal and inline tables fail closed.
        std::size_t b = pos_;
        if (peek() == '+' || peek() == '-') ++pos_;
        std::string digits;
        bool last_us = true;
        while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
            if (peek() == '_') {
                if (last_us) fail("bad integer");
                last_us = true;
            } else {
                digits.push_back(peek());
                last_us = false;
            }
            ++pos_;
        }
        if (digits.empty() || last_us) fail("unsupported value");
        if (digits.size() > 1 && digits[0] == '0') fail("leading zero in integer");
        if (!eof() && peek() != ' ' && peek() != '\t' && peek() != '#' && peek() != '\n' && peek() != '\r')
            fail("unsupported value");
        try {
            return static_cast<std::int64_t>(std::stoll((s_[b] == '-' ? "-" : "") + digits));
        } catch (const std::exception&) {
            fail("integer out of range");
        }
    }
};

}  // namespace

TomlDoc parse_toml(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    return Parser(text).run();
}

}  // namespace prism::deps
