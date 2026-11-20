// Device-model parser and C emitter (docs/DEVICE_MODEL.md section 2).
//
// The format is a small block language; expressions and statements are a
// restricted C subset whose identifiers are checked against the model's
// declared names at parse time. The emitted C is compiled by the same clang
// front end as the driver, so every operator has its ISO C meaning on
// unsigned 64-bit values (division and modulo are not in the subset: a model
// must not contain undefined behaviour of its own).
#include "prism/module.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>

namespace prism::module {

namespace {

struct Tok {
    enum K { Ident, Int, Str, Punct, End } k = End;
    std::string s;
    std::uint64_t v = 0;
    int line = 0;
};

class Lexer {
public:
    Lexer(std::string_view t, std::string f) : text_(t), file_(std::move(f)) { run(); }
    std::vector<Tok> toks;
    const std::string& file() const { return file_; }

private:
    void run() {
        int line = 1;
        std::size_t i = 0;
        auto n = text_.size();
        while (i < n) {
            char c = text_[i];
            if (c == '\n') { ++line; ++i; continue; }
            if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
            if (c == '#' || (c == '/' && i + 1 < n && text_[i + 1] == '/')) {
                while (i < n && text_[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && text_[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(text_[i] == '*' && text_[i + 1] == '/')) {
                    if (text_[i] == '\n') ++line;
                    ++i;
                }
                i += 2;
                continue;
            }
            Tok t;
            t.line = line;
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                std::size_t j = i;
                while (j < n && (std::isalnum(static_cast<unsigned char>(text_[j])) || text_[j] == '_')) ++j;
                t.k = Tok::Ident;
                t.s = std::string(text_.substr(i, j - i));
                i = j;
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                std::size_t j = i;
                int base = 10;
                if (c == '0' && j + 1 < n && (text_[j + 1] == 'x' || text_[j + 1] == 'X')) { base = 16; j += 2; }
                std::size_t st = j;
                while (j < n && std::isxdigit(static_cast<unsigned char>(text_[j])) && (base == 16 || std::isdigit(static_cast<unsigned char>(text_[j])))) ++j;
                if (j == st) throw ParseError(file_, line, "bad number");
                t.k = Tok::Int;
                t.v = std::stoull(std::string(text_.substr(st, j - st)), nullptr, base);
                t.s = std::string(text_.substr(i, j - i));
                // integer suffixes (u, l) are accepted and ignored
                while (j < n && (text_[j] == 'u' || text_[j] == 'U' || text_[j] == 'l' || text_[j] == 'L')) ++j;
                i = j;
            } else if (c == '"') {
                std::size_t j = i + 1;
                std::string s;
                while (j < n && text_[j] != '"') {
                    if (text_[j] == '\\' && j + 1 < n) { s += text_[j + 1]; j += 2; continue; }
                    if (text_[j] == '\n') throw ParseError(file_, line, "unterminated string");
                    s += text_[j++];
                }
                if (j >= n) throw ParseError(file_, line, "unterminated string");
                t.k = Tok::Str;
                t.s = std::move(s);
                i = j + 1;
            } else {
                static const char* two[] = {"<<=", ">>=", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "|=", "&=", "^=", "+=", "-=", ".."};
                bool done = false;
                for (auto* p : two) {
                    std::size_t L = std::char_traits<char>::length(p);
                    if (text_.substr(i, L) == p) { t.k = Tok::Punct; t.s = p; i += L; done = true; break; }
                }
                if (!done) { t.k = Tok::Punct; t.s = std::string(1, c); ++i; }
            }
            toks.push_back(std::move(t));
        }
        Tok e;
        e.k = Tok::End;
        e.line = line;
        toks.push_back(e);
    }
    std::string_view text_;
    std::string file_;
};

std::string hexs(std::uint64_t v) {
    std::ostringstream o;
    o << "0x" << std::hex << v << "ULL";
    return o.str();
}

std::string maskc(unsigned width) {
    return width >= 64 ? "0xffffffffffffffffULL" : hexs((std::uint64_t{1} << width) - 1);
}

std::string cstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        o += c;
    }
    return o + "\"";
}

class Parser {
public:
    Parser(std::string_view text, const std::string& file) : lx_(text, file) {}

