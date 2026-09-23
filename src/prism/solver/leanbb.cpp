// Certified mode's Lean-proved bit-blaster (roadmap 3.2 step 2, 5.4, 8.2).
//
// The proof lives in proofs/techniques (PrismTechniques/Bitblast*.lean): the
// executable `prism-bitblast` runs exactly the proved function `toCNF` on a
// formula written in its S-expression format, and `prism-lrat-check --dag`
// runs core Lean's verified LRAT checker on the CNF it rebuilds from the same
// formula (theorem checkDag_sound). What this file adds to the trusted base is
// small and tested: the Z3 -> S-expression serializer (to_lean_dag), checked
// against Z3's own evaluator on random assignments both by the C++ reference
// evaluator below and, when built, by the Lean semantics (`--eval`).

#include "prism/solver.hpp"
#include "internal.hpp"

#ifdef PRISM_HAS_Z3
#include "query.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace prism::solver {

std::string_view bitblaster_name(Bitblaster b) {
    switch (b) {
        case Bitblaster::Lean: return "lean";
        case Bitblaster::Z3: return "z3";
        case Bitblaster::Auto: break;
    }
    return "auto";
}

namespace {

using u128 = unsigned __int128;

bool has_line(std::string_view out, std::string_view line) {
    std::size_t s = 0;
    while (s <= out.size()) {
        std::size_t e = out.find('\n', s);
        if (e == std::string_view::npos) e = out.size();
        auto l = out.substr(s, e - s);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.remove_suffix(1);
        if (l == line) return true;
        s = e + 1;
    }
    return false;
}

std::string tail_of(const std::string& s, std::size_t n = 300) {
    return s.size() <= n ? s : s.substr(s.size() - n);
}

// SMT-LIB literal (#b.., #x.., (_ bvN W), true, false) -> bits, LSB first.
std::optional<std::vector<bool>> literal_bits(std::string_view v, unsigned w) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
    std::vector<bool> bits;
    if (v == "true" || v == "false") {
        if (w != 1) return std::nullopt;
        return std::vector<bool>{v == "true"};
    }
    if (v.size() > 2 && v[0] == '#' && v[1] == 'b') {
        for (std::size_t i = v.size(); i-- > 2;) {
            if (v[i] != '0' && v[i] != '1') return std::nullopt;
            bits.push_back(v[i] == '1');
        }
    } else if (v.size() > 2 && v[0] == '#' && v[1] == 'x') {
        for (std::size_t i = v.size(); i-- > 2;) {
            char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(v[i])));
            int d = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
            if (d < 0) return std::nullopt;
            for (int b = 0; b < 4; ++b) bits.push_back((d >> b) & 1);
        }
    } else if (v.starts_with("(_ bv") && v.back() == ')') {
        std::istringstream in{std::string(v.substr(5, v.size() - 6))};
        std::string num;
        unsigned width = 0;
        if (!(in >> num >> width) || width != w || num.empty() ||
            !std::all_of(num.begin(), num.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
            return std::nullopt;
        // decimal -> binary by repeated halving
        std::string d = num;
        while (bits.size() < w) {
            int rem = 0;
            std::string q;
            for (char ch : d) {
                int cur = rem * 10 + (ch - '0');
                if (!q.empty() || cur / 2) q += char('0' + cur / 2);
                rem = cur % 2;
            }
            bits.push_back(rem == 1);
            d = q.empty() ? "0" : q;
        }
        return bits;
    } else {
        return std::nullopt;
    }
    if (bits.size() != w) return std::nullopt;
    return bits;
}

std::string bits_decimal(const std::vector<bool>& bits) {  // LSB first
    std::string d = "0";
    for (std::size_t i = bits.size(); i-- > 0;) {
        int carry = bits[i] ? 1 : 0;
        for (std::size_t j = d.size(); j-- > 0;) {
            int x = (d[j] - '0') * 2 + carry;
            d[j] = char('0' + x % 10);
            carry = x / 10;
        }
        if (carry) d.insert(d.begin(), char('0' + carry));
    }
    return d;
}

// ---------------------------------------------------------------- reference evaluator
struct SNode {
    std::string atom;
    std::vector<std::size_t> kids;
    bool list = false;
};

bool parse_sexps(std::string_view s, std::vector<SNode>& nodes, std::vector<std::size_t>& tops) {
    std::vector<std::size_t> open;
    std::size_t i = 0;
    while (i < s.size()) {
        char ch = s[i];
        if (ch == ';') { while (i < s.size() && s[i] != '\n') ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(ch))) { ++i; continue; }
        if (ch == '(') {
            nodes.push_back(SNode{{}, {}, true});
            std::size_t id = nodes.size() - 1;
            if (open.empty()) tops.push_back(id);
            else nodes[open.back()].kids.push_back(id);
            open.push_back(id);
            ++i;
            continue;
        }
        if (ch == ')') {
            if (open.empty()) return false;
            open.pop_back();
            ++i;
            continue;
        }
        std::size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' && s[i] != ')') ++i;
        nodes.push_back(SNode{std::string(s.substr(b, i - b)), {}, false});
        if (open.empty()) tops.push_back(nodes.size() - 1);
        else nodes[open.back()].kids.push_back(nodes.size() - 1);
    }
    return open.empty();
}

