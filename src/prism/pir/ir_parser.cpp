// Textual LLVM IR parser for the subset PIR consumes (docs/PIR.md).
//
// A real tokenizer + recursive-descent parser over what `opt -S` prints:
// types (iN, ptr, float kinds, structs, arrays, vectors, function types),
// values (locals, globals, integer literals, undef/poison/null, constant
// expressions skipped as opaque), and the instruction forms PIR translates.
// Any other opcode is kept with parsed=false so the translator can name it
// in an "UNENCODED: <op>" verdict instead of dropping it.

#include "prism/pir.hpp"

#include "fp.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace prism::pir::ir {
namespace {

struct Tok {
    enum Kind { End, Local, Global, Meta, Attr, Num, Str, Word, Punct };
    Kind kind = End;
    std::string text;  // Local/Global/Meta: name without sigil; Punct: the char
};

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
}

std::vector<Tok> lex(std::string_view s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    auto quoted = [&](std::size_t& j) {
        // s[j] == '"'
        std::string v;
        ++j;
        while (j < s.size() && s[j] != '"') v.push_back(s[j++]);
        if (j < s.size()) ++j;
        return v;
    };
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == ';') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '%' || c == '@') {
            Tok t;
            t.kind = c == '%' ? Tok::Local : Tok::Global;
            ++i;
            if (i < s.size() && s[i] == '"') {
                t.text = quoted(i);
            } else {
                while (i < s.size() && (word_char(s[i]) || s[i] == '-')) t.text.push_back(s[i++]);
            }
            out.push_back(std::move(t));
            continue;
        }
        if (c == '!') {
            Tok t;
            t.kind = Tok::Meta;
            ++i;
            if (i < s.size() && s[i] == '{') {
                t.text = "{";
                ++i;
            } else if (i < s.size() && s[i] == '"') {
                t.text = quoted(i);
            } else {
                while (i < s.size() && (word_char(s[i]) || s[i] == '-')) t.text.push_back(s[i++]);
            }
            out.push_back(std::move(t));
            continue;
        }
        if (c == '#') {
            Tok t;
            t.kind = Tok::Attr;
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) t.text.push_back(s[i++]);
            out.push_back(std::move(t));
            continue;
        }
        if (c == '"') {
            Tok t;
            t.kind = Tok::Str;
            t.text = quoted(i);
            out.push_back(std::move(t));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            Tok t;
            t.kind = Tok::Num;
            t.text.push_back(s[i++]);
            // integers, floats (1.5e+00), hex (0x...), hex floats (0xK...)
            while (i < s.size()) {
                char d = s[i];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '.') {
                    t.text.push_back(d);
                    ++i;
                } else if ((d == '+' || d == '-') && !t.text.empty() &&
                           (t.text.back() == 'e' || t.text.back() == 'E') &&
                           t.text.find("0x") == std::string::npos) {
                    t.text.push_back(d);
                    ++i;
                } else {
                    break;
                }
            }
            out.push_back(std::move(t));
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$') {
            Tok t;
            t.kind = Tok::Word;
            while (i < s.size() && word_char(s[i])) t.text.push_back(s[i++]);
            // c"..." string constants
            if (t.text == "c" && i < s.size() && s[i] == '"') {
                t.kind = Tok::Str;
                t.text = quoted(i);
            }
            out.push_back(std::move(t));
            continue;
        }
        if (c == '.' && s.substr(i, 3) == "...") {
            out.push_back({Tok::Word, "..."});
            i += 3;
            continue;
        }
        out.push_back({Tok::Punct, std::string(1, c)});
        ++i;
    }
    return out;
}

bool is_float_word(std::string_view w) {
    return w == "half" || w == "bfloat" || w == "float" || w == "double" || w == "x86_fp80" ||
           w == "fp128" || w == "ppc_fp128";
}

// IEEE bits of a floating-point literal of type `ty` (half/float/double):
// decimal ("1.500000e+00"), the double-precision hex form LLVM prints for
// float and double ("0x3FF8000000000000"), or "0xH3C00" for half.
std::optional<uint64_t> fp_literal(std::string_view s, std::string_view ty) {
    auto w = ty == "half" ? 16u : ty == "float" ? 32u : ty == "double" ? 64u : 0u;
    if (!w) return std::nullopt;
    auto hex = [](std::string_view h) -> std::optional<uint64_t> {
        if (h.empty() || h.size() > 16) return std::nullopt;
        uint64_t v = 0;
        auto r = std::from_chars(h.data(), h.data() + h.size(), v, 16);
        if (r.ec != std::errc() || r.ptr != h.data() + h.size()) return std::nullopt;
        return v;
    };
    if (s.starts_with("0xH")) {
        if (w != 16) return std::nullopt;
        return hex(s.substr(3));
    }
    uint64_t dbits = 0;
    if (s.starts_with("0x")) {
        if (s.size() > 2 && !std::isxdigit(static_cast<unsigned char>(s[2])))
            return std::nullopt;  // 0xK / 0xL / 0xM / 0xR: other formats
        auto h = hex(s.substr(2));
        if (!h) return std::nullopt;
        dbits = *h;
    } else {
        std::string t(s);
        char* end = nullptr;
        double d = std::strtod(t.c_str(), &end);
        if (end != t.c_str() + t.size()) return std::nullopt;
        std::memcpy(&dbits, &d, 8);
    }
    if (w == 64) return dbits;
    return fp::from_double_bits(dbits, w);
}