    DevModel parse() {
        for (std::size_t i = 0; i + 1 < lx_.toks.size(); ++i) {
            const auto& t = lx_.toks[i];
            if (t.k != Tok::Ident || (t.s != "state" && t.s != "register") || lx_.toks[i + 1].k != Tok::Ident) continue;
            names_.insert(lx_.toks[i + 1].s);
            // width: `: uN` (state) or `@ off : uN` (register)
            for (std::size_t j = i + 2; j + 1 < lx_.toks.size() && j < i + 8; ++j)
                if (lx_.toks[j].k == Tok::Punct && lx_.toks[j].s == ":" && lx_.toks[j + 1].k == Tok::Ident) {
                    const auto& w = lx_.toks[j + 1].s;
                    widths_[lx_.toks[i + 1].s] = w == "u8" ? 8u : w == "u16" ? 16u : w == "u32" ? 32u : 64u;
                    break;
                }
        }
        expect_ident("model");
        m_.file = lx_.file();
        m_.name = ident("model name");
        punct("{");
        while (!is_punct("}")) item();
        punct("}");
        if (peek().k != Tok::End) err("text after the model's closing brace");
        finish();
        return std::move(m_);
    }

private:
    const Tok& peek(int o = 0) const { return lx_.toks[std::min(pos_ + static_cast<std::size_t>(o), lx_.toks.size() - 1)]; }
    Tok take() { return lx_.toks[std::min(pos_++, lx_.toks.size() - 1)]; }
    [[noreturn]] void err(const std::string& w) const { throw ParseError(lx_.file(), peek().line, w); }
    bool is_punct(const char* p, int o = 0) const { return peek(o).k == Tok::Punct && peek(o).s == p; }
    bool is_ident(const char* p, int o = 0) const { return peek(o).k == Tok::Ident && peek(o).s == p; }
    void punct(const char* p) {
        if (!is_punct(p)) err(std::string("expected '") + p + "'");
        take();
    }
    void expect_ident(const char* p) {
        if (!is_ident(p)) err(std::string("expected '") + p + "'");
        take();
    }
    std::string ident(const char* what) {
        if (peek().k != Tok::Ident) err(std::string("expected ") + what);
        return take().s;
    }
    std::uint64_t integer(const char* what) {
        if (peek().k != Tok::Int) err(std::string("expected ") + what);
        return take().v;
    }
    std::string str(const char* what) {
        if (peek().k != Tok::Str) err(std::string("expected ") + what);
        return take().s;
    }

    // ---- names ----
    std::set<std::string> names_;      // state, registers (stored value): every one declared anywhere in the model
    std::set<std::string> declared_;
    std::map<std::string, unsigned> widths_;
    void declare(const std::string& n, int line) {
        static const std::set<std::string> reserved{"value", "result", "index", "offset", "nondet", "model", "register",
                                                    "state", "event", "irq", "group", "init", "policy", "window",
                                                    "doc", "source", "validation", "dma", "if", "else", "assume",
                                                    "require", "havoc", "where", "read", "write", "on_read", "in",
                                                    "unconstrained", "reset", "mask", "RW", "RO", "WO", "W1C"};
        if (reserved.count(n)) throw ParseError(lx_.file(), line, "'" + n + "' is a reserved word");
        if (!declared_.insert(n).second) throw ParseError(lx_.file(), line, "name '" + n + "' declared twice");
    }