struct Val {
    unsigned w = 0;
    u128 v = 0;
};

u128 mask_of(unsigned w) { return w >= 128 ? ~u128(0) : ((u128(1) << w) - 1); }

struct Evaluator {
    const std::vector<SNode>& n;
    std::map<unsigned long long, Val> seg;  // base -> segment (the assignment ρ)
    std::string why;

    bool bit(unsigned long long j) const {
        auto it = seg.upper_bound(j);
        if (it == seg.begin()) return false;
        --it;
        if (j >= it->first + it->second.w) return false;
        unsigned k = unsigned(j - it->first);
        return k < 128 && ((it->second.v >> k) & 1);
    }
    void update(unsigned long long base, Val v) {
        // Remove the overlapped part of existing segments (the serializer
        // never overlaps them; keep the semantics exact anyway).
        std::map<unsigned long long, Val> keep;
        for (auto& [b, s] : seg) {
            if (b + s.w <= base || b >= base + v.w) { keep[b] = s; continue; }
            for (unsigned k = 0; k < s.w; ++k) {
                unsigned long long j = b + k;
                if (j >= base && j < base + v.w) continue;
                keep[j] = Val{1, (s.v >> k) & 1};
            }
        }
        keep[base] = v;
        seg = std::move(keep);
    }
    bool num(std::size_t i, unsigned long long& out) {
        const auto& a = n[i];
        if (a.list || a.atom.empty() || !std::all_of(a.atom.begin(), a.atom.end(), ::isdigit)) {
            why = "expected a number";
            return false;
        }
        out = 0;
        for (char ch : a.atom) {
            if (out > (~0ull - 9) / 10) { out = ~0ull; return true; }  // saturate
            out = out * 10 + unsigned(ch - '0');
        }
        return true;
    }
    static __int128 sval(Val x) {
        if (x.w == 0) return 0;
        if (x.w < 128 && ((x.v >> (x.w - 1)) & 1)) return __int128(x.v) - (__int128(1) << (x.w - 1)) * 2;
        return __int128(x.v);
    }
    static bool msb(Val x) { return x.w > 0 && ((x.v >> (x.w - 1)) & 1); }
    static Val mk(unsigned w, u128 v) { return Val{w, v & mask_of(w)}; }
    static Val neg(Val x) { return mk(x.w, ~x.v + 1); }
    static Val udiv(Val x, Val y) { return y.v == 0 ? mk(x.w, ~u128(0)) : mk(x.w, x.v / y.v); }
    static Val urem(Val x, Val y) { return y.v == 0 ? x : mk(x.w, x.v % y.v); }