const std::unordered_set<std::string>& param_attr_words() {
    static const std::unordered_set<std::string> k{
        "noundef",   "signext",   "zeroext",  "inreg",        "noalias",  "nocapture",
        "nonnull",   "readonly",  "writeonly", "readnone",    "immarg",   "returned",
        "nest",      "swiftself", "swifterror", "swiftasync", "noext",    "nofree",
        "dead_on_unwind", "writable", "allocalign", "allocptr", "noundef",
        // with a parenthesised or numeric argument:
        "align",     "dereferenceable", "dereferenceable_or_null", "byval", "byref",
        "sret",      "inalloca",  "preallocated", "elementtype", "range", "nofpclass",
        "captures",  "initializes", "alignstack",
    };
    return k;
}

const std::unordered_set<std::string>& call_prefix_words() {
    static const std::unordered_set<std::string> k{
        // fast-math flags
        "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc", "fast",
        // calling conventions
        "ccc", "fastcc", "coldcc", "tailcc", "swiftcc", "swifttailcc", "cfguard_checkcc",
        "webkit_jscc", "anyregcc", "preserve_mostcc", "preserve_allcc", "preserve_nonecc",
        "cxx_fast_tlscc", "ghccc", "x86_stdcallcc", "x86_fastcallcc", "x86_thiscallcc",
        "x86_vectorcallcc", "x86_regcallcc", "x86_64_sysvcc", "win64cc", "intel_ocl_bicc",
        "spir_func", "spir_kernel",
    };
    return k;
}

struct P {
    const std::vector<Tok>& t;
    std::size_t i = 0;

    const Tok& peek(std::size_t k = 0) const {
        static const Tok end{};
        return i + k < t.size() ? t[i + k] : end;
    }
    bool at_end() const { return i >= t.size(); }
    bool is_punct(char c, std::size_t k = 0) const {
        auto& x = peek(k);
        return x.kind == Tok::Punct && x.text.size() == 1 && x.text[0] == c;
    }
    bool is_word(std::string_view w, std::size_t k = 0) const {
        auto& x = peek(k);
        return x.kind == Tok::Word && x.text == w;
    }
    Tok next() {
        if (at_end()) throw ParseError("unexpected end of instruction");
        return t[i++];
    }
    void expect_punct(char c) {
        if (!is_punct(c)) throw ParseError(std::string("expected '") + c + "'");
        ++i;
    }
    bool accept_punct(char c) {
        if (is_punct(c)) {
            ++i;
            return true;
        }
        return false;
    }
    bool accept_word(std::string_view w) {
        if (is_word(w)) {
            ++i;
            return true;
        }
        return false;
    }
    // Skip a balanced (), [], {} or <> group starting at the current token.
    void skip_group() {
        if (at_end()) return;
        auto open = peek().text;
        if (peek().kind != Tok::Punct) {
            ++i;
            return;
        }
        char o = open[0];
        char cl = o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : o == '<' ? '>' : 0;
        if (!cl) {
            ++i;
            return;
        }
        int depth = 0;
        while (!at_end()) {
            auto x = next();
            if (x.kind != Tok::Punct) continue;
            if (x.text[0] == o) ++depth;
            else if (x.text[0] == cl && --depth == 0) return;
        }
    }

    Type type() {
        Type ty = base_type();
        // function type: ret (params)
        while (true) {
            if (is_punct('(')) {
                Type fn;
                fn.kind = Type::Other;
                fn.text = ty.text + " (";
                ++i;
                bool first = true;
                while (!is_punct(')')) {
                    if (!first) expect_punct(',');
                    first = false;
                    if (is_word("...")) {
                        ++i;
                        fn.text += "...";
                        continue;
                    }
                    auto pt = type();
                    fn.elems.push_back(pt);
                    fn.text += pt.text;
                }
                expect_punct(')');
                fn.text += ")";
                fn.elems.insert(fn.elems.begin(), ty);
                ty = fn;
                continue;
            }
            if (is_punct('*')) {  // legacy typed pointer
                ++i;
                Type p;
                p.kind = Type::Ptr;
                p.text = ty.text + "*";
                ty = p;
                continue;
            }
            break;
        }
        return ty;
    }