    // ---- expressions ----
    // Consumes tokens up to (not including) a terminator at depth 0 and
    // returns the C text. Identifiers: declared names, plus the scope words.
    std::string expr(const std::set<std::string>& extra, const char* stop1, const char* stop2 = nullptr) {
        std::string out;
        int depth = 0;
        int count = 0;
        for (;;) {
            const Tok& t = peek();
            if (t.k == Tok::End) err("unexpected end of file in an expression");
            if (depth == 0 && t.k == Tok::Punct && (t.s == stop1 || (stop2 && t.s == stop2))) break;
            take();
            ++count;
            switch (t.k) {
                case Tok::Int: out += hexs(t.v) + " "; break;
                case Tok::Str: throw ParseError(lx_.file(), t.line, "string in an expression");
                case Tok::Ident: {
                    if (t.s == "nondet" && is_punct("(") && is_punct(")", 1)) {
                        take(); take();
                        out += "prism_nondet_u64() ";
                    } else if (names_.count(t.s)) {
                        out += "dm_" + t.s + " ";
                    } else if (extra.count(t.s)) {
                        out += "dm_" + t.s + " ";
                    } else {
                        throw ParseError(lx_.file(), t.line, "unknown name '" + t.s + "' in an expression");
                    }
                    break;
                }
                case Tok::Punct: {
                    static const std::set<std::string> ok{"+", "-", "*", "<<", ">>", "&", "|", "^", "~", "!", "&&", "||",
                                                          "==", "!=", "<", ">", "<=", ">=", "?", ":", "(", ")"};
                    if (t.s == "/" || t.s == "%") throw ParseError(lx_.file(), t.line, "division and modulo are not in the model language");
                    if (!ok.count(t.s)) throw ParseError(lx_.file(), t.line, "'" + t.s + "' is not allowed in an expression");
                    if (t.s == "(") ++depth;
                    if (t.s == ")") { if (--depth < 0) throw ParseError(lx_.file(), t.line, "unbalanced ')'"); }
                    out += t.s + " ";
                    break;
                }
                default: break;
            }
        }
        if (count == 0) err("empty expression");
        if (depth != 0) err("unbalanced '('");
        return "(" + out + ")";
    }

    // ---- statements ----
    std::string stmts(const std::set<std::string>& extra, unsigned lhs_default_width = 64) {
        std::string out;
        punct("{");
        while (!is_punct("}")) out += stmt(extra, lhs_default_width);
        punct("}");
        return out;
    }

    unsigned width_of(const std::string& n) const {
        auto it = widths_.find(n);
        return it == widths_.end() ? 64u : it->second;
    }

    std::string stmt(const std::set<std::string>& extra, unsigned) {
        const Tok& t = peek();
        if (t.k != Tok::Ident) err("expected a statement");
        if (t.s == "if") {
            take();
            punct("(");
            std::string c = expr(extra, ")");
            punct(")");
            std::string body = stmts(extra);
            std::string r = "if (" + c + ") {\n" + body + "}\n";
            if (is_ident("else")) {
                take();
                if (is_ident("if")) r += "else {\n" + stmt(extra, 64) + "}\n";
                else r += "else {\n" + stmts(extra) + "}\n";
            }
            return r;
        }
        if (t.s == "assume") {
            take();
            std::string c = expr(extra, ";");
            punct(";");
            return "__VERIFIER_assume(" + c + ");\n";
        }
        if (t.s == "require") {
            take();
            std::string c = expr(extra, ":");
            punct(":");
            std::string msg = str("a message string");
            punct(";");
            return "PRISM_DEV_REQUIRE(" + c + ", " + cstr(msg) + ");\n";
        }
        if (t.s == "havoc") {
            take();
            std::string n = ident("a state variable or register");
            if (!names_.count(n)) throw ParseError(lx_.file(), t.line, "havoc of unknown name '" + n + "'");
            std::string r = "dm_" + n + " = prism_nondet_u64() & " + maskc(width_of(n)) + ";\n";
            if (is_ident("where")) {
                take();
                std::string c = expr(extra, ";");
                r += "__VERIFIER_assume(" + c + ");\n";
            }
            punct(";");
            return r;
        }
        // assignment
        std::string n = take().s;
        bool ok = names_.count(n) || extra.count(n);
        if (!ok) throw ParseError(lx_.file(), t.line, "unknown name '" + n + "' in a statement");
        if (n == "value" || n == "index" || n == "offset")
            throw ParseError(lx_.file(), t.line, "'" + n + "' is read-only");
        std::string op;
        if (peek().k == Tok::Punct && (peek().s == "=" || peek().s == "|=" || peek().s == "&=" || peek().s == "^=" ||
                                       peek().s == "+=" || peek().s == "-=" || peek().s == "<<=" || peek().s == ">>="))
            op = take().s;
        else
            err("expected an assignment operator");
        std::string rhs = expr(extra, ";");
        punct(";");
        unsigned w = extra.count(n) ? 64 : width_of(n);
        std::string lhs = "dm_" + n;
        if (op == "=") return lhs + " = " + rhs + " & " + maskc(w) + ";\n";
        std::string bop = op.substr(0, op.size() - 1);
        return lhs + " = (" + lhs + " " + bop + " " + rhs + ") & " + maskc(w) + ";\n";
    }

