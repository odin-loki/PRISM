// Module description parser, entry-point discovery and harness generator
// (docs/DEVICE_MODEL.md sections 4 and 5).
#include "prism/module.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace prism::module {

namespace {

class SpecScanner {
public:
    SpecScanner(std::string_view t, std::string f) : t_(t), f_(std::move(f)) {}

    bool eof() { skip(); return i_ >= t_.size(); }
    std::string word() {
        skip();
        std::size_t j = i_;
        while (j < t_.size() && (std::isalnum(static_cast<unsigned char>(t_[j])) || t_[j] == '_' || t_[j] == '-' || t_[j] == '.' ||
                                 t_[j] == ':' || t_[j] == '/' || t_[j] == '<' || t_[j] == '>'))
            ++j;
        if (j == i_) fail("expected a word");
        std::string w(t_.substr(i_, j - i_));
        i_ = j;
        return w;
    }
    std::string quoted() {
        skip();
        if (i_ >= t_.size() || t_[i_] != '"') fail("expected a quoted string");
        std::size_t j = ++i_;
        std::string s;
        while (j < t_.size() && t_[j] != '"') {
            if (t_[j] == '\\' && j + 1 < t_.size()) { s += t_[j + 1]; j += 2; continue; }
            s += t_[j++];
        }
        if (j >= t_.size()) fail("unterminated string");
        i_ = j + 1;
        return s;
    }
    bool peek_char(char c) { skip(); return i_ < t_.size() && t_[i_] == c; }
    void expect(char c) {
        skip();
        if (i_ >= t_.size() || t_[i_] != c) fail(std::string("expected '") + c + "'");
        ++i_;
    }
    bool peek_quote() { skip(); return i_ < t_.size() && t_[i_] == '"'; }
    // raw text up to the next ';' (not consumed), trimmed
    std::string until_semicolon() {
        std::size_t st = i_;
        while (i_ < t_.size() && t_[i_] != ';') ++i_;
        std::string r(t_.substr(st, i_ - st));
        auto a = r.find_first_not_of(" \t\r\n");
        auto b = r.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : r.substr(a, b - a + 1);
    }
    // raw text of a {...} block; the braces are consumed
    std::string block() {
        expect('{');
        int depth = 1;
        std::size_t st = i_;
        while (i_ < t_.size() && depth > 0) {
            char c = t_[i_];
            if (c == '\n') ++line_;
            if (c == '"' || c == '\'') {
                char q = c;
                ++i_;
                while (i_ < t_.size() && t_[i_] != q) { if (t_[i_] == '\\') ++i_; ++i_; }
            } else if (c == '/' && i_ + 1 < t_.size() && t_[i_ + 1] == '/') {
                while (i_ < t_.size() && t_[i_] != '\n') ++i_;
                continue;
            } else if (c == '/' && i_ + 1 < t_.size() && t_[i_ + 1] == '*') {
                i_ += 2;
                while (i_ + 1 < t_.size() && !(t_[i_] == '*' && t_[i_ + 1] == '/')) { if (t_[i_] == '\n') ++line_; ++i_; }
                i_ += 1;
            } else if (c == '{') ++depth;
            else if (c == '}') --depth;
            ++i_;
        }
        if (depth != 0) fail("unterminated block");
        return std::string(t_.substr(st, i_ - 1 - st));
    }
    [[noreturn]] void fail(const std::string& w) const { throw ParseError(f_, line_, w); }

private:
    void skip() {
        while (i_ < t_.size()) {
            char c = t_[i_];
            if (c == '\n') { ++line_; ++i_; }
            else if (std::isspace(static_cast<unsigned char>(c))) ++i_;
            else if (c == '#') { while (i_ < t_.size() && t_[i_] != '\n') ++i_; }
            else break;
        }
    }
    std::string_view t_;
    std::string f_;
    std::size_t i_ = 0;
    int line_ = 1;
};

bool yes(const std::string& w, SpecScanner& s) {
    if (w == "yes" || w == "true") return true;
    if (w == "no" || w == "false") return false;
    s.fail("expected yes or no");
}

}  // namespace