    Type base_type() {
        Type ty;
        if (is_punct('{')) {
            ++i;
            ty.kind = Type::Struct;
            ty.text = "{ ";
            bool first = true;
            while (!is_punct('}')) {
                if (!first) expect_punct(',');
                first = false;
                auto e = type();
                ty.text += (ty.elems.empty() ? "" : ", ") + e.text;
                ty.elems.push_back(e);
            }
            expect_punct('}');
            ty.text += " }";
            return ty;
        }
        if (is_punct('<') && is_punct('{', 1)) {
            i += 2;
            ty.kind = Type::Struct;
            ty.text = "<{ ";
            bool first = true;
            while (!is_punct('}')) {
                if (!first) expect_punct(',');
                first = false;
                auto e = type();
                ty.text += (ty.elems.empty() ? "" : ", ") + e.text;
                ty.elems.push_back(e);
            }
            expect_punct('}');
            expect_punct('>');
            ty.text += " }>";
            return ty;
        }
        if (is_punct('[') || is_punct('<')) {
            bool vec = is_punct('<');
            ++i;
            std::string pre;
            if (accept_word("vscale")) {
                if (!accept_word("x")) throw ParseError("bad vscale vector type");
                pre = "vscale x ";
            }
            auto n = next();
            if (n.kind != Tok::Num) throw ParseError("expected element count");
            if (!accept_word("x")) throw ParseError("expected 'x' in aggregate type");
            auto e = type();
            expect_punct(vec ? '>' : ']');
            ty.kind = vec ? Type::Vector : Type::Array;
            ty.text = std::string(vec ? "<" : "[") + pre + n.text + " x " + e.text + (vec ? ">" : "]");
            ty.elems.push_back(e);
            return ty;
        }
        if (peek().kind == Tok::Local) {  // named struct type %struct.S
            ty.kind = Type::Struct;
            ty.text = "%" + next().text;
            return ty;
        }
        auto w = next();
        if (w.kind != Tok::Word) throw ParseError("expected type, got '" + w.text + "'");
        ty.text = w.text;
        if (w.text.size() > 1 && w.text[0] == 'i' &&
            std::all_of(w.text.begin() + 1, w.text.end(),
                        [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
            ty.kind = Type::Int;
            ty.bits = static_cast<unsigned>(std::stoul(w.text.substr(1)));
        } else if (w.text == "ptr") {
            ty.kind = Type::Ptr;
            if (is_word("addrspace")) {
                ++i;
                skip_group();
                ty.text = "ptr addrspace";
            }
        } else if (w.text == "void") {
            ty.kind = Type::Void;
        } else if (is_float_word(w.text)) {
            ty.kind = Type::Float;
        } else if (w.text == "label") {
            ty.kind = Type::Label;
        } else if (w.text == "metadata") {
            ty.kind = Type::Metadata;
        } else {
            ty.kind = Type::Other;
        }
        return ty;
    }

    void skip_param_attrs(std::string* keep = nullptr) {
        while (peek().kind == Tok::Word && param_attr_words().count(peek().text)) {
            auto w = next().text;
            if (keep) {
                if (!keep->empty()) *keep += ' ';
                *keep += w;
            }
            if (is_punct('(') && (w == "byval" || w == "sret" || w == "byref")) {
                // keep the pointee type: "byval(<type>)"
                ++i;
                auto ty = type();
                expect_punct(')');
                if (keep) *keep += "(" + ty.text + ")";
            } else if (is_punct('(')) {
                skip_group();
            } else if (w == "align" && peek().kind == Tok::Num) {
                auto n = next().text;
                if (keep) *keep += " " + n;
            }
        }
    }

    // `, align N` suffix of alloca/load/store (other trailing words ignored)
    unsigned trailing_align() {
        unsigned a = 0;
        while (!at_end()) {
            if (accept_punct(',')) continue;
            if (accept_word("align") && peek().kind == Tok::Num) {
                a = static_cast<unsigned>(std::stoul(next().text));
                continue;
            }
            ++i;
        }
        return a;
    }

    // Constant expression after its opcode word: getelementptr [flags] (T, ops...)
    // or a cast (T v to T2).
    void const_expr(Value& v, const std::string& op) {
        v.kind = Value::ConstExpr;
        v.ce_op = op;
        v.text = op;
        while (peek().kind == Tok::Word) v.ce_flags.push_back(next().text);
        if (!is_punct('(')) throw ParseError("constant expression without operands");
        ++i;
        if (op == "getelementptr") {
            v.ce_ty = type();
            while (accept_punct(',')) {
                if (is_word("inrange")) {
                    ++i;
                    if (is_punct('(')) skip_group();
                }
                v.elems.push_back(typed_operand());
            }
        } else {
            v.elems.push_back(typed_operand());
            if (!accept_word("to")) throw ParseError("expected 'to' in constant cast");
            v.ce_ty = type();
        }
        expect_punct(')');
    }

    // [ T v, ... ] / { T v, ... } / <{ ... }> aggregate constant elements
    void aggregate(Value& v) {
        v.kind = Value::Aggregate;
        v.text = "aggregate";
        bool packed = is_punct('<') && is_punct('{', 1);
        if (packed) ++i;
        char open = peek().text[0];
        char close = open == '[' ? ']' : open == '{' ? '}' : '>';
        ++i;
        while (!is_punct(close)) {
            if (!v.elems.empty()) expect_punct(',');
            v.elems.push_back(typed_operand());
        }
        expect_punct(close);
        if (packed) expect_punct('>');
    }

    Value value(const Type& ty) {
        Value v;
        auto& x = peek();
        switch (x.kind) {
            case Tok::Local:
                v.kind = Value::Local;
                v.name = next().text;
                v.text = "%" + v.name;
                return v;
            case Tok::Global:
                v.kind = Value::Global;
                v.name = next().text;
                v.text = "@" + v.name;
                return v;
            case Tok::Num: {
                auto s = next().text;
                v.text = s;
                bool neg = !s.empty() && s[0] == '-';
                std::string_view digits(s);
                if (neg) digits.remove_prefix(1);
                bool plain = !digits.empty() &&
                             std::all_of(digits.begin(), digits.end(), [](char ch) {
                                 return std::isdigit(static_cast<unsigned char>(ch));
                             });
                if (ty.kind == Type::Float) {
                    if (auto b = fp_literal(s, ty.text)) {
                        v.kind = Value::Fp;
                        v.bits = *b;
                    } else {
                        v.kind = Value::Other;
                    }
                    return v;
                }
                if (!plain || ty.kind != Type::Int) {
                    v.kind = Value::Other;
                    return v;
                }
                unsigned long long mag = 0;
                auto r = std::from_chars(digits.data(), digits.data() + digits.size(), mag);
                if (r.ec != std::errc()) {
                    v.kind = Value::Other;  // wider than 64 bits
                    return v;
                }
                v.kind = Value::Int;
                v.negative = neg;
                v.bits = neg ? (~static_cast<uint64_t>(mag) + 1) : static_cast<uint64_t>(mag);
                return v;
            }
            case Tok::Word: {
                auto w = next().text;
                v.text = w;
                if (w == "true" || w == "false") {
                    v.kind = Value::Int;
                    v.bits = w == "true" ? 1 : 0;
                } else if (w == "undef") {
                    v.kind = Value::Undef;
                } else if (w == "poison") {
                    v.kind = Value::Poison;
                } else if (w == "null") {
                    v.kind = Value::Null;
                } else if (w == "zeroinitializer") {
                    v.kind = Value::Zero;
                    if (ty.kind == Type::Float && fp_literal("0x0", ty.text)) {
                        v.kind = Value::Fp;
                        v.bits = 0;
                    }
                    if (ty.kind == Type::Int) {
                        v.kind = Value::Int;
                        v.bits = 0;
                    }
                } else if (w == "getelementptr" || w == "ptrtoint" || w == "inttoptr" || w == "bitcast" ||
                           w == "addrspacecast") {
                    auto save = i;
                    try {
                        const_expr(v, w);
                    } catch (const ParseError&) {
                        i = save;
                        v = Value{};
                        v.text = w;
                        v.kind = Value::Other;
                        while (peek().kind == Tok::Word) ++i;
                        if (is_punct('(')) skip_group();
                    }
                } else {
                    // other constant expression: kept opaque
                    v.kind = Value::Other;
                    while (peek().kind == Tok::Word) ++i;  // inbounds, nuw ...
                    if (is_punct('(')) skip_group();
                }
                return v;
            }
            case Tok::Str: {
                v.kind = Value::Str;
                auto raw = next().text;
                v.text = "c\"" + raw + "\"";
                for (std::size_t k = 0; k < raw.size(); ++k) {
                    if (raw[k] == '\\' && k + 2 < raw.size() &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 1])) &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 2]))) {
                        v.bytes.push_back(static_cast<char>(std::stoi(raw.substr(k + 1, 2), nullptr, 16)));
                        k += 2;
                    } else if (raw[k] == '\\' && k + 1 < raw.size() && raw[k + 1] == '\\') {
                        v.bytes.push_back('\\');
                        ++k;
                    } else {
                        v.bytes.push_back(raw[k]);
                    }
                }
                return v;
            }
            case Tok::Punct:
                if (is_punct('{') || is_punct('[') || is_punct('<')) {
                    auto save = i;
                    try {
                        aggregate(v);
                    } catch (const ParseError&) {
                        i = save;
                        v = Value{};
                        v.kind = Value::Other;
                        v.text = "aggregate";
                        if (is_punct('<') && is_punct('{', 1)) {
                            ++i;
                            skip_group();
                            expect_punct('>');
                        } else {
                            skip_group();
                        }
                    }
                    return v;
                }
                break;
            case Tok::Meta:
                v.kind = Value::Other;
                v.text = "!" + next().text;
                if (v.text == "!{") {
                    int depth = 1;
                    while (!at_end() && depth > 0) {
                        auto y = next();
                        if (y.kind == Tok::Punct && y.text == "{") ++depth;
                        if (y.kind == Tok::Punct && y.text == "}") --depth;
                    }
                }
                return v;
            default:
                break;
        }
        throw ParseError("expected value, got '" + x.text + "'");
    }

    Operand typed_operand(bool attrs = false) {
        Operand o;
        o.ty = type();
        if (attrs) skip_param_attrs(&o.attrs);
        o.v = value(o.ty);
        return o;
    }

    std::string label() {
        if (!accept_word("label")) throw ParseError("expected label");
        auto x = next();
        if (x.kind != Tok::Local) throw ParseError("expected %label");
        return x.text;
    }
};