    // ---- items ----
    void item() {
        const Tok& t = peek();
        if (t.k != Tok::Ident) err("expected a model item");
        const std::string kw = t.s;
        if (kw == "doc") { take(); m_.doc.push_back(str("text")); punct(";"); return; }
        if (kw == "source") { take(); m_.sources.push_back(str("text")); punct(";"); return; }
        if (kw == "validation") {
            take();
            m_.validation = ident("unvalidated | datasheet | differential");
            if (m_.validation != "unvalidated" && m_.validation != "datasheet" && m_.validation != "differential")
                err("validation must be unvalidated, datasheet or differential");
            m_.validation_how = str("how the model was validated");
            punct(";");
            return;
        }
        if (kw == "window") { take(); m_.window = integer("a size"); punct(";"); return; }
        if (kw == "policy") { take(); policy(); return; }
        if (kw == "state") { take(); state_var(); return; }
        if (kw == "register") { take(); reg(false); return; }
        if (kw == "group") { take(); reg(true); return; }
        if (kw == "init") { take(); m_.init_body = stmts({}); return; }
        if (kw == "event") {
            take();
            Event e;
            e.line = t.line;
            e.name = ident("an event name");
            e.body = stmts({});
            m_.events.push_back(std::move(e));
            return;
        }
        if (kw == "irq") {
            take();
            IrqLine l;
            l.name = ident("an irq line name");
            punct("=");
            l.level_expr = expr({}, ";");
            punct(";");
            m_.irqs.push_back(std::move(l));
            return;
        }
        if (kw == "dma") {
            take();
            std::string text;
            while (!is_punct(";")) {
                if (peek().k == Tok::End) err("unterminated dma rule");
                Tok x = take();
                text += (x.k == Tok::Str ? "\"" + x.s + "\"" : x.s) + " ";
            }
            punct(";");
            m_.dma_notes.push_back(text);
            return;
        }
        err("unknown model item '" + kw + "'");
    }

    Policy pol(const char* what) {
        std::string p = ident(what);
        if (p == "zero") return Policy::Zero;
        if (p == "unconstrained") return Policy::Unconstrained;
        if (p == "violation") return Policy::Violation;
        if (p == "ignore") return Policy::Ignore;
        err("policy must be zero, unconstrained, violation or ignore");
    }
    void policy() {
        std::string w = ident("a policy name");
        if (w == "unmapped_read") m_.unmapped_read = pol("a policy");
        else if (w == "unmapped_write") m_.unmapped_write = pol("a policy");
        else if (w == "wo_read") m_.wo_read = pol("a policy");
        else if (w == "ro_write") m_.ro_write = pol("a policy");
        else if (w == "width") {
            std::string v = ident("strict | any");
            if (v != "strict" && v != "any") err("width policy must be strict or any");
            m_.strict_width = v == "strict";
        } else err("unknown policy '" + w + "'");
        punct(";");
    }

    unsigned parse_width() {
        std::string u = ident("u8, u16, u32 or u64");
        if (u == "u8") return 8;
        if (u == "u16") return 16;
        if (u == "u32") return 32;
        if (u == "u64") return 64;
        err("width must be u8, u16, u32 or u64");
    }

