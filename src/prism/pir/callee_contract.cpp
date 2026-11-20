// Assume-guarantee stubs for callees without a body (callee_contract.hpp, docs/HARNESS_PHASE2.md).

#include "callee_contract.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace prism::pir {

namespace {
std::string trim_ws(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r");
    auto b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

std::map<std::string, CalleeContract> load_callee_contracts(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = "cannot read " + file.string();
        return {};
    }
    std::map<std::string, CalleeContract> out;
    std::string line;
    for (int no = 1; std::getline(in, line); ++no) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim_ws(line).empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        for (std::string::size_type s = 0;;) {
            const auto p = line.find('\t', s);
            cols.push_back(line.substr(s, p == std::string::npos ? std::string::npos : p - s));
            if (p == std::string::npos) break;
            s = p + 1;
        }
        if (cols.size() == 7 && trim_ws(cols[0]) == "function") continue;  // header
        if (cols.size() != 7) {
            error = "line " + std::to_string(no) + ": expected 7 tab-separated columns (function, signature, requires, "
                    "assigns, ensures, status, source), got " + std::to_string(cols.size());
            return {};
        }
        CalleeContract c;
        c.function = trim_ws(cols[0]);
        c.signature = trim_ws(cols[1]);
        c.requires_ = trim_ws(cols[2]);
        c.assigns = trim_ws(cols[3]);
        c.ensures = trim_ws(cols[4]);
        c.status = trim_ws(cols[5]);
        c.source = trim_ws(cols[6]);
        if (c.function.empty()) {
            error = "line " + std::to_string(no) + ": empty function name";
            return {};
        }
        if (c.status.empty()) {
            error = "line " + std::to_string(no) + ": the status column is required (UNVALIDATED, REVIEWED or VALIDATED)";
            return {};
        }
        if (c.source.empty()) {
            error = "line " + std::to_string(no) + ": the source column is required (where the contract comes from)";
            return {};
        }
        if (out.count(c.function)) {
            error = "line " + std::to_string(no) + ": a second contract for " + c.function;
            return {};
        }
        out[c.function] = std::move(c);
    }
    return out;
}