Operand local_ptr(const std::string& name) {
    Operand o;
    o.ty = Type{Type::Ptr, 0, "ptr", {}};
    o.v.kind = Value::Local;
    o.v.name = name;
    o.v.text = "%" + name;
    return o;
}

bool is_binop(std::string_view op) {
    static const std::unordered_set<std::string_view> k{
        "add", "sub", "mul", "udiv", "sdiv", "urem", "srem", "shl", "lshr", "ashr",
        "and", "or", "xor", "fadd", "fsub", "fmul", "fdiv", "frem"};
    return k.count(op) > 0;
}

bool is_cast(std::string_view op) {
    static const std::unordered_set<std::string_view> k{
        "zext", "sext", "trunc", "fptrunc", "fpext", "fptoui", "fptosi", "uitofp", "sitofp",
        "ptrtoint", "inttoptr", "bitcast", "addrspacecast"};
    return k.count(op) > 0;
}

// Parse the tokens of one instruction (metadata attachments already removed).
Inst parse_inst(const std::vector<Tok>& toks, std::string text) {
    Inst in;
    in.text = std::move(text);
    P p{toks};
    if (p.peek().kind == Tok::Local && p.is_punct('=', 1)) {
        in.result = p.next().text;
        ++p.i;
    }
    auto opw = p.next();
    if (opw.kind != Tok::Word) throw ParseError("expected opcode");
    in.op = opw.text;
    if (in.op == "tail" || in.op == "musttail" || in.op == "notail") {
        auto c = p.next();
        in.op = c.text;
    }
    const auto& op = in.op;
    if (is_binop(op)) {
        while (p.peek().kind == Tok::Word &&
               (p.is_word("nsw") || p.is_word("nuw") || p.is_word("exact") ||
                p.is_word("disjoint") || call_prefix_words().count(p.peek().text)))
            in.flags.push_back(p.next().text);
        in.ty = p.type();
        Operand a;
        a.ty = in.ty;
        a.v = p.value(in.ty);
        p.expect_punct(',');
        Operand b;
        b.ty = in.ty;
        b.v = p.value(in.ty);
        in.ops = {a, b};
        return in;
    }
    if (op == "icmp") {
        if (p.accept_word("samesign")) in.flags.push_back("samesign");
        in.pred = p.next().text;
        auto ty = p.type();
        Operand a{ty, p.value(ty), {}};
        p.expect_punct(',');
        Operand b{ty, p.value(ty), {}};
        in.ty = ty;  // operand type; result is i1
        in.ops = {a, b};
        return in;
    }
    if (op == "fcmp") {
        while (p.peek().kind == Tok::Word && call_prefix_words().count(p.peek().text))
            in.flags.push_back(p.next().text);  // fast-math flags
        in.pred = p.next().text;
        auto ty = p.type();
        Operand a{ty, p.value(ty), {}};
        p.expect_punct(',');
        Operand b{ty, p.value(ty), {}};
        in.ty = ty;  // operand type; result is i1
        in.ops = {a, b};
        return in;
    }
    if (op == "fneg") {
        while (p.peek().kind == Tok::Word && call_prefix_words().count(p.peek().text))
            in.flags.push_back(p.next().text);
        auto a = p.typed_operand();
        in.ty = a.ty;
        in.ops = {a};
        return in;
    }
    if (op == "landingpad") {
        // landingpad T [cleanup] (catch T v | filter T v)*
        in.ty = p.type();
        while (!p.at_end()) {
            if (p.accept_word("cleanup")) {
                in.flags.push_back("cleanup");
                continue;
            }
            if (p.is_word("catch") || p.is_word("filter")) {
                auto kind = p.next().text;
                in.clauses.emplace_back(kind, p.typed_operand());
                continue;
            }
            throw ParseError("unexpected landingpad clause");
        }
        return in;
    }
    if (op == "resume") {
        auto a = p.typed_operand();
        in.ty = a.ty;
        in.ops = {a};
        return in;
    }
    if (op == "insertvalue") {
        auto agg = p.typed_operand();
        p.expect_punct(',');
        auto val = p.typed_operand();
        in.ty = agg.ty;
        in.ops = {agg, val};
        while (p.accept_punct(',')) {
            auto n = p.next();
            if (n.kind != Tok::Num) throw ParseError("expected index");
            in.indices.push_back(static_cast<unsigned>(std::stoul(n.text)));
        }
        return in;
    }
    if (op == "select") {
        while (p.peek().kind == Tok::Word && call_prefix_words().count(p.peek().text)) ++p.i;
        auto c = p.typed_operand();
        p.expect_punct(',');
        auto a = p.typed_operand();
        p.expect_punct(',');
        auto b = p.typed_operand();
        in.ty = a.ty;
        in.ops = {c, a, b};
        return in;
    }
    if (is_cast(op)) {
        while (p.is_word("nneg") || p.is_word("nuw") || p.is_word("nsw"))
            in.flags.push_back(p.next().text);
        auto a = p.typed_operand();
        if (!p.accept_word("to")) throw ParseError("expected 'to'");
        in.ty = p.type();
        in.ops = {a};
        return in;
    }
    if (op == "phi") {
        while (p.peek().kind == Tok::Word && call_prefix_words().count(p.peek().text)) ++p.i;
        in.ty = p.type();
        bool first = true;
        while (!p.at_end()) {
            if (!first) p.expect_punct(',');
            first = false;
            p.expect_punct('[');
            Operand v{in.ty, p.value(in.ty), {}};
            p.expect_punct(',');
            auto bb = p.next();
            if (bb.kind != Tok::Local) throw ParseError("expected phi block");
            p.expect_punct(']');
            in.incoming.emplace_back(v, bb.text);
        }
        return in;
    }
    if (op == "freeze") {
        auto a = p.typed_operand();
        in.ty = a.ty;
        in.ops = {a};
        return in;
    }
    if (op == "br") {
        if (p.is_word("label")) {
            in.targets.push_back(p.label());
            return in;
        }
        auto c = p.typed_operand();
        p.expect_punct(',');
        auto t = p.label();
        p.expect_punct(',');
        auto f = p.label();
        in.ops = {c};
        in.targets = {t, f};
        return in;
    }
    if (op == "ret") {
        if (p.accept_word("void")) {
            in.ty.kind = Type::Void;
            in.ty.text = "void";
            return in;
        }
        auto a = p.typed_operand();
        in.ty = a.ty;
        in.ops = {a};
        return in;
    }
    if (op == "switch") {
        auto v = p.typed_operand();
        in.ty = v.ty;
        in.ops = {v};
        p.expect_punct(',');
        in.targets.push_back(p.label());
        p.expect_punct('[');
        while (!p.is_punct(']')) {
            auto c = p.typed_operand();
            p.expect_punct(',');
            auto l = p.label();
            in.cases.emplace_back(c, l);
        }
        p.expect_punct(']');
        return in;
    }
    if (op == "unreachable") return in;
    if (op == "extractvalue") {
        auto a = p.typed_operand();
        in.ops = {a};
        while (p.accept_punct(',')) {
            auto n = p.next();
            if (n.kind != Tok::Num) throw ParseError("expected index");
            in.indices.push_back(static_cast<unsigned>(std::stoul(n.text)));
        }
        if (!a.ty.elems.empty() && in.indices.size() == 1 && in.indices[0] < a.ty.elems.size())
            in.ty = a.ty.elems[in.indices[0]];
        return in;
    }
    if (op == "alloca") {
        if (p.is_word("inalloca")) {
            in.parsed = false;
            return in;
        }
        in.ety = p.type();
        in.ty = Type{Type::Ptr, 0, "ptr", {}};
        if (p.is_punct(',') && !p.is_word("align", 1) && !p.is_word("addrspace", 1)) {
            ++p.i;
            in.ops.push_back(p.typed_operand());
        }
        in.align = p.trailing_align();
        return in;
    }
    if (op == "load") {
        if (p.is_word("atomic") || p.is_word("volatile")) {
            in.flags.push_back(p.next().text);
            in.parsed = false;
            return in;
        }
        in.ty = p.type();
        p.expect_punct(',');
        in.ops.push_back(p.typed_operand());
        in.align = p.trailing_align();
        return in;
    }
    if (op == "store") {
        if (p.is_word("atomic") || p.is_word("volatile")) {
            in.flags.push_back(p.next().text);
            in.parsed = false;
            return in;
        }
        auto v = p.typed_operand();
        p.expect_punct(',');
        auto ptr = p.typed_operand();
        in.ty = v.ty;
        in.ops = {v, ptr};
        in.align = p.trailing_align();
        return in;
    }
    if (op == "getelementptr") {
        while (p.is_word("inbounds") || p.is_word("nuw") || p.is_word("nusw")) in.flags.push_back(p.next().text);
        in.ety = p.type();
        in.ty = Type{Type::Ptr, 0, "ptr", {}};
        while (p.accept_punct(',')) {
            if (p.is_word("inrange")) {
                ++p.i;
                if (p.is_punct('(')) p.skip_group();
            }
            in.ops.push_back(p.typed_operand());
        }
        if (!in.ops.empty() && in.ops[0].ty.kind == Type::Vector) in.ty = in.ops[0].ty;
        return in;
    }
    if (op == "invoke") {
        // invoke <ty> @f(args) [fn attrs] to label %normal unwind label %lpad
        while (p.peek().kind == Tok::Word && call_prefix_words().count(p.peek().text)) ++p.i;
        p.skip_param_attrs();
        in.ty = p.type();
        if (in.ty.kind == Type::Other && !in.ty.elems.empty()) in.ty = in.ty.elems[0];
        auto cal = p.next();
        if (cal.kind == Tok::Global) in.callee = cal.text;
        else if (cal.kind == Tok::Local) in.callee_op = local_ptr(cal.text);
        else if (p.is_punct('(') && !p.is_punct('(', 1)) p.skip_group();
        p.expect_punct('(');
        bool first = true;
        while (!p.is_punct(')')) {
            if (!first) p.expect_punct(',');
            first = false;
            in.ops.push_back(p.typed_operand(true));
        }
        p.expect_punct(')');
        while (!p.at_end() && !p.is_word("to")) ++p.i;
        if (!p.accept_word("to")) throw ParseError("invoke without normal destination");
        in.targets.push_back(p.label());
        if (!p.accept_word("unwind")) throw ParseError("invoke without unwind destination");
        in.targets.push_back(p.label());
        return in;
    }
    if (op == "call") {
        while (p.peek().kind == Tok::Word && call_prefix_words().count(p.peek().text)) ++p.i;
        p.skip_param_attrs();
        in.ty = p.type();
        if (in.ty.kind == Type::Other && !in.ty.elems.empty()) in.ty = in.ty.elems[0];  // fnty
        if (p.is_word("asm")) {
            // inline assembly: asm [sideeffect] [alignstack] [inteldialect] [unwind] "text", "constraints"
            ++p.i;
            while (p.peek().kind == Tok::Word) ++p.i;
            auto t = p.next();
            if (t.kind != Tok::Str) throw ParseError("expected asm string");
            in.is_asm = true;
            in.asm_text = t.text;
            p.expect_punct(',');
            p.next();  // constraint string
        }
        auto cal = in.is_asm ? Tok{Tok::Word, "asm"} : p.next();
        if (in.is_asm) {
            // no callee symbol
        } else if (cal.kind == Tok::Global) {
            in.callee = cal.text;
        } else if (cal.kind == Tok::Local) {
            in.callee_op = local_ptr(cal.text);
        } else {
            // constant-expression callee
            if (p.is_punct('(')) p.skip_group();
        }
        p.expect_punct('(');
        bool first = true;
        while (!p.is_punct(')')) {
            if (!first) p.expect_punct(',');
            first = false;
            in.ops.push_back(p.typed_operand(true));
        }
        p.expect_punct(')');
        return in;  // trailing fn attrs / bundles ignored
    }
    // Recognised shape, not modelled: keep the opcode for the UNENCODED reason.
    in.parsed = false;
    return in;
}