    void state_var() {
        StateVar s;
        int line = peek().line;
        s.name = ident("a state variable name");
        declare(s.name, line);
        punct(":");
        s.width = parse_width();
        if (is_punct("=")) {
            take();
            if (is_ident("nondet")) { take(); punct("("); punct(")"); s.init_nondet = true; }
            else s.init = integer("an initial value") & (s.width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << s.width) - 1));
        }
        punct(";");
        m_.state.push_back(std::move(s));
    }

    void reg(bool group) {
        Register r;
        r.line = peek().line;
        r.group = group;
        r.name = ident("a register name");
        // a group's stored value is not a name; `index` is
        if (!group) declare(r.name, r.line);
        else if (!names_.insert("grp:" + r.name).second) err("name declared twice");
        punct("@");
        r.offset = integer("an offset");
        if (group) { punct(".."); r.group_end = integer("an end offset"); }
        punct(":");
        r.width = parse_width();
        std::string a = ident("RW, RO, WO or W1C");
        if (a == "RW") r.access = Access::RW;
        else if (a == "RO") r.access = Access::RO;
        else if (a == "WO") r.access = Access::WO;
        else if (a == "W1C") r.access = Access::W1C;
        else err("access must be RW, RO, WO or W1C");
        if (r.width < 64) r.mask = (std::uint64_t{1} << r.width) - 1;
        std::set<std::string> extra{"value", "result"};
        if (group) extra.insert("index");
        while (is_ident("reset") || is_ident("mask")) {
            std::string w = take().s;
            std::uint64_t v = integer("a value");
            if (w == "reset") r.reset = v; else r.mask = v;
        }
        m_.regs.push_back(r);          // visible to width_of in its own body
        Register& rr = m_.regs.back();
        if (is_punct(";")) { take(); return; }
        punct("{");
        while (!is_punct("}")) {
            std::string kw = ident("read, on_read or write");
            if (kw == "read") {
                if (is_punct("=")) {
                    take();
                    std::set<std::string> ex{"index"};
                    rr.read_expr = expr(ex, ";");
                    punct(";");
                } else if (is_ident("in")) {
                    take();
                    std::set<std::string> ex{"result", "index"};
                    rr.read_in = expr(ex, ";");
                    punct(";");
                } else if (is_ident("unconstrained")) {
                    take();
                    punct(";");
                    rr.read_unconstrained = true;
                } else err("expected '=', 'in' or 'unconstrained' after read");
            } else if (kw == "on_read") {
                rr.on_read = stmts(group ? std::set<std::string>{"index"} : std::set<std::string>{});
            } else if (kw == "write") {
                rr.has_write_body = true;
                rr.write_body = stmts(extra);
            } else err("unknown register item '" + kw + "'");
        }
        punct("}");
    }

    void finish() {
        // overlapping registers
        for (std::size_t i = 0; i < m_.regs.size(); ++i)
            for (std::size_t j = i + 1; j < m_.regs.size(); ++j) {
                auto& a = m_.regs[i];
                auto& b = m_.regs[j];
                auto aend = a.group ? a.group_end : a.offset + a.width / 8;
                auto bend = b.group ? b.group_end : b.offset + b.width / 8;
                if (a.offset < bend && b.offset < aend)
                    throw ParseError(lx_.file(), b.line, "register " + b.name + " overlaps " + a.name);
            }
        for (auto& r : m_.regs)
            if ((r.group ? r.group_end : r.offset + r.width / 8) > m_.window)
                throw ParseError(lx_.file(), r.line, "register " + r.name + " lies outside the window");
    }

    Lexer lx_;
    DevModel m_;
    std::size_t pos_ = 0;
};

const char* policy_name(Policy p) {
    switch (p) {
        case Policy::Zero: return "zero";
        case Policy::Unconstrained: return "unconstrained";
        case Policy::Violation: return "violation";
        case Policy::Ignore: return "ignore";
    }
    return "?";
}

std::string indent(const std::string& s, const char* pad = "    ") {
    std::string o;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) o += std::string(pad) + l + "\n";
    return o;
}

}  // namespace

DevModel parse_device_model(std::string_view text, const std::string& file) {
    Parser p(text, file);
    // the lexer is built in the Parser's constructor; file is used for errors
    return p.parse();
}

std::string describe_device(const DevModel& m) {
    std::ostringstream o;
    o << "device model " << m.name << " (validation: " << m.validation << ")\n";
    for (auto& s : m.sources) o << "  source: " << s << "\n";
    for (auto& r : m.regs) {
        o << "  " << r.name << " @0x" << std::hex << r.offset << std::dec << " u" << r.width << " ";
        o << (r.access == Access::RW ? "RW" : r.access == Access::RO ? "RO" : r.access == Access::WO ? "WO" : "W1C");
        if (r.group) o << " (group to 0x" << std::hex << r.group_end << std::dec << ")";
        o << " reset 0x" << std::hex << r.reset << std::dec;
        o << (r.read_unconstrained ? " read: unconstrained" : !r.read_in.empty() ? " read: constrained" : !r.read_expr.empty() ? " read: function of state" : " read: stored value");
        o << "\n";
    }
    o << "  events: " << m.events.size() << ", irq lines: " << m.irqs.size() << "\n";
    o << "  policy: unmapped_read=" << policy_name(m.unmapped_read) << " unmapped_write=" << policy_name(m.unmapped_write)
      << " wo_read=" << policy_name(m.wo_read) << " ro_write=" << policy_name(m.ro_write)
      << " width=" << (m.strict_width ? "strict" : "any") << "\n";
    for (auto& d : m.dma_notes) o << "  dma rule (parsed, not enforced): " << d << "\n";
    return o.str();
}