namespace pirmem {

namespace {

struct Reject {
    std::string why;
};

// ---- signature ------------------------------------------------------------------------------------------------

struct SigT {
    enum K { Ptr, S, U, Bool, Void } k = Void;
    unsigned bits = 0;
};

struct Sig {
    std::vector<std::string> names;
    std::vector<SigT> params;
    SigT ret;
};

SigT parse_sigt(const std::string& s0) {
    const std::string s = trim_ws(s0);
    SigT t;
    if (s == "ptr") {
        t.k = SigT::Ptr;
        t.bits = 64;
    } else if (s == "bool") {
        t.k = SigT::Bool;
        t.bits = 1;
    } else if (s == "void") {
        t.k = SigT::Void;
    } else if (s.size() >= 2 && (s[0] == 'u' || s[0] == 's') &&
               std::all_of(s.begin() + 1, s.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        t.k = s[0] == 'u' ? SigT::U : SigT::S;
        t.bits = static_cast<unsigned>(std::stoul(s.substr(1)));
        if (t.bits != 8 && t.bits != 16 && t.bits != 32 && t.bits != 64) throw Reject{"type " + s + ": width not 8/16/32/64"};
    } else {
        throw Reject{"unknown type '" + s + "' in the signature"};
    }
    return t;
}

Sig parse_sig(const std::string& text) {
    Sig sig;
    std::string params = text, ret;
    if (auto p = text.find("->"); p != std::string::npos) {
        params = text.substr(0, p);
        ret = text.substr(p + 2);
    }
    if (!trim_ws(ret).empty()) sig.ret = parse_sigt(ret);
    std::set<std::string> seen;
    for (std::string::size_type s = 0; s < params.size() && !trim_ws(params.substr(s)).empty();) {
        auto c = params.find(',', s);
        std::string item = params.substr(s, c == std::string::npos ? std::string::npos : c - s);
        auto col = item.find(':');
        if (col == std::string::npos) throw Reject{"parameter '" + trim_ws(item) + "' has no type (name:type)"};
        std::string name = trim_ws(item.substr(0, col));
        if (name.empty() || !seen.insert(name).second) throw Reject{"empty or repeated parameter name '" + name + "'"};
        SigT t = parse_sigt(item.substr(col + 1));
        if (t.k == SigT::Void) throw Reject{"a parameter cannot be void"};
        sig.names.push_back(name);
        sig.params.push_back(t);
        if (c == std::string::npos) break;
        s = c + 1;
    }
    return sig;
}

// ---- expressions ----------------------------------------------------------------------------------------------

struct Node;
using NP = std::shared_ptr<Node>;
struct Node {
    enum K { Int, Var, Result, Null, True, False, Not, Neg, Bin, Valid } k = Int;
    uint64_t i = 0;
    int var = -1;
    std::string op;   // Bin; Valid: "bytes" (assigns) | "valid" | "valid_read"
    NP a, b;
};

struct Lexer {
    struct Tok {
        enum K { End, Int, Id, Bs, Op } k = End;
        std::string s;
        uint64_t v = 0;
    };
    std::string src;
    std::size_t p = 0;
    Tok cur;
    explicit Lexer(std::string s) : src(std::move(s)) { next(); }
    void next() {
        while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
        cur = Tok{};
        if (p >= src.size()) return;
        const char c = src[p];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t q = p;
            uint64_t v = 0;
            if (c == '0' && p + 1 < src.size() && (src[p + 1] == 'x' || src[p + 1] == 'X')) {
                q += 2;
                if (q >= src.size() || !std::isxdigit(static_cast<unsigned char>(src[q]))) throw Reject{"bad hex literal"};
                while (q < src.size() && std::isxdigit(static_cast<unsigned char>(src[q]))) {
                    const char d = src[q++];
                    const unsigned dv = std::isdigit(static_cast<unsigned char>(d)) ? static_cast<unsigned>(d - '0')
                                                                                      : static_cast<unsigned>(std::tolower(d) - 'a' + 10);
                    if (v >> 60) throw Reject{"integer literal too large"};
                    v = v * 16 + dv;
                }
            } else {
                while (q < src.size() && std::isdigit(static_cast<unsigned char>(src[q]))) {
                    const unsigned dv = static_cast<unsigned>(src[q++] - '0');
                    if (v > (UINT64_MAX - dv) / 10) throw Reject{"integer literal too large"};
                    v = v * 10 + dv;
                }
            }
            while (q < src.size() && (src[q] == 'u' || src[q] == 'U' || src[q] == 'l' || src[q] == 'L')) ++q;  // C suffixes
            cur.k = Tok::Int;
            cur.v = v;
            p = q;
            return;
        }
        if (c == '\\' || std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            std::size_t q = p + 1;
            while (q < src.size() && (std::isalnum(static_cast<unsigned char>(src[q])) || src[q] == '_' || src[q] == '$')) ++q;
            cur.k = c == '\\' ? Tok::Bs : Tok::Id;
            cur.s = src.substr(p, q - p);
            p = q;
            return;
        }
        static const char* two[] = {"||", "&&", "==", "!=", "<=", ">="};
        for (auto* t : two)
            if (src.compare(p, 2, t) == 0) {
                cur.k = Tok::Op;
                cur.s = t;
                p += 2;
                return;
            }
        if (std::string("<>+-*!(),").find(c) != std::string::npos) {
            cur.k = Tok::Op;
            cur.s = std::string(1, c);
            ++p;
            return;
        }
        throw Reject{std::string("unexpected character '") + c + "'"};
    }
};

class Parser {
public:
    Parser(const std::string& text, const Sig& sig) : lx_(text), sig_(sig) {}
    NP parse_all() {
        NP n = or_();
        if (lx_.cur.k != Lexer::Tok::End) throw Reject{"unexpected '" + lx_.cur.s + "' after the expression"};
        return n;
    }

private:
    Lexer lx_;
    const Sig& sig_;
    bool is(const char* s) const { return lx_.cur.k == Lexer::Tok::Op && lx_.cur.s == s; }
    void expect(const char* s) {
        if (!is(s)) throw Reject{std::string("expected '") + s + "'"};
        lx_.next();
    }
    NP mk(Node::K k) {
        auto n = std::make_shared<Node>();
        n->k = k;
        return n;
    }
    NP bin(const std::string& op, NP a, NP b) {
        auto n = mk(Node::Bin);
        n->op = op;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }
    NP or_() {
        NP l = and_();
        while (is("||")) {
            lx_.next();
            l = bin("||", l, and_());
        }
        return l;
    }
    NP and_() {
        NP l = eq();
        while (is("&&")) {
            lx_.next();
            l = bin("&&", l, eq());
        }
        return l;
    }
    NP eq() {
        NP l = rel();
        while (is("==") || is("!=")) {
            std::string o = lx_.cur.s;
            lx_.next();
            l = bin(o, l, rel());
        }
        return l;
    }
    NP rel() {
        NP l = add();
        while (is("<") || is("<=") || is(">") || is(">=")) {
            std::string o = lx_.cur.s;
            lx_.next();
            l = bin(o, l, add());
        }
        return l;
    }
    NP add() {
        NP l = mul();
        while (is("+") || is("-")) {
            std::string o = lx_.cur.s;
            lx_.next();
            l = bin(o, l, mul());
        }
        return l;
    }
    NP mul() {
        NP l = un();
        while (is("*")) {
            lx_.next();
            l = bin("*", l, un());
        }
        return l;
    }
    NP un() {
        if (is("!")) {
            lx_.next();
            auto n = mk(Node::Not);
            n->a = un();
            return n;
        }
        if (is("-")) {
            lx_.next();
            auto n = mk(Node::Neg);
            n->a = un();
            return n;
        }
        return prim();
    }
    NP prim() {
        using T = Lexer::Tok;
        if (lx_.cur.k == T::Int) {
            auto n = mk(Node::Int);
            n->i = lx_.cur.v;
            lx_.next();
            return n;
        }
        if (is("(")) {
            lx_.next();
            NP n = or_();
            expect(")");
            return n;
        }
        if (lx_.cur.k == T::Id) {
            for (std::size_t i = 0; i < sig_.names.size(); ++i)
                if (sig_.names[i] == lx_.cur.s) {
                    auto n = mk(Node::Var);
                    n->var = static_cast<int>(i);
                    lx_.next();
                    return n;
                }
            throw Reject{"unknown name '" + lx_.cur.s + "' (not a parameter of the signature)"};
        }
        if (lx_.cur.k == T::Bs) {
            const std::string w = lx_.cur.s;
            lx_.next();
            if (w == "\\result") return mk(Node::Result);
            if (w == "\\null") return mk(Node::Null);
            if (w == "\\true") return mk(Node::True);
            if (w == "\\false") return mk(Node::False);
            if (w == "\\valid_bytes" || w == "\\valid_read_bytes" || w == "\\bytes") {
                auto n = mk(Node::Valid);
                n->op = w == "\\bytes" ? "bytes" : w == "\\valid_bytes" ? "valid" : "valid_read";
                expect("(");
                n->a = or_();
                expect(",");
                n->b = or_();
                expect(")");
                return n;
            }
            throw Reject{"unsupported ACSL construct " + w};
        }
        throw Reject{"expected an expression"};
    }
};

std::vector<std::string> split_clauses(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ';' && depth == 0) {
            if (!trim_ws(cur).empty()) r.push_back(trim_ws(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim_ws(cur).empty()) r.push_back(trim_ws(cur));
    return r;
}

void conjuncts(const NP& n, std::vector<NP>& out) {
    if (n->k == Node::Bin && n->op == "&&") {
        conjuncts(n->a, out);
        conjuncts(n->b, out);
    } else {
        out.push_back(n);
    }
}

bool mentions_valid(const NP& n) {
    if (!n) return false;
    return n->k == Node::Valid || mentions_valid(n->a) || mentions_valid(n->b);
}

// ---- evaluation -----------------------------------------------------------------------------------------------

struct Val {
    enum D { S, U, P, B } d = S;  // S: signed value (sign-/zero-extended to 64 bits, `bound` bits), U: u64, P: pointer, B: i1
    Arg a;
    int bound = 64;
    std::optional<int64_t> k;
};

int bit_length(uint64_t v) {
    int n = 0;
    while (v) {
        ++n;
        v >>= 1;
    }
    return n;
}

class Eval {
public:
    // dry: only types are computed (no statement is emitted): a Reject can then never leave half a stub behind
    Eval(TrApi& t, MemTr& mt, const Sig& sig, const StubCall& call, int& cur, bool dry)
        : t_(t), sig_(sig), cur_(cur), dry_(dry) {
        (void)mt;
        for (std::size_t i = 0; i < sig_.params.size(); ++i) params_.push_back(leaf(call.args.at(i), sig_.params[i]));
    }
    std::optional<Val> result;

    Val leaf(Arg a, const SigT& ty) {
        Val v;
        v.a = a;
        switch (ty.k) {
            case SigT::Ptr:
                v.d = Val::P;
                return v;
            case SigT::Bool:
                v.d = Val::S;
                v.a = ext(a, false);
                v.bound = 1;
                return v;
            case SigT::U:
                if (ty.bits == 64) {
                    v.d = Val::U;
                    return v;
                }
                v.d = Val::S;
                v.a = ext(a, false);
                v.bound = static_cast<int>(ty.bits);
                return v;
            case SigT::S:
                v.d = Val::S;
                v.a = ty.bits == 64 ? a : ext(a, true);
                v.bound = static_cast<int>(ty.bits);
                return v;
            default: throw Reject{"void value"};
        }
    }

    Val eval(const NP& n) {
        switch (n->k) {
            case Node::Int: {
                if (n->i > static_cast<uint64_t>(INT64_MAX)) throw Reject{"integer literal beyond 2^63"};
                Val v;
                v.d = Val::S;
                v.a = Arg::c(64, n->i);
                v.bound = std::max(1, bit_length(n->i));
                v.k = static_cast<int64_t>(n->i);
                return v;
            }
            case Node::Var: return params_.at(static_cast<std::size_t>(n->var));
            case Node::Result:
                if (!result) throw Reject{"\\result used where the callee has none (or before it is defined)"};
                return *result;
            case Node::Null: {
                Val v;
                v.d = Val::P;
                v.a = Arg::c(ptr_w(), 0);
                return v;
            }
            case Node::True:
            case Node::False: {
                Val v;
                v.d = Val::B;
                v.a = Arg::c(1, n->k == Node::True ? 1 : 0);
                return v;
            }
            case Node::Not: {
                Val x = eval(n->a);
                need_bool(x, "!");
                return boolean(emit(Op::Xor, 1, {x.a, Arg::c(1, 1)}, "c"));
            }
            case Node::Neg: {
                Val x = eval(n->a);
                need_signed(x, "unary -");
                Val v;
                v.d = Val::S;
                v.a = emit(Op::Sub, 64, {Arg::c(64, 0), x.a}, "k");
                v.bound = x.bound + 1;
                if (x.k) v.k = -*x.k;
                return v;
            }
            case Node::Valid: throw Reject{"\\valid_bytes / \\bytes only as a whole clause (not inside an expression)"};
            case Node::Bin: return binary(*n);
        }
        throw Reject{"expression"};
    }

private:
    TrApi& t_;
    const Sig& sig_;
    int& cur_;
    bool dry_;
    std::vector<Val> params_;

    Arg emit(Op op, unsigned w, std::vector<Arg> args, const char* base) {
        if (dry_) return Arg::c(w, 0);
        return t_.assign(cur_, op, w, std::move(args), base);
    }
    Arg ext(Arg a, bool sgn) {
        if (a.width == 64) return a;
        return emit(sgn ? Op::SExt : Op::ZExt, 64, {a}, "k");
    }
    static Val boolean(Arg a) {
        Val v;
        v.d = Val::B;
        v.a = a;
        return v;
    }
    static void need_bool(const Val& v, const char* what) {
        if (v.d != Val::B) throw Reject{std::string("operand of ") + what + " is not a boolean expression"};
    }
    static void need_signed(const Val& v, const char* what) {
        if (v.d != Val::S || v.bound > 62) throw Reject{std::string("operand of ") + what + " is not a small signed integer (64-bit values are only compared)"};
    }

    Val binary(const Node& n) {
        const std::string& op = n.op;
        Val x = eval(n.a), y = eval(n.b);
        if (op == "&&" || op == "||") {
            need_bool(x, op.c_str());
            need_bool(y, op.c_str());
            return boolean(emit(op == "&&" ? Op::And : Op::Or, 1, {x.a, y.a}, "c"));
        }
        if (op == "+" || op == "-" || op == "*") {
            need_signed(x, op.c_str());
            need_signed(y, op.c_str());
            Val v;
            v.d = Val::S;
            v.bound = op == "*" ? x.bound + y.bound : std::max(x.bound, y.bound) + 1;
            if (v.bound > 62) throw Reject{"arithmetic could overflow 64 bits (bound " + std::to_string(v.bound) + ")"};
            v.a = emit(op == "+" ? Op::Add : op == "-" ? Op::Sub : Op::Mul, 64, {x.a, y.a}, "k");
            if (x.k && y.k) v.k = op == "+" ? *x.k + *y.k : op == "-" ? *x.k - *y.k : *x.k * *y.k;
            return v;
        }
        // comparisons
        const bool is_eq = op == "==" || op == "!=";
        if (x.d == Val::P || y.d == Val::P) {
            if (x.d != Val::P || y.d != Val::P || !is_eq) throw Reject{"pointers are only compared with == and !="};
            return boolean(emit(op == "==" ? Op::Eq : Op::Ne, 1, {x.a, y.a}, "c"));
        }
        if (x.d == Val::B || y.d == Val::B) {
            if (x.d != Val::B || y.d != Val::B || !is_eq) throw Reject{"booleans are only compared with == and !="};
            return boolean(emit(op == "==" ? Op::Eq : Op::Ne, 1, {x.a, y.a}, "c"));
        }
        bool uns;
        if (x.d == Val::U && y.d == Val::U) uns = true;
        else if (x.d == Val::S && y.d == Val::S) uns = false;
        else {
            // u64 against a signed operand: only a non-negative constant (then unsigned)
            const Val& s = x.d == Val::S ? x : y;
            if (!s.k || *s.k < 0) throw Reject{"a u64 value is compared with a signed expression that is not a non-negative constant"};
            uns = true;
        }
        Op o;
        if (op == "==") o = Op::Eq;
        else if (op == "!=") o = Op::Ne;
        else if (op == "<") o = uns ? Op::Ult : Op::Slt;
        else if (op == "<=") o = uns ? Op::Ule : Op::Sle;
        else if (op == ">") o = uns ? Op::Ugt : Op::Sgt;
        else o = uns ? Op::Uge : Op::Sge;
        return boolean(emit(o, 1, {x.a, y.a}, "c"));
    }
};

std::string show(const std::string& s) { return s.empty() ? "(none)" : s; }

// parse + signature compatibility with the IR call
Sig sig_for_call(const CalleeContract& c, const StubCall* call) {
    Sig sig = parse_sig(c.signature);
    if (!call) return sig;
    if (call->args.size() != sig.params.size())
        throw Reject{"the call passes " + std::to_string(call->args.size()) + " argument(s), the signature lists " + std::to_string(sig.params.size())};
    for (std::size_t i = 0; i < sig.params.size(); ++i) {
        const auto& ty = call->arg_types.at(i);
        const auto& st = sig.params[i];
        const bool ptr_ok = st.k == SigT::Ptr && ty.kind == ir::Type::Ptr && ty.text == "ptr";
        const bool int_ok = st.k != SigT::Ptr && ty.kind == ir::Type::Int &&
                            (st.k == SigT::Bool ? (ty.bits == 1 || ty.bits == 8) : ty.bits == st.bits);
        if (!ptr_ok && !int_ok)
            throw Reject{"parameter " + sig.names[i] + " (" + (st.k == SigT::Ptr ? "ptr" : "integer") + ") does not match the call's argument type " + ty.text};
        if (call->args.at(i).width == 0) throw Reject{"argument value"};
    }
    const auto& rt = call->ret;
    const auto& sr = sig.ret;
    const bool void_ok = sr.k == SigT::Void && rt.kind == ir::Type::Void;
    const bool ptr_ok = sr.k == SigT::Ptr && rt.kind == ir::Type::Ptr && rt.text == "ptr";
    const bool int_ok = (sr.k == SigT::S || sr.k == SigT::U || sr.k == SigT::Bool) && rt.kind == ir::Type::Int &&
                        (sr.k == SigT::Bool ? (rt.bits == 1 || rt.bits == 8) : rt.bits == sr.bits);
    if (!void_ok && !ptr_ok && !int_ok) throw Reject{"the result type " + rt.text + " does not match the signature's result"};
    return sig;
}

}  // namespace

std::string callee_contract_problem(const CalleeContract& c) {
    try {
        Sig sig = sig_for_call(c, nullptr);
        for (const auto& cl : split_clauses(c.requires_)) Parser(cl, sig).parse_all();
        for (const auto& cl : split_clauses(c.assigns)) Parser(cl == "\\nothing" || cl == "\\anything" ? "\\true" : cl, sig).parse_all();
        for (const auto& cl : split_clauses(c.ensures)) Parser(cl, sig).parse_all();
    } catch (const Reject& r) {
        return r.why;
    }
    return {};
}

StubOutcome apply_callee_contract(TrApi& t, MemTr& mt, const CalleeContract& c, const StubCall& call, int& cur) {
    StubOutcome out;
    try {
        if (ptr_w() != 64) throw Reject{"callee contracts need the 64-bit pointer model"};
        Sig sig = sig_for_call(c, &call);
        // parse everything first: nothing is emitted for a contract that cannot be applied
        std::vector<NP> req, ens;
        std::vector<NP> asg;
        bool asg_nothing = false, asg_anything = false;
        for (const auto& cl : split_clauses(c.requires_)) {
            std::vector<NP> cj;
            conjuncts(Parser(cl, sig).parse_all(), cj);
            for (auto& n : cj) req.push_back(n);
        }
        for (const auto& cl : split_clauses(c.assigns)) {
            if (cl == "\\nothing") asg_nothing = true;
            else if (cl == "\\anything") asg_anything = true;
            else if (cl == "\\result") continue;
            else {
                NP n = Parser(cl, sig).parse_all();
                if (n->k != Node::Valid || n->op != "bytes") throw Reject{"assigns: only \\nothing, \\anything, \\result and \\bytes(p, n) are supported, got '" + cl + "'"};
                asg.push_back(n);
            }
        }
        if (asg_nothing && (asg_anything || !asg.empty())) throw Reject{"assigns: \\nothing together with other locations"};
        for (const auto& cl : split_clauses(c.ensures)) {
            std::vector<NP> cj;
            conjuncts(Parser(cl, sig).parse_all(), cj);
            for (auto& n : cj) ens.push_back(n);
        }
        // result-pointer ensures: at most one validity conjunct, optionally under `\result == \null || ...`
        auto is_result = [](const NP& n) { return n && n->k == Node::Result; };
        auto is_null_eq = [&](const NP& n) {
            return n->k == Node::Bin && n->op == "==" && ((is_result(n->a) && n->b->k == Node::Null) || (is_result(n->b) && n->a->k == Node::Null));
        };
        auto is_result_valid = [&](const NP& n) { return n->k == Node::Valid && n->op == "valid" && is_result(n->a); };
        NP valid_conj;
        bool nullable = false;
        std::vector<NP> plain_ens;
        for (auto& n : ens) {
            if (is_result_valid(n)) {
                if (valid_conj) throw Reject{"ensures: more than one \\valid_bytes(\\result, n)"};
                valid_conj = n;
            } else if (n->k == Node::Bin && n->op == "||" && ((is_null_eq(n->a) && is_result_valid(n->b)) || (is_null_eq(n->b) && is_result_valid(n->a)))) {
                if (valid_conj) throw Reject{"ensures: more than one \\valid_bytes(\\result, n)"};
                valid_conj = is_result_valid(n->a) ? n->a : n->b;
                nullable = true;
            } else {
                if (mentions_valid(n)) throw Reject{"ensures: \\valid_bytes is supported for \\result only, as a whole clause or as `\\result == \\null || \\valid_bytes(\\result, n)`"};
                plain_ens.push_back(n);
            }
        }
        if (valid_conj && sig.ret.k != SigT::Ptr) throw Reject{"ensures: \\valid_bytes(\\result, n) on a callee whose result is not a pointer"};
        for (auto& n : req)
            if (mentions_valid(n) && n->k != Node::Valid) throw Reject{"requires: \\valid_bytes only as a whole clause"};
        for (auto& n : req)
            if (n->k == Node::Valid && n->op == "bytes") throw Reject{"requires: \\bytes is an assigns location"};
        for (auto& n : req)
            if (n->k == Node::Valid && !(n->op == "valid" || n->op == "valid_read")) throw Reject{"requires: bad validity clause"};

        // Dry pass: type-check every clause without emitting a statement, so a Reject never leaves half a stub behind.
        {
            Eval dry(t, mt, sig, call, cur, true);
            Val res;
            if (sig.ret.k == SigT::Ptr) {
                res.d = Val::P;
                res.a = Arg::c(ptr_w(), 0);
            } else if (sig.ret.k != SigT::Void) {
                res = dry.leaf(Arg::c(call.ret.bits, 0), sig.ret);
            }
            if (sig.ret.k != SigT::Void) dry.result = res;
            auto size_ok = [&](const NP& n, const char* where) {
                Val p = dry.eval(n->a), s = dry.eval(n->b);
                if (p.d != Val::P) throw Reject{std::string(where) + ": the first argument of a validity or assigns clause is not a pointer"};
                if (!(s.d == Val::S || s.d == Val::U) || (s.k && *s.k < 0)) throw Reject{std::string(where) + ": the size is not a non-negative integer"};
            };
            for (auto& n : req) {
                if (n->k == Node::Valid) size_ok(n, "requires");
                else if (dry.eval(n).d != Val::B) throw Reject{"requires: '" + c.requires_ + "' is not a boolean expression"};
            }
            for (auto& n : asg) size_ok(n, "assigns");
            if (valid_conj) size_ok(valid_conj, "ensures");
            for (auto& n : plain_ens)
                if (dry.eval(n).d != Val::B) throw Reject{"ensures: a clause is not a boolean expression"};
        }
        Eval ev(t, mt, sig, call, cur, false);
        std::vector<Val> req_vals;
        std::vector<std::pair<Val, Val>> valid_vals;  // (pointer, size)
        for (auto& n : req) {
            if (n->k == Node::Valid) {
                Val p = ev.eval(n->a), s = ev.eval(n->b);
                if (p.d != Val::P) throw Reject{"requires: the first argument of \\valid_bytes is not a pointer"};
                if (!(s.d == Val::S || s.d == Val::U) || (s.k && *s.k < 0)) throw Reject{"requires: the size of \\valid_bytes is not an integer"};
                valid_vals.emplace_back(p, s);
            } else {
                Val v = ev.eval(n);
                if (v.d != Val::B) throw Reject{"requires: '" + c.requires_ + "' is not a boolean expression"};
                req_vals.push_back(v);
            }
        }
        std::vector<std::pair<Val, Val>> asg_vals;
        for (auto& n : asg) {
            Val p = ev.eval(n->a), s = ev.eval(n->b);
            if (p.d != Val::P) throw Reject{"assigns: the first argument of \\bytes is not a pointer"};
            if (!(s.d == Val::S || s.d == Val::U) || (s.k && *s.k < 0)) throw Reject{"assigns: the size of \\bytes is not an integer"};
            asg_vals.emplace_back(p, s);
        }
        // ---- emission starts: requires -------------------------------------------------------------------------------
        const std::string what = "call of @" + call.callee + " violates its contract (" + c.source + ", " + c.status + ")";
        std::size_t vi = 0, bi = 0;
        for (auto& n : req) {
            if (n->k == Node::Valid) {
                auto& [p, s] = valid_vals.at(vi++);
                mt.contract_valid(cur, p.a, s.a, n->op == "valid", what + ": requires \\" + (n->op == "valid" ? "valid_bytes" : "valid_read_bytes") + " of the argument", call.line);
            } else {
                Val v = req_vals.at(bi++);
                Arg viol = t.assign(cur, Op::Xor, 1, {v.a, Arg::c(1, 1)}, "c");
                t.check(cur, viol, "callee-requires", "FUNC-CONTRACT", what + ": a requires clause can be false (" + c.requires_ + ")", call.line);
            }
        }
        // ---- assigns -------------------------------------------------------------------------------------------------
        std::string assigns_text;
        if (asg_anything) {
            mt.havoc_all(cur);
            assigns_text = "assigns \\anything: every object's contents are havocked";
        } else if (asg_nothing) {
            assigns_text = "assigns \\nothing: no memory is written";
        } else if (!asg_vals.empty()) {
            for (auto& [p, s] : asg_vals) mt.write_havoc(cur, p.a, s.a, call.line);
            assigns_text = "assigns " + c.assigns + ": only those byte ranges are havocked";
        } else {
            std::size_t np = 0;
            for (std::size_t i = 0; i < call.args.size(); ++i)
                if (sig.params[i].k == SigT::Ptr) {
                    mt.havoc_object_tail(cur, call.args[i], call.line);
                    ++np;
                }
            assigns_text = "no assigns clause: the " + std::to_string(np) + " object(s) the pointer argument(s) point into are havocked and no other memory changes (as --assume-unknown-calls)";
        }
        // ---- result and ensures --------------------------------------------------------------------------------------
        if (sig.ret.k != SigT::Void) {
            if (sig.ret.k == SigT::Ptr) {
                Arg r;
                if (valid_conj) {
                    Val sz = ev.eval(valid_conj->b);
                    if (!(sz.d == Val::S || sz.d == Val::U) || (sz.k && *sz.k < 0)) throw Reject{"ensures: the size of \\valid_bytes is not an integer"};
                    Arg obj = mt.alloc(cur, sz.a, MemKind::Extern, 2, "returned by @" + call.callee + " (callee contract)", call.line);
                    if (nullable) {
                        Arg is_null = t.havoc(cur, 1, false, false);
                        r = t.assign(cur, Op::Select, ptr_w(), {is_null, Arg::c(ptr_w(), 0), obj}, "cptr");
                    } else {
                        r = obj;
                    }
                } else {
                    r = t.havoc(cur, ptr_w(), false, true);
                }
                Val rv;
                rv.d = Val::P;
                rv.a = r;
                ev.result = rv;
                out.result = r;
            } else {
                Arg r = t.havoc(cur, call.ret.bits, false, false);
                ev.result = ev.leaf(r, sig.ret);
                out.result = r;
            }
        }
        for (auto& n : plain_ens) {
            Val v = ev.eval(n);
            if (v.d != Val::B) throw Reject{"ensures: a clause is not a boolean expression"};
            t.assume(cur, v.a);
        }
        out.applied = true;
        out.assumption = "callee-contract: @" + call.callee + " is replaced by its contract (--callee-contracts; source: " + c.source +
                         "; status: " + c.status + "; the contract was not verified against the callee): requires [" + show(c.requires_) +
                         "] is checked at the call site; " + assigns_text + "; ensures [" + show(c.ensures) +
                         "] is assumed; the callee is assumed to return" + (c.ensures.find("\\false") != std::string::npos ? " (unless its ensures say it does not)" : "") +
                         ", to free nothing, to retain no pointer argument and not to call back into the unit";
    } catch (const Reject& r) {
        out.applied = false;
        out.why = r.why;
    }
    return out;
}

}  // namespace pirmem
}  // namespace prism::pir