// Strip trailing ", !kind !N" metadata attachments and "#N" attribute
// groups. Returns the !dbg reference if present.
std::string strip_attachments(std::vector<Tok>& toks) {
    std::string dbg;
    std::vector<Tok> out;
    out.reserve(toks.size());
    for (std::size_t i = 0; i < toks.size(); ++i) {
        auto& t = toks[i];
        if (t.kind == Tok::Attr) continue;
        if (t.kind == Tok::Punct && t.text == "," && i + 2 < toks.size() &&
            toks[i + 1].kind == Tok::Meta && toks[i + 2].kind == Tok::Meta) {
            if (toks[i + 1].text == "dbg") dbg = "!" + toks[i + 2].text;
            // skip `, !name !N` (and an inline !{...} tuple)
            std::size_t j = i + 2;
            if (toks[j].text == "{") {
                int depth = 1;
                ++j;
                while (j < toks.size() && depth > 0) {
                    if (toks[j].kind == Tok::Punct && toks[j].text == "{") ++depth;
                    if (toks[j].kind == Tok::Punct && toks[j].text == "}") --depth;
                    ++j;
                }
                i = j - 1;
            } else {
                i = j;
            }
            continue;
        }
        out.push_back(t);
    }
    toks.swap(out);
    return dbg;
}

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// Field value "key: value" inside a specialised metadata node.
std::string md_field(std::string_view line, std::string_view key) {
    std::string pat = std::string(key) + ": ";
    std::size_t pos = 0;
    while ((pos = line.find(pat, pos)) != std::string_view::npos) {
        if (pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == '(') break;
        ++pos;
    }
    if (pos == std::string_view::npos) return {};
    pos += pat.size();
    if (pos < line.size() && line[pos] == '"') {
        auto end = line.find('"', pos + 1);
        return std::string(line.substr(pos + 1, end == std::string_view::npos ? std::string_view::npos
                                                                            : end - pos - 1));
    }
    auto end = pos;
    int depth = 0;
    while (end < line.size()) {
        char c = line[end];
        if (c == '(') ++depth;
        if (c == ')') {
            if (depth == 0) break;
            --depth;
        }
        if (c == ',' && depth == 0) break;
        ++end;
    }
    return trim(line.substr(pos, end - pos));
}