std::string emit_device_c(const DevModel& m) {
    std::ostringstream o;
    o << "/* generated by prism-module from device model '" << m.name << "' (" << m.file << "): do not edit */\n";
    o << "#define PRISM_DEV_WINDOW " << hexs(m.window) << "\n";
    o << "#define PRISM_DEV_REQUIRE(c, text) PRISM_MOD_CHECK(c, protocol, \"" << m.name << ": \" text)\n";
    o << "#define PRISM_DEV_ACCESS_ERR(text) PRISM_MOD_CHECK(0, protocol, \"" << m.name << ": \" text)\n";
    // state
    for (auto& s : m.state)
        o << "static unsigned long long dm_" << s.name << " = " << (s.init_nondet ? "0" : hexs(s.init)) << ";\n";
    for (auto& r : m.regs)
        if (!r.group) o << "static unsigned long long dm_" << r.name << " = " << hexs(r.reset & (r.width >= 64 ? ~0ULL : ((1ULL << r.width) - 1))) << ";\n";
    o << "static unsigned long long dm_value, dm_result, dm_index;\n";

    // init
    o << "static void prism_dev_init(void) {\n";
    for (auto& s : m.state)
        if (s.init_nondet) o << "    dm_" << s.name << " = prism_nondet_u64() & " << maskc(s.width) << ";\n";
    o << indent(m.init_body);
    o << "}\n";

    // read
    o << "unsigned long long prism_dev_read(unsigned long off, unsigned width) {\n";
    o << "    unsigned long long dm_result = 0;\n    unsigned long long dm_index = 0;\n    (void)dm_index;\n";
    o << "    switch (off) {\n";
    auto width_check = [&](const Register& r, const char* what) {
        if (m.strict_width)
            o << "        if (width != " << r.width / 8 << ") PRISM_DEV_ACCESS_ERR(\"" << what << " of " << r.name
              << " with a width other than " << r.width / 8 << " bytes\");\n";
    };
    for (auto& r : m.regs) {
        if (r.group) continue;
        o << "    case " << hexs(r.offset) << ": {\n";
        width_check(r, "read");
        if (r.access == Access::WO) {
            if (m.wo_read == Policy::Violation) o << "        PRISM_DEV_ACCESS_ERR(\"read of write-only register " << r.name << "\");\n";
            else if (m.wo_read == Policy::Unconstrained) o << "        dm_result = prism_nondet_u64();\n";
            else o << "        dm_result = 0;\n";
            o << "        break;\n    }\n";
            continue;
        }
        o << indent(r.on_read, "        ");
        if (r.read_unconstrained) o << "        dm_result = prism_nondet_u64();\n";
        else if (!r.read_in.empty()) o << "        dm_result = prism_nondet_u64() & " << maskc(r.width) << ";\n        __VERIFIER_assume" << r.read_in << ";\n";
        else if (!r.read_expr.empty()) o << "        dm_result = " << r.read_expr << ";\n";
        else o << "        dm_result = dm_" << r.name << ";\n";
        o << "        break;\n    }\n";
    }
    o << "    default: {\n";
    for (auto& r : m.regs) {
        if (!r.group) continue;
        o << "        if (off >= " << hexs(r.offset) << " && off < " << hexs(r.group_end) << ") {\n";
        o << "            dm_index = (off - " << hexs(r.offset) << ") / " << r.width / 8 << ";\n";
        if (m.strict_width) o << "            if (width != " << r.width / 8 << ") PRISM_DEV_ACCESS_ERR(\"read of " << r.name << " with a width other than " << r.width / 8 << " bytes\");\n";
        o << indent(r.on_read, "            ");
        if (r.read_unconstrained) o << "            dm_result = prism_nondet_u64();\n";
        else if (!r.read_in.empty()) o << "            dm_result = prism_nondet_u64() & " << maskc(r.width) << ";\n            __VERIFIER_assume" << r.read_in << ";\n";
        else o << "            dm_result = " << (r.read_expr.empty() ? "0" : r.read_expr) << ";\n";
        o << "            break;\n        }\n";
    }
    // group fallthrough: compiled as a chain; unmapped handling after
    o << "        {\n";
    if (m.unmapped_read == Policy::Violation) o << "            PRISM_DEV_ACCESS_ERR(\"read of an unmapped offset\");\n";
    else if (m.unmapped_read == Policy::Unconstrained) o << "            dm_result = prism_nondet_u64();\n";
    else o << "            dm_result = 0;\n";
    o << "        }\n    }\n    }\n";
    o << "    return dm_result & (width >= 8 ? ~0ULL : ((1ULL << (width * 8)) - 1));\n}\n";

    // write
    o << "void prism_dev_write(unsigned long off, unsigned long long val, unsigned width) {\n";
    o << "    unsigned long long dm_value = val;\n    unsigned long long dm_index = 0;\n    (void)dm_value; (void)dm_index;\n";
    o << "    switch (off) {\n";
    for (auto& r : m.regs) {
        if (r.group) continue;
        o << "    case " << hexs(r.offset) << ": {\n";
        width_check(r, "write");
        if (r.access == Access::RO) {
            if (!r.has_write_body) {
                if (m.ro_write == Policy::Violation) o << "        PRISM_DEV_ACCESS_ERR(\"write to read-only register " << r.name << "\");\n";
                o << "        break;\n    }\n";
                continue;
            }
        }
        if (r.has_write_body) o << indent(r.write_body, "        ");
        else if (r.access == Access::W1C) o << "        dm_" << r.name << " = dm_" << r.name << " & ~(dm_value & " << hexs(r.mask) << ") & " << maskc(r.width) << ";\n";
        else o << "        dm_" << r.name << " = (dm_" << r.name << " & ~" << hexs(r.mask) << ") | (dm_value & " << hexs(r.mask) << ");\n        dm_" << r.name << " &= " << maskc(r.width) << ";\n";
        o << "        break;\n    }\n";
    }
    o << "    default: {\n";
    bool any_group_write = false;
    for (auto& r : m.regs) {
        if (!r.group) continue;
        any_group_write = true;
        o << "        if (off >= " << hexs(r.offset) << " && off < " << hexs(r.group_end) << ") {\n";
        if (r.access == Access::RO && m.ro_write == Policy::Violation && !r.has_write_body)
            o << "            PRISM_DEV_ACCESS_ERR(\"write to read-only register group " << r.name << "\");\n";
        else if (r.has_write_body) o << indent(r.write_body, "            ");
        o << "            break;\n        }\n";
    }
    (void)any_group_write;
    o << "        {\n";
    if (m.unmapped_write == Policy::Violation) o << "            PRISM_DEV_ACCESS_ERR(\"write to an unmapped offset\");\n";
    o << "        }\n    }\n    }\n}\n";

    // irq lines
    o << "int prism_dev_irq_level(unsigned line) {\n    switch (line) {\n";
    for (std::size_t i = 0; i < m.irqs.size(); ++i)
        o << "    case " << i << ": return (" << m.irqs[i].level_expr << ") != 0;\n";
    o << "    default: return 0;\n    }\n}\n";
    o << "#define PRISM_DEV_IRQ_LINES " << m.irqs.size() << "\n";

    // events
    o << "#define PRISM_DEV_EVENTS " << m.events.size() << "\n";
    o << "static void prism_dev_event(void) {\n";
    if (m.events.empty()) o << "    return;\n";
    else {
        o << "    unsigned c = prism_nondet_choice(" << m.events.size() << ");\n";
        for (std::size_t i = 0; i < m.events.size(); ++i) {
            o << "    " << (i ? "else " : "") << "if (c == " << i << ") {  /* event " << m.events[i].name << " */\n";
            o << indent(m.events[i].body);
            o << "    }\n";
        }
    }
    o << "}\n";
    return o.str();
}

}  // namespace prism::module