    std::optional<Val> eval(std::size_t i) {
        const auto& e = n[i];
        if (!e.list || e.kids.empty() || n[e.kids[0]].list) { why = "malformed expression"; return std::nullopt; }
        const std::string& op = n[e.kids[0]].atom;
        const std::size_t k = e.kids.size() - 1;
        auto arg = [&](std::size_t j) { return eval(e.kids[j]); };
        auto bad = [&](std::string m) -> std::optional<Val> { why = m; return std::nullopt; };
        if (op == "var" || op == "const") {
            unsigned long long w = 0, b = 0;
            if (k != 2 || !num(e.kids[1], w)) return bad("bad " + op);
            if (w > 128) return bad("width above 128");
            if (op == "var") {
                if (!num(e.kids[2], b)) return bad("bad var");
                u128 v = 0;
                for (unsigned t = 0; t < w; ++t) v |= u128(bit(b + t)) << t;
                return Val{unsigned(w), v};
            }
            const std::string& d = n[e.kids[2]].atom;
            u128 v = 0;
            for (char ch : d) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) return bad("bad const");
                v = v * 10 + unsigned(ch - '0');  // mod 2^128, then mod 2^w
            }
            return mk(unsigned(w), v);
        }
        if (op == "shlc" || op == "lshrc" || op == "ashrc" || op == "zext" || op == "sext") {
            unsigned long long p = 0;
            if (k != 2 || !num(e.kids[1], p)) return bad("bad " + op);
            auto a = arg(2);
            if (!a) return std::nullopt;
            if (op == "zext" || op == "sext") {
                if (p > 128) return bad("width above 128");
                if (op == "zext" || !msb(*a) || a->w >= 128) return mk(unsigned(p), a->v);
                return mk(unsigned(p), a->v | (~u128(0) << a->w));
            }
            if (op == "shlc") return p >= a->w ? mk(a->w, 0) : mk(a->w, a->v << p);
            if (op == "lshrc") return p >= a->w ? mk(a->w, 0) : mk(a->w, a->v >> p);
            if (p >= a->w) return mk(a->w, msb(*a) ? ~u128(0) : 0);
            u128 r = a->v >> p;
            if (msb(*a) && p > 0) r |= ~u128(0) << (a->w - p);
            return mk(a->w, r);
        }
        if (op == "extract") {
            unsigned long long lo = 0, len = 0;
            if (k != 3 || !num(e.kids[1], lo) || !num(e.kids[2], len)) return bad("bad extract");
            if (len > 128) return bad("width above 128");
            auto a = arg(3);
            if (!a) return std::nullopt;
            return mk(unsigned(len), lo >= 128 ? 0 : a->v >> lo);
        }
        if (op == "not" || op == "neg") {
            if (k != 1) return bad("bad " + op);
            auto a = arg(1);
            if (!a) return std::nullopt;
            return op == "not" ? mk(a->w, ~a->v) : neg(*a);
        }
        if (op == "ite") {
            if (k != 3) return bad("bad ite");
            auto c = arg(1), a = arg(2), b = arg(3);
            if (!c || !a || !b) return std::nullopt;
            if (c->w != 1 || a->w != b->w) return bad("ite widths");
            return (c->v & 1) ? *a : *b;
        }
        if (k != 2) return bad("bad " + op);
        auto a = arg(1), b = arg(2);
        if (!a || !b) return std::nullopt;
        if (op == "concat") {
            if (a->w + b->w > 128) return bad("width above 128");
            return mk(a->w + b->w, (b->w >= 128 ? 0 : (a->v << b->w)) | b->v);
        }
        if (a->w != b->w) return bad("width mismatch in " + op);
        const unsigned w = a->w;
        const u128 M = mask_of(w);
        auto B = [](bool x) { return Val{1, x ? u128(1) : u128(0)}; };
        if (op == "and") return mk(w, a->v & b->v);
        if (op == "or") return mk(w, a->v | b->v);
        if (op == "xor") return mk(w, a->v ^ b->v);
        if (op == "add") return mk(w, a->v + b->v);
        if (op == "sub") return mk(w, a->v - b->v);
        if (op == "mul") return mk(w, a->v * b->v);
        if (op == "udiv") return udiv(*a, *b);
        if (op == "urem") return urem(*a, *b);
        if (op == "sdiv" || op == "srem") {
            Val x = msb(*a) ? neg(*a) : *a, y = msb(*b) ? neg(*b) : *b;
            if (op == "sdiv") {
                Val q = udiv(x, y);
                return (msb(*a) != msb(*b)) ? neg(q) : q;
            }
            Val r = urem(x, y);
            return msb(*a) ? neg(r) : r;
        }
        if (op == "shl") return b->v >= w ? mk(w, 0) : mk(w, a->v << unsigned(b->v));
        if (op == "lshr") return b->v >= w ? mk(w, 0) : mk(w, a->v >> unsigned(b->v));
        if (op == "ashr") {
            if (b->v >= w) return mk(w, msb(*a) ? ~u128(0) : 0);
            unsigned p = unsigned(b->v);
            u128 r = a->v >> p;
            if (msb(*a) && p > 0) r |= ~u128(0) << (w - p);
            return mk(w, r);
        }
        if (op == "eq") return B(a->v == b->v);
        if (op == "ult") return B(a->v < b->v);
        if (op == "ule") return B(a->v <= b->v);
        if (op == "slt") return B(sval(*a) < sval(*b));
        if (op == "sle") return B(sval(*a) <= sval(*b));
        if (op == "uaddo") return B(a->v > M - b->v);
        if (op == "usubo") return B(a->v < b->v);
        if (op == "saddo" || op == "ssubo") {
            Val r = op == "saddo" ? mk(w, a->v + b->v) : mk(w, a->v - b->v);
            bool same = op == "saddo" ? msb(*a) == msb(*b) : msb(*a) != msb(*b);
            return B(same && msb(r) != msb(*a));
        }
        if (op == "umulo") return B(b->v != 0 && a->v > M / b->v);
        if (op == "smulhi" || op == "smullo") {
            if (w == 0) return B(false);
            if (w > 64) return bad("smulo evaluation above 64 bits");
            __int128 p = sval(*a) * sval(*b);
            __int128 lim = __int128(1) << (w - 1);
            return B(op == "smulhi" ? p >= lim : p < -lim);
        }
        return bad("unknown operator " + op);
    }
};

}  // namespace