int to_int(const std::string& s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

void parse_define(std::string_view header, const std::vector<std::string_view>& body, Function& fn) {
    auto toks = lex(header);
    // header: define [linkage...] [ret attrs] <type> @name(params) [...] [!dbg !N] {
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].kind == Tok::Meta && toks[i].text == "dbg" && toks[i + 1].kind == Tok::Meta)
            fn.dbg = "!" + toks[i + 1].text;
    }
    std::size_t at = 0;
    while (at < toks.size() && toks[at].kind != Tok::Global) ++at;
    if (at >= toks.size()) throw ParseError("define without @name");
    fn.name = toks[at].text;
    // return type: parse backwards is hard; re-parse forward skipping linkage words.
    {
        std::vector<Tok> pre(toks.begin() + 1, toks.begin() + static_cast<std::ptrdiff_t>(at));
        // drop leading linkage / visibility / cconv / attribute words until a type parses
        // that consumes the rest exactly.
        for (std::size_t s = 0; s < pre.size(); ++s) {
            std::vector<Tok> tail(pre.begin() + static_cast<std::ptrdiff_t>(s), pre.end());
            P p{tail};
            try {
                p.skip_param_attrs(&fn.ret_attrs);
                auto ty = p.type();
                if (p.at_end()) {
                    fn.ret = ty;
                    break;
                }
            } catch (const ParseError&) {
            }
            fn.ret_attrs.clear();
        }
    }
    {
        std::vector<Tok> rest(toks.begin() + static_cast<std::ptrdiff_t>(at) + 1, toks.end());
        P p{rest};
        p.expect_punct('(');
        bool first = true;
        while (!p.is_punct(')')) {
            if (!first) p.expect_punct(',');
            first = false;
            if (p.is_word("...")) {
                ++p.i;
                fn.parse_error = "variadic function";
                continue;
            }
            Param prm;
            prm.ty = p.type();
            p.skip_param_attrs(&prm.attrs);
            if (p.peek().kind == Tok::Local) prm.name = p.next().text;
            fn.params.push_back(prm);
        }
    }
    // body
    Block* cur = nullptr;
    std::string pending;
    int depth = 0;
    auto flush = [&](const std::string& line) {
        auto toks2 = lex(line);
        if (toks2.empty()) return;
        auto dbg = strip_attachments(toks2);
        if (!cur) {
            fn.blocks.push_back(Block{"__entry", {}});
            cur = &fn.blocks.back();
        }
        Inst in;
        try {
            in = parse_inst(toks2, trim(line));
        } catch (const ParseError& e) {
            if (fn.parse_error.empty()) fn.parse_error = std::string(e.what()) + ": " + trim(line);
            in.parsed = false;
            in.text = trim(line);
            auto sp = in.text.find(' ');
            auto eq = in.text.find(" = ");
            in.op = eq != std::string::npos ? in.text.substr(eq + 3, in.text.find(' ', eq + 3) - eq - 3)
                                             : in.text.substr(0, sp);
        }
        in.dbg = dbg;
        cur->insts.push_back(std::move(in));
    };
    std::string lpad;  // a landingpad whose clauses continue on the next lines
    for (auto raw : body) {
        auto line = trim(raw);
        if (line.empty() || line[0] == ';') continue;
        if (!lpad.empty()) {
            if (line.starts_with("catch ") || line.starts_with("filter ") || line.starts_with("cleanup")) {
                lpad += " " + line;
                continue;
            }
            flush(lpad);
            lpad.clear();
        }
        if (depth == 0) {
            // label?
            std::string lab;
            auto semi = line.find(';');
            auto code = trim(semi == std::string::npos ? std::string_view(line)
                                                       : std::string_view(line).substr(0, semi));
            if (!code.empty() && code.back() == ':' && code.find(' ') == std::string::npos) {
                lab = code.substr(0, code.size() - 1);
                if (lab.size() >= 2 && lab.front() == '"' && lab.back() == '"')
                    lab = lab.substr(1, lab.size() - 2);
                fn.blocks.push_back(Block{lab, {}});
                cur = &fn.blocks.back();
                continue;
            }
        }
        for (char c : line) {
            if (c == '[') ++depth;
            if (c == ']') --depth;
        }
        pending += pending.empty() ? line : " " + line;
        // `invoke ...` continues on the next line with "to label ... unwind label ..."
        bool open_invoke = (pending.starts_with("invoke ") || pending.find(" = invoke ") != std::string::npos) &&
                           pending.find(" unwind label ") == std::string::npos &&
                           pending.find(" unwind to caller") == std::string::npos;
        if (depth <= 0 && open_invoke) continue;
        if (depth <= 0) {
            depth = 0;
            if (pending.find(" = landingpad ") != std::string::npos || pending.starts_with("landingpad "))
                lpad = pending;  // clauses follow on the next lines
            else
                flush(pending);
            pending.clear();
        }
    }
    if (!lpad.empty()) flush(lpad);
    if (!pending.empty()) flush(pending);
}

}  // namespace