Spec parse_module_spec(std::string_view text, const std::string& file) {
    SpecScanner s(text, file);
    Spec sp;
    sp.file = file;
    bool named = false;
    while (!s.eof()) {
        std::string kw = s.word();
        if (kw == "module") { sp.name = s.word(); named = true; s.expect(';'); }
        else if (kw == "kernel") { sp.kernel = s.word(); s.expect(';'); if (sp.kernel != "linux" && sp.kernel != "toy") s.fail("kernel must be linux or toy"); }
        else if (kw == "driver") { sp.driver = s.quoted(); s.expect(';'); }
        else if (kw == "source") { sp.extra_sources.push_back(s.quoted()); s.expect(';'); }
        else if (kw == "device") { sp.device = s.peek_quote() ? s.quoted() : s.word(); s.expect(';'); }
        else if (kw == "stubs") {
            while (!s.peek_char(';')) sp.stubs.push_back(s.word());
            s.expect(';');
        } else if (kw == "steps") { sp.steps = std::stoi(s.word()); s.expect(';'); if (sp.steps < 0 || sp.steps > 8) s.fail("steps must be 0..8"); }
        else if (kw == "irq_points") {
            sp.irq_at_api = sp.irq_at_mmio = sp.irq_at_steps = false;
            while (!s.peek_char(';')) {
                std::string w = s.word();
                if (w == "api") sp.irq_at_api = true;
                else if (w == "mmio") sp.irq_at_mmio = true;
                else if (w == "steps") sp.irq_at_steps = true;
                else if (w != "none") s.fail("irq_points: api, mmio, steps or none");
            }
            s.expect(';');
        } else if (kw == "irq_spurious") {
            sp.irq_spurious = s.word();
            if (sp.irq_spurious != "auto" && sp.irq_spurious != "yes" && sp.irq_spurious != "no") s.fail("irq_spurious: auto, yes or no");
            s.expect(';');
        } else if (kw == "teardown_ops") { sp.teardown_ops = yes(s.word(), s); s.expect(';'); }
        else if (kw == "properties") {
            while (!s.peek_char(';')) { std::string w = s.word(); if (w != "all") sp.properties.push_back(w); }
            s.expect(';');
        } else if (kw == "assume") { sp.assumptions.push_back(s.quoted()); s.expect(';'); }
        else if (kw == "include") { sp.includes_pre += "#include " + s.word() + "\n"; s.expect(';'); }
        else if (kw == "patch") {
            auto trim = [](std::string t) {
                auto a = t.find_first_not_of(" \t\r\n");
                auto b = t.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
            };
            std::string o = trim(s.block());
            std::string n = trim(s.block());
            sp.patches.emplace_back(o, n);
            s.expect(';');
        }
        else if (kw == "define") {
            // `define NAME [VALUE];` or a function-like `define NAME(args) body;`: the rest up to the ';'
            std::string d = s.word();
            std::string rest = s.until_semicolon();
            if (!rest.empty() && rest[0] == '(') d += rest;
            else if (!rest.empty()) d += " " + rest;
            sp.defines.push_back(d);
            s.expect(';');
        }
        else if (kw == "env") sp.env += s.block() + "\n";
        else if (kw == "setup") sp.setup += s.block() + "\n";
        else if (kw == "probe") sp.probe = s.block();
        else if (kw == "remove") sp.remove = s.block();
        else if (kw == "op") {
            Op o;
            o.name = s.word();
            o.body = s.block();
            sp.ops.push_back(std::move(o));
        } else s.fail("unknown directive '" + kw + "'");
    }
    if (!named) throw ParseError(file, 1, "missing 'module NAME;'");
    if (sp.driver.empty()) throw ParseError(file, 1, "missing 'driver \"path\";'");
    if (sp.probe.empty()) throw ParseError(file, 1, "missing a probe { ... } block");
    if (sp.remove.empty()) throw ParseError(file, 1, "missing a remove { ... } block");
    return sp;
}

std::vector<Registered> discover_entry_points(std::string_view src, const Spec& spec) {
    // functions defined at column 0 of the file: `... name(` on a line that does not end in ';'
    std::set<std::string> defined;
    {
        static const std::regex def(R"(^[A-Za-z_][A-Za-z0-9_ \t\*]*[ \t\*]([A-Za-z_][A-Za-z0-9_]*)\s*\()", std::regex::ECMAScript);
        std::istringstream in{std::string(src)};
        std::string l;
        while (std::getline(in, l)) {
            std::smatch m;
            if (std::regex_search(l, m, def) && l.find(';') == std::string::npos) defined.insert(m[1]);
        }
    }
    std::vector<Registered> out;
    static const std::regex fld(R"(^\s*\.([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*,?\s*(/\*.*)?$)");
    std::istringstream in{std::string(src)};
    std::string l;
    int line = 0;
    std::string glue = spec.probe + spec.remove;
    for (auto& o : spec.ops) glue += o.body;
    glue += spec.setup + spec.env;
    while (std::getline(in, l)) {
        ++line;
        std::smatch m;
        if (std::regex_match(l, m, fld) && defined.count(m[2])) {
            Registered r;
            r.field = m[1];
            r.symbol = m[2];
            r.line = line;
            // exercised: the symbol is named in an entry block, or the field is reached through a table in one
            r.exercised = glue.find(r.symbol) != std::string::npos ||
                          (!spec.ops.empty() && glue.find("." + r.field) != std::string::npos) ||
                          glue.find("->" + r.field) != std::string::npos;
            out.push_back(std::move(r));
        }
    }
    return out;
}

}  // namespace prism::module