std::optional<std::string> lean_rho(const LeanDag& dag, const std::map<std::string, std::string>& model) {
    std::string out = "(rho";
    for (const auto& in : dag.inputs) {
        auto it = model.find(in.name);
        std::vector<bool> bits(in.width, false);
        if (it != model.end()) {
            auto b = literal_bits(it->second, in.width);
            if (!b) return std::nullopt;
            bits = *b;
        }
        out += " (" + std::to_string(in.base) + " " + std::to_string(in.width) + " " + bits_decimal(bits) + ")";
    }
    return out + ")";
}

std::optional<bool> eval_lean_dag(const LeanDag& dag, const std::map<std::string, std::string>& model,
                                  std::string* why) {
    auto fail = [&](std::string m) -> std::optional<bool> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    std::vector<SNode> nodes;
    std::vector<std::size_t> tops;
    if (!parse_sexps(dag.text, nodes, tops) || tops.size() != 1) return fail("malformed dag text");
    const auto& d = nodes[tops[0]];
    if (!d.list || d.kids.size() < 2 || nodes[d.kids[0]].atom != "dag") return fail("not a (dag ...) form");
    Evaluator ev{nodes, {}, {}};
    for (const auto& in : dag.inputs) {
        if (in.width > 128) return fail("input wider than 128 bits");
        u128 v = 0;
        if (auto it = model.find(in.name); it != model.end()) {
            auto b = literal_bits(it->second, in.width);
            if (!b) return fail("malformed value for " + in.name);
            for (unsigned i = 0; i < in.width; ++i) v |= u128((*b)[i]) << i;
        }
        ev.update(in.base, Val{in.width, v});
    }
    for (std::size_t i = 1; i + 1 < d.kids.size(); ++i) {
        const auto& df = nodes[d.kids[i]];
        unsigned long long w = 0, b = 0;
        if (!df.list || df.kids.size() != 4 || nodes[df.kids[0]].atom != "def" || !ev.num(df.kids[1], w) ||
            !ev.num(df.kids[2], b))
            return fail("malformed def");
        auto v = ev.eval(df.kids[3]);
        if (!v) return fail(ev.why);
        if (v->w != w) return fail("def width mismatch");
        ev.update(b, *v);
    }
    auto top = ev.eval(d.kids.back());
    if (!top) return fail(ev.why);
    if (top->w != 1) return fail("top formula is not 1 bit wide");
    return (top->v & 1) != 0;
}