const Function* Module::find(std::string_view name) const {
    for (auto& f : functions)
        if (f.name == name) return &f;
    return nullptr;
}

const Global* Module::find_global(std::string_view name) const {
    for (auto& g : globals)
        if (g.name == name) return &g;
    return nullptr;
}

namespace {

// "@name = [linkage...] (global|constant) T [init][, align N][, ...]"
bool parse_global(std::string_view line, Global& g) {
    auto toks = lex(line);
    strip_attachments(toks);
    if (toks.size() < 4 || toks[0].kind != Tok::Global || toks[1].kind != Tok::Punct || toks[1].text != "=")
        return false;
    g.name = toks[0].text;
    g.text = std::string(line);
    std::vector<Tok> rest(toks.begin() + 2, toks.end());
    P p{rest};
    bool found = false;
    while (!p.at_end()) {
        if (p.is_word("alias") || p.is_word("ifunc")) return false;
        if (p.is_word("global") || p.is_word("constant")) {
            g.is_const = p.next().text == "constant";
            found = true;
            break;
        }
        auto w = p.next();
        if (w.kind == Tok::Word && w.text == "external") g.external = true;
        if (w.kind == Tok::Word && w.text == "extern_weak") g.external = true;
        if (w.kind == Tok::Word && w.text == "thread_local") {
            g.thread_local_ = true;
            if (p.is_punct('(')) p.skip_group();
        }
        if (w.kind == Tok::Word && w.text == "addrspace" && p.is_punct('(')) p.skip_group();
    }
    if (!found) return false;
    try {
        g.ty = p.type();
        if (!g.external && !p.at_end() && !p.is_punct(',')) {
            Operand o;
            o.ty = g.ty;
            o.v = p.value(g.ty);
            g.init.push_back(std::move(o));
        } else if (!g.external) {
            g.external = true;
        }
        g.align = p.trailing_align();
    } catch (const ParseError&) {
        g.init.clear();
        g.external = true;  // unparsed initializer: contents unknown
    }
    return true;
}

}  // namespace

Type parse_type(std::string_view text) {
    auto toks = lex(text);
    P p{toks};
    return p.type();
}

Module parse_module(std::string_view text) {
    Module m;
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    std::map<std::string, std::string> files;  // "!1" -> filename
    std::vector<std::pair<std::string, std::string>> subs_raw;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto line = lines[i];
        if (line.starts_with("define ")) {
            Function fn;
            std::vector<std::string_view> body;
            std::size_t j = i + 1;
            // header may end with '{' on the same line (opt -S always does)
            while (j < lines.size() && lines[j] != "}") body.push_back(lines[j++]);
            auto header = line;
            if (auto brace = header.rfind('{'); brace != std::string_view::npos)
                header = header.substr(0, brace);
            try {
                parse_define(header, body, fn);
            } catch (const ParseError& e) {
                fn.parse_error = std::string("header: ") + e.what();
            }
            m.functions.push_back(std::move(fn));
            i = j;
            continue;
        }
        if (line.starts_with("@")) {
            Global g;
            if (parse_global(line, g)) m.globals.push_back(std::move(g));
            continue;
        }
        if (line.starts_with("%") && line.find(" = type ") != std::string_view::npos) {
            auto toks = lex(line);
            if (toks.size() >= 4 && toks[0].kind == Tok::Local) {
                std::vector<Tok> rest(toks.begin() + 3, toks.end());
                Type body;
                if (rest.size() == 1 && rest[0].kind == Tok::Word && rest[0].text == "opaque") {
                    body.kind = Type::Other;
                    body.text = "opaque";
                } else {
                    try {
                        P p{rest};
                        body = p.type();
                    } catch (const ParseError&) {
                        body = Type{};
                        body.text = "unparsed";
                    }
                }
                m.types[toks[0].text] = body;
            }
            continue;
        }
        if (line.starts_with("target datalayout = \"")) {
            auto a = line.find('"');
            auto b = line.rfind('"');
            if (a != std::string_view::npos && b > a) m.datalayout = std::string(line.substr(a + 1, b - a - 1));
            continue;
        }
        if (line.starts_with("declare ")) {
            auto toks = lex(line);
            for (auto& t : toks)
                if (t.kind == Tok::Global) {
                    m.declarations.push_back(t.text);
                    break;
                }
            continue;
        }
        if (line.starts_with("!")) {
            auto eq = line.find(" = ");
            if (eq == std::string_view::npos) continue;
            std::string ref(line.substr(0, eq));
            auto rhs = line.substr(eq + 3);
            if (rhs.find("!DILocation(") != std::string_view::npos) {
                DILoc l;
                l.line = to_int(md_field(rhs, "line"));
                l.col = to_int(md_field(rhs, "column"));
                m.locs[ref] = l;
            } else if (rhs.find("!DISubprogram(") != std::string_view::npos) {
                subs_raw.emplace_back(ref, std::string(rhs));
            } else if (rhs.find("!DIFile(") != std::string_view::npos) {
                files[ref] = md_field(rhs, "filename");
            }
        }
    }
    for (auto& [ref, rhs] : subs_raw) {
        DISub s;
        s.name = md_field(rhs, "name");
        s.linkage = md_field(rhs, "linkageName");
        s.line = to_int(md_field(rhs, "line"));
        auto f = md_field(rhs, "file");
        if (auto it = files.find(f); it != files.end()) s.file = it->second;
        s.artificial = md_field(rhs, "flags").find("DIFlagArtificial") != std::string::npos;
        m.subprograms[ref] = s;
    }
    return m;
}

}  // namespace prism::pir::ir