// ---------------------------------------------------------------- running the Lean tools
std::optional<Cnf> lean_bitblast(const LeanDag& dag, const ToolInfo& exe, const fs::path& work, double timeout_s,
                                 std::string* why, const std::atomic<bool>* stop) {
    auto fail = [&](std::string m) -> std::optional<Cnf> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    const fs::path dp = work / "query.dag", op = work / "query.bb", cp = work / "query.cnf";
    if (!detail::write_file(dp, dag.text)) return fail("cannot write " + dp.string());
    auto p = detail::run({exe.path.string()}, timeout_s, stop, 1u << 20, dp.string(), op.string());
    if (p.failed) return fail("prism-bitblast did not start: " + p.out);
    if (p.cancelled) return fail("prism-bitblast cancelled");
    if (p.timed_out) return fail("prism-bitblast timed out");
    if (p.rc != 0) return fail("prism-bitblast exit " + std::to_string(p.rc) + ": " + tail_of(p.out));
    std::string txt = detail::read_file(op);
    std::error_code ec;
    fs::remove(op, ec);
    std::size_t pos = txt.rfind("p cnf ", 0) == 0 ? 0 : txt.find("\np cnf ");
    if (pos == std::string::npos) return fail("prism-bitblast wrote no DIMACS header");
    if (pos != 0) ++pos;
    const std::string header = txt.substr(0, pos), dimacs = txt.substr(pos);
    std::map<unsigned long long, std::vector<int>> vars;  // base -> DIMACS vars of the bits
    {
        std::istringstream hs(header);
        std::string line;
        bool magic = false;
        while (std::getline(hs, line)) {
            if (line == "c prism-bitblast 1") { magic = true; continue; }
            std::istringstream ls(line);
            std::string c, tag;
            unsigned long long base = 0, width = 0;
            if (!(ls >> c >> tag) || c != "c" || tag != "var") continue;
            if (!(ls >> base >> width)) return fail("malformed variable map line: " + line);
            std::vector<int> vs;
            long long v = 0;
            while (ls >> v) vs.push_back(int(v));
            if (vs.size() != width) return fail("variable map width mismatch: " + line);
            vars[base] = std::move(vs);
        }
        if (!magic) return fail("prism-bitblast output has no version line");
    }
    std::string perr;
    auto cnf = parse_dimacs(dimacs, &perr);
    if (!cnf) return fail("prism-bitblast DIMACS: " + perr);
    std::size_t mapped = 0;
    for (const auto& in : dag.inputs) {
        Cnf::Symbol sym;
        sym.name = in.name;
        sym.width = in.width;
        sym.is_bool = in.is_bool;
        sym.vars.assign(in.width, 0);  // a variable the formula does not read: not in the CNF
        if (auto it = vars.find(in.base); it != vars.end()) {
            if (it->second.size() != in.width) return fail("variable map disagrees on the width of " + in.name);
            for (unsigned i = 0; i < in.width; ++i) {
                // CNF.dimacs numbers input bit j as DIMACS variable 2*j+1.
                if (it->second[i] != int(2 * (in.base + i) + 1)) return fail("variable map disagrees for " + in.name);
                sym.vars[i] = it->second[i];
            }
            ++mapped;
        }
        cnf->symbols.push_back(std::move(sym));
    }
    if (mapped != vars.size()) return fail("prism-bitblast reports an input the serializer did not declare");
    if (!detail::write_file(cp, dimacs)) return fail("cannot write " + cp.string());
    return cnf;
}

CheckOutcome check_lrat_dag(const ToolInfo& checker, const fs::path& dag, const fs::path& cnf, const fs::path& lrat,
                            double timeout_s) {
    CheckOutcome o;
    o.checker = checker.name;
    o.version = checker.version;
    auto p = detail::run({checker.path.string(), "--dag", dag.string(), cnf.string(), lrat.string()}, timeout_s);
    if (p.failed) { o.detail = p.out; return o; }
    o.ran = true;
    if (p.timed_out) { o.detail = "checker timed out"; return o; }
    o.verified = p.rc == 0 && has_line(p.out, "s VERIFIED UNSAT");
    o.detail = o.verified ? "accepted" : "rejected: " + tail_of(p.out);
    return o;
}

// ---------------------------------------------------------------- the serializer
#ifdef PRISM_HAS_Z3
namespace {

unsigned width_of(const z3::expr& e) { return e.is_bool() ? 1u : e.get_sort().bv_size(); }

std::string balanced(const char* op, const std::vector<std::string>& xs, std::size_t lo, std::size_t hi) {
    if (hi - lo == 1) return xs[lo];
    std::size_t mid = lo + (hi - lo) / 2;
    return std::string("(") + op + " " + balanced(op, xs, lo, mid) + " " + balanced(op, xs, mid, hi) + ")";
}

std::string numeral(const z3::expr& e) { return Z3_get_numeral_string(e.ctx(), e); }

// One node, its arguments already written. Returns false (with why) for an
// operator outside the proved fragment.
bool emit(const z3::expr& e, const std::vector<std::string>& a, const std::string& leaf, std::string& out,
          std::string& why) {
    const auto k = e.decl().decl_kind();
    const std::size_t n = e.num_args();
    const std::string W = std::to_string(width_of(e));
    auto s1 = [&](const char* op) { out = std::string("(") + op + " " + a[0] + ")"; return true; };
    auto s2 = [&](const char* op, std::size_t i, std::size_t j) {
        out = std::string("(") + op + " " + a[i] + " " + a[j] + ")";
        return true;
    };
    auto nary = [&](const char* op, const char* unit) {
        out = n == 0 ? std::string(unit) : balanced(op, a, 0, n);
        return true;
    };
    auto param = [&](unsigned i) { return unsigned(Z3_get_decl_int_parameter(e.ctx(), e.decl(), i)); };
    auto outside = [&]() {
        why = "operator " + e.decl().name().str() + " is outside the proved fragment";
        return false;
    };
    switch (k) {
        case Z3_OP_TRUE: out = "(const 1 1)"; return true;
        case Z3_OP_FALSE: out = "(const 1 0)"; return true;
        case Z3_OP_UNINTERPRETED:
            if (n != 0 || leaf.empty()) { why = "uninterpreted function " + e.decl().name().str(); return false; }
            out = leaf;
            return true;
        case Z3_OP_BNUM: out = "(const " + W + " " + numeral(e) + ")"; return true;
        case Z3_OP_NOT: return n == 1 && s1("not");
        case Z3_OP_AND: return nary("and", "(const 1 1)");
        case Z3_OP_OR: return nary("or", "(const 1 0)");
        case Z3_OP_XOR: return nary("xor", "(const 1 0)");
        case Z3_OP_IMPLIES:
            if (n != 2) return outside();
            out = "(or (not " + a[0] + ") " + a[1] + ")";
            return true;
        case Z3_OP_IFF: return n == 2 && s2("eq", 0, 1);
        case Z3_OP_EQ: return n == 2 ? s2("eq", 0, 1) : outside();
        case Z3_OP_DISTINCT: {
            if (n < 2) { out = "(const 1 1)"; return true; }
            std::vector<std::string> pairs;
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = i + 1; j < n; ++j) pairs.push_back("(not (eq " + a[i] + " " + a[j] + "))");
            out = balanced("and", pairs, 0, pairs.size());
            return true;
        }
        case Z3_OP_ITE: return n == 3 && (out = "(ite " + a[0] + " " + a[1] + " " + a[2] + ")", true);
        case Z3_OP_ULEQ: return s2("ule", 0, 1);
        case Z3_OP_UGEQ: return s2("ule", 1, 0);
        case Z3_OP_ULT: return s2("ult", 0, 1);
        case Z3_OP_UGT: return s2("ult", 1, 0);
        case Z3_OP_SLEQ: return s2("sle", 0, 1);
        case Z3_OP_SGEQ: return s2("sle", 1, 0);
        case Z3_OP_SLT: return s2("slt", 0, 1);
        case Z3_OP_SGT: return s2("slt", 1, 0);
        case Z3_OP_BSMUL_NO_OVFL: out = "(not (smulhi " + a[0] + " " + a[1] + "))"; return true;
        case Z3_OP_BSMUL_NO_UDFL: out = "(not (smullo " + a[0] + " " + a[1] + "))"; return true;
        case Z3_OP_BUMUL_NO_OVFL: out = "(not (umulo " + a[0] + " " + a[1] + "))"; return true;
        case Z3_OP_BNOT: return s1("not");
        case Z3_OP_BNEG: return s1("neg");
        case Z3_OP_BAND: return n > 0 ? nary("and", "") : outside();
        case Z3_OP_BOR: return n > 0 ? nary("or", "") : outside();
        case Z3_OP_BXOR: return n > 0 ? nary("xor", "") : outside();
        case Z3_OP_BNAND: return n == 2 && (out = "(not (and " + a[0] + " " + a[1] + "))", true);
        case Z3_OP_BNOR: return n == 2 && (out = "(not (or " + a[0] + " " + a[1] + "))", true);
        case Z3_OP_BXNOR: return n == 2 && (out = "(not (xor " + a[0] + " " + a[1] + "))", true);
        case Z3_OP_BADD: return n > 0 ? nary("add", "") : outside();
        case Z3_OP_BMUL: return n > 0 ? nary("mul", "") : outside();
        case Z3_OP_BSUB: {
            if (n == 0) return outside();
            out = a[0];
            for (std::size_t i = 1; i < n; ++i) out = "(sub " + out + " " + a[i] + ")";
            return true;
        }
        case Z3_OP_BUDIV: return n == 2 && s2("udiv", 0, 1);
        case Z3_OP_BUREM: return n == 2 && s2("urem", 0, 1);
        case Z3_OP_BSDIV: return n == 2 && s2("sdiv", 0, 1);
        case Z3_OP_BSREM: return n == 2 && s2("srem", 0, 1);
        case Z3_OP_BSHL:
        case Z3_OP_BLSHR:
        case Z3_OP_BASHR: {
            if (n != 2) return outside();
            const char* v = k == Z3_OP_BSHL ? "shl" : k == Z3_OP_BLSHR ? "lshr" : "ashr";
            if (e.arg(1).is_numeral()) {
                out = std::string("(") + v + "c " + numeral(e.arg(1)) + " " + a[0] + ")";
                return true;
            }
            return s2(v, 0, 1);
        }
        case Z3_OP_CONCAT: return n > 0 ? nary("concat", "") : outside();
        case Z3_OP_EXTRACT: {
            const unsigned hi = param(0), lo = param(1);
            out = "(extract " + std::to_string(lo) + " " + std::to_string(hi - lo + 1) + " " + a[0] + ")";
            return true;
        }
        case Z3_OP_ZERO_EXT:
        case Z3_OP_SIGN_EXT: {
            const unsigned w0 = width_of(e.arg(0));
            out = std::string(k == Z3_OP_ZERO_EXT ? "(zext " : "(sext ") + std::to_string(w0 + param(0)) + " " + a[0] + ")";
            return true;
        }
        case Z3_OP_REPEAT: {
            const unsigned r = param(0);
            if (r == 0) return outside();
            std::vector<std::string> copies(r, a[0]);
            out = balanced("concat", copies, 0, r);
            return true;
        }
        case Z3_OP_ROTATE_LEFT:
        case Z3_OP_ROTATE_RIGHT: {
            const unsigned w = width_of(e);
            if (w == 0) return outside();
            unsigned s = param(0) % w;
            if (k == Z3_OP_ROTATE_RIGHT) s = (w - s) % w;
            if (s == 0) { out = a[0]; return true; }
            // rotl(x, s) = x[w-s-1:0] ++ x[w-1:w-s]
            out = "(concat (extract 0 " + std::to_string(w - s) + " " + a[0] + ") (extract " + std::to_string(w - s) +
                  " " + std::to_string(s) + " " + a[0] + "))";
            return true;
        }
        case Z3_OP_BCOMP: return n == 2 && s2("eq", 0, 1);
        case Z3_OP_BREDOR: {
            const std::string w0 = std::to_string(width_of(e.arg(0)));
            out = "(not (eq " + a[0] + " (const " + w0 + " 0)))";
            return true;
        }
        case Z3_OP_BREDAND: {
            const std::string w0 = std::to_string(width_of(e.arg(0)));
            out = "(eq " + a[0] + " (not (const " + w0 + " 0)))";
            return true;
        }
        default: return outside();
    }
}

bool is_leaf_text(const std::string& t) { return t.rfind("(var ", 0) == 0 || t.rfind("(const ", 0) == 0; }

}  // namespace

std::optional<LeanDag> to_lean_dag(const z3::expr& f, std::string* why) {
    auto fail = [&](std::string m) -> std::optional<LeanDag> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    if (!f.is_bool()) return fail("formula is not Boolean");
    LeanDag out;
    std::unordered_map<unsigned, std::string> leaf;  // input constant id -> "(var W BASE)"
    unsigned long long base = 0;
    for (const auto& k : detail::collect_consts(f)) {
        if (!k.is_bool() && !k.is_bv())
            return fail("constant " + detail::const_name(k) + " of sort " + k.get_sort().to_string());
        LeanDag::Input in{detail::const_name(k), width_of(k), k.is_bool(), base};
        leaf[k.id()] = "(var " + std::to_string(in.width) + " " + std::to_string(base) + ")";
        base += in.width;
        out.inputs.push_back(std::move(in));
    }
    // Parent counts: a node with more than one use becomes a definition.
    // Operators that copy an argument (repeat, rotate, n-ary distinct) count
    // double so that argument is written once.
    std::unordered_map<unsigned, unsigned> refs;
    {
        std::unordered_set<unsigned> seen;
        std::vector<z3::expr> st{f};
        while (!st.empty()) {
            z3::expr e = st.back();
            st.pop_back();
            if (!seen.insert(e.id()).second) continue;
            if (!e.is_app()) return fail("quantifier or bound variable");
            if (!e.is_bool() && !e.is_bv()) return fail("term of sort " + e.get_sort().to_string());
            const auto k = e.decl().decl_kind();
            const unsigned extra = (k == Z3_OP_REPEAT || k == Z3_OP_ROTATE_LEFT || k == Z3_OP_ROTATE_RIGHT ||
                                    (k == Z3_OP_DISTINCT && e.num_args() > 2))
                                       ? 2u
                                       : 1u;
            for (unsigned i = 0; i < e.num_args(); ++i) {
                refs[e.arg(i).id()] += extra;
                st.push_back(e.arg(i));
            }
        }
    }
    // Post-order emission with an explicit stack (deep terms do not recurse).
    // Definitions get bases above every input, in emission order, so each
    // reads only inputs and earlier definitions (the defsOK check).
    std::unordered_map<unsigned, std::string> text;
    std::unordered_map<unsigned, unsigned> depth;
    std::string defs;
    struct Frame {
        z3::expr e;
        bool expanded;
    };
    std::vector<Frame> st{{f, false}};
    std::string reason;
    while (!st.empty()) {
        Frame fr = st.back();
        st.pop_back();
        const unsigned id = fr.e.id();
        if (text.count(id)) continue;
        if (!fr.expanded) {
            st.push_back({fr.e, true});
            for (unsigned i = fr.e.num_args(); i-- > 0;)
                if (!text.count(fr.e.arg(i).id())) st.push_back({fr.e.arg(i), false});
            continue;
        }
        std::vector<std::string> args;
        unsigned d = 0;
        for (unsigned i = 0; i < fr.e.num_args(); ++i) {
            const unsigned aid = fr.e.arg(i).id();
            args.push_back(text.at(aid));
            d = std::max(d, depth[aid]);
        }
        auto lf = leaf.find(id);
        std::string t;
        if (!emit(fr.e, args, lf == leaf.end() ? std::string() : lf->second, t, reason)) return fail(reason);
        // An argument used once has been consumed.
        for (unsigned i = 0; i < fr.e.num_args(); ++i) {
            const unsigned aid = fr.e.arg(i).id();
            if (refs[aid] <= 1) { text.erase(aid); depth.erase(aid); }
        }
        const bool leafy = is_leaf_text(t);
        d = leafy ? 0 : d + 1;
        if (!leafy && (refs[id] > 1 || d > 64)) {
            const unsigned w = width_of(fr.e);
            defs += "(def " + std::to_string(w) + " " + std::to_string(base) + " " + t + ")\n";
            t = "(var " + std::to_string(w) + " " + std::to_string(base) + ")";
            base += w;
            ++out.defs;
            d = 0;
        }
        text[id] = std::move(t);
        depth[id] = d;
    }
    out.text = "(dag\n" + defs + text.at(f.id()) + ")\n";
    return out;
}
#endif  // PRISM_HAS_Z3

}  // namespace prism::solver
