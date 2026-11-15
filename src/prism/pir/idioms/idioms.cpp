// Loop idiom library: shapes, matchers and invariant templates (idioms.hpp).
//
// Everything here is a heuristic for choosing candidates. A wrong match, a
// wrong bound or a wrong interval only makes a candidate that Houdini's base,
// step or final query rejects; no proof relies on this file.

#include "idioms.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace prism::pir::idioms {
namespace {

using i128 = __int128;

uint64_t wmask(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }
int64_t as_signed(uint64_t v, unsigned w) {
    if (w == 0) return 0;
    if (w >= 64) return static_cast<int64_t>(v);
    const uint64_t sign = uint64_t{1} << (w - 1);
    v &= wmask(w);
    return static_cast<int64_t>((v ^ sign) - sign);
}
bool is_cmp(Op o) { return o >= Op::Eq && o <= Op::Sge; }
bool pow2(uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }

// Linear form: c + sum(sign * leaf), leaves are variables not defined by
// add/sub/copy (or past the depth limit).
struct Lin {
    uint64_t c = 0;
    unsigned w = 0;
    std::vector<std::pair<int, int>> leaves;  // (var, +1/-1)
    int count(int v, int sign) const {
        int n = 0;
        for (auto& [x, s] : leaves)
            if (x == v && s == sign) ++n;
        return n;
    }
    // the leaves other than one occurrence of (v, +1)
    std::vector<std::pair<int, int>> others(int v) const {
        std::vector<std::pair<int, int>> o;
        bool skipped = false;
        for (auto& l : leaves) {
            if (!skipped && l.first == v && l.second == 1) {
                skipped = true;
                continue;
            }
            o.push_back(l);
        }
        return o;
    }
};

// One way a back edge can update a header phi.
struct Upd {
    enum K { Same, AddC, AddV, MaskAdd, ModAdd, SetC, Shift, Inner, Other } k = Other;
    int64_t c = 0;                           // AddC / MaskAdd / ModAdd step, SetC value, Shift amount
    uint64_t m = 0;                          // MaskAdd mask, ModAdd modulus
    std::vector<std::pair<int, int>> vars;   // AddV: the added leaves (with c)
    int inner = -1, inner_phi = -1;          // Inner: loop index, its header phi index
    Op op = Op::Copy;                        // Shift / Other
};

Upd mk(Upd::K k) {
    Upd u;
    u.k = k;
    return u;
}

struct Guard {
    Op op;
    Lin l, r;
    bool exit = false;  // one successor leaves the loop
};

struct Access {
    bool store = false;
    unsigned bytes = 0;  // access width
    int base = -1;       // scope pointer variable the address is derived from
    unsigned eb = 1;     // element bytes (index scale)
    int idx = -1;        // the index variable (before zext/sext), -1: none / constant
    bool sext = false;
    Lin lin;             // the index's linear form
    bool masked = false; // index = x & m
    uint64_t m = 0;
    uint64_t stored_const = 0;
    bool stores_const = false;
};

class Ana {
public:
    const Function& fn;
    const std::vector<LoopView>& loops;
    std::vector<const Stmt*> def;
    std::vector<int> def_block;
    std::vector<std::pair<int, int>> phi_at;  // var -> (block, phi index)
    std::vector<std::vector<int>> preds;

    Ana(const Function& f, const std::vector<LoopView>& ls) : fn(f), loops(ls) {
        def.assign(fn.vars.size(), nullptr);
        def_block.assign(fn.vars.size(), -1);
        phi_at.assign(fn.vars.size(), {-1, -1});
        preds.assign(fn.blocks.size(), {});
        for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
            auto& bl = fn.blocks[b];
            for (std::size_t i = 0; i < bl.phis.size(); ++i) {
                const int d = bl.phis[i].dst;
                if (d >= 0) {
                    phi_at[static_cast<std::size_t>(d)] = {static_cast<int>(b), static_cast<int>(i)};
                    def_block[static_cast<std::size_t>(d)] = static_cast<int>(b);
                }
            }
            for (auto& s : bl.stmts) {
                for (int d : {s.dst, s.dst2, s.dst3})
                    if (d >= 0) def_block[static_cast<std::size_t>(d)] = static_cast<int>(b);
                if (s.kind == Stmt::Assign && s.dst >= 0) def[static_cast<std::size_t>(s.dst)] = &s;
            }
            for (int t : {bl.term.t, bl.term.f})
                if (t >= 0 && static_cast<std::size_t>(t) < fn.blocks.size() &&
                    (bl.term.kind == Term::Br || (bl.term.kind == Term::Jmp && t == bl.term.t)))
                    preds[static_cast<std::size_t>(t)].push_back(static_cast<int>(b));
        }
    }
    bool in(std::size_t L, int b) const {
        return b >= 0 && static_cast<std::size_t>(b) < loops[L].body.size() && loops[L].body[static_cast<std::size_t>(b)];
    }
    int header(std::size_t L) const { return loops[L].header; }
    const Block& hblock(std::size_t L) const { return fn.blocks[static_cast<std::size_t>(header(L))]; }
    int phi_var(std::size_t L, int i) const { return hblock(L).phis[static_cast<std::size_t>(i)].dst; }
    unsigned width(int v) const { return fn.vars[static_cast<std::size_t>(v)].width; }
    // header phi index of v in loop L, or -1
    int hphi(std::size_t L, int v) const {
        if (v < 0) return -1;
        auto [b, i] = phi_at[static_cast<std::size_t>(v)];
        return b == header(L) ? i : -1;
    }
    int loop_of_header(int b) const {
        for (std::size_t M = 0; M < loops.size(); ++M)
            if (loops[M].header == b) return static_cast<int>(M);
        return -1;
    }
    // M is nested in L (M != L)
    bool inside(std::size_t M, std::size_t L) const { return M != L && in(L, header(M)); }
    int depth(std::size_t L) const {
        int d = 1;
        for (std::size_t M = 0; M < loops.size(); ++M)
            if (inside(L, M)) ++d;
        return d;
    }

    Lin lin(const Arg& a, int depth = 0) const {
        Lin r;
        r.w = a.width;
        if (a.is_const) {
            r.c = a.bits & wmask(a.width);
            return r;
        }
        auto* s = def[static_cast<std::size_t>(a.var)];
        if (depth < 8 && s && s->args.size() >= 1) {
            if (s->op == Op::Copy && s->args.size() == 1) return lin(s->args[0], depth + 1);
            if ((s->op == Op::Add || s->op == Op::Sub) && s->args.size() == 2 && !s->ptr_arith) {
                auto x = lin(s->args[0], depth + 1), y = lin(s->args[1], depth + 1);
                r.c = (s->op == Op::Add ? x.c + y.c : x.c - y.c) & wmask(r.w);
                r.leaves = x.leaves;
                for (auto [v, sg] : y.leaves) r.leaves.push_back({v, s->op == Op::Add ? sg : -sg});
                return r;
            }
        }
        r.leaves.push_back({a.var, 1});
        return r;
    }

    // Value ranges (signed, mathematical) for candidate constants. Heuristic:
    // counter phis are taken to be inside their guard's range in the body.
    std::optional<std::pair<i128, i128>> interval(const Arg& a, int depth = 0) const {
        constexpr i128 kLim = static_cast<i128>(1) << 100;
        auto ok = [&](i128 lo, i128 hi) -> std::optional<std::pair<i128, i128>> {
            if (lo > hi || lo < -kLim || hi > kLim) return std::nullopt;
            return std::make_pair(lo, hi);
        };
        auto full_s = [](unsigned w) {
            return std::make_pair(-(static_cast<i128>(1) << (w - 1)), (static_cast<i128>(1) << (w - 1)) - 1);
        };
        auto full_u = [](unsigned w) { return std::make_pair(static_cast<i128>(0), (static_cast<i128>(1) << w) - 1); };
        if (a.is_const) {
            const i128 v = as_signed(a.bits, a.width);
            return std::make_pair(v, v);
        }
        if (depth > 10) return std::nullopt;
        const int v = a.var;
        if (auto [b, i] = phi_at[static_cast<std::size_t>(v)]; b >= 0) {
            const int M = loop_of_header(b);
            if (M >= 0) return counter_range(static_cast<std::size_t>(M), i);
            return std::nullopt;
        }
        auto* s = def[static_cast<std::size_t>(v)];
        if (!s || s->op == Op::Havoc) return guard_range(v);
        auto arg = [&](std::size_t k) { return interval(s->args[k], depth + 1); };
        const unsigned w = width(v);
        switch (s->op) {
            case Op::Copy: return arg(0);
            case Op::ZExt: {
                const unsigned sw = s->args[0].width;
                if (sw == 0 || sw > 64) return std::nullopt;
                auto x = arg(0);
                if (x && x->first >= 0) return x;
                return full_u(sw);
            }
            case Op::SExt: {
                const unsigned sw = s->args[0].width;
                if (sw == 0 || sw > 64) return std::nullopt;
                auto x = arg(0);
                if (x) return x;
                return full_s(sw);
            }
            case Op::Trunc: {
                auto x = arg(0);
                if (x && x->first >= 0 && x->second < (static_cast<i128>(1) << (w - 1))) return x;
                return std::nullopt;
            }
            case Op::And: {
                for (std::size_t k = 0; k < 2 && k < s->args.size(); ++k)
                    if (s->args[k].is_const) {
                        const i128 m = as_signed(s->args[k].bits, s->args[k].width);
                        if (m >= 0) return std::make_pair(static_cast<i128>(0), m);
                    }
                return std::nullopt;
            }
            case Op::Or:
            case Op::Xor: {
                auto x = arg(0), y = arg(1);
                if (!x || !y || x->first < 0 || y->first < 0) return std::nullopt;
                i128 hi = std::max(x->second, y->second), p = 1;
                while (p <= hi) p <<= 1;
                return std::make_pair(static_cast<i128>(0), p - 1);
            }
            case Op::LShr: {
                if (!s->args[1].is_const) return std::nullopt;
                const unsigned sh = static_cast<unsigned>(s->args[1].bits);
                if (sh >= w) return std::nullopt;
                auto x = arg(0);
                if (x && x->first >= 0) return std::make_pair(x->first >> sh, x->second >> sh);
                return std::make_pair(static_cast<i128>(0), full_u(w).second >> sh);
            }
            case Op::Add:
            case Op::Sub:
            case Op::Mul: {
                auto x = arg(0), y = arg(1);
                if (!x || !y) return std::nullopt;
                if (s->op == Op::Add) return ok(x->first + y->first, x->second + y->second);
                if (s->op == Op::Sub) return ok(x->first - y->second, x->second - y->first);
                const i128 p[] = {x->first * y->first, x->first * y->second, x->second * y->first, x->second * y->second};
                return ok(*std::min_element(p, p + 4), *std::max_element(p, p + 4));
            }
            case Op::Select: {
                auto x = arg(1), y = arg(2);
                if (!x || !y) return std::nullopt;
                return std::make_pair(std::min(x->first, y->first), std::max(x->second, y->second));
            }
            default: return std::nullopt;
        }
    }

    // Range of a value from the branches that leave the function before any
    // loop when it is out of range: `if (a < 0 || a > K) return 0;` gives
    // [0, K] inside the loops those branches dominate (heuristic).
    mutable std::optional<std::vector<std::vector<char>>> dom_;
    mutable std::map<int, std::optional<std::pair<i128, i128>>> grange_;
    std::optional<std::pair<i128, i128>> guard_range(int v) const {
        if (auto it = grange_.find(v); it != grange_.end()) return it->second;
        auto& res = grange_[v];
        if (loops.empty()) return res;
        if (!dom_) dom_ = dominators_of();
        const std::size_t n = fn.blocks.size();
        const unsigned w = width(v);
        if (w == 0 || w > 64) return res;
        i128 lo = -(static_cast<i128>(1) << (w - 1)), hi = (static_cast<i128>(1) << (w - 1)) - 1;
        bool any = false;
        auto reaches_loop = [&](int s0) {
            std::vector<char> seen(n, 0);
            std::vector<int> st{s0};
            while (!st.empty()) {
                const int x = st.back();
                st.pop_back();
                if (x < 0 || static_cast<std::size_t>(x) >= n || seen[static_cast<std::size_t>(x)]) continue;
                seen[static_cast<std::size_t>(x)] = 1;
                for (auto& L : loops)
                    if (L.header == x) return true;
                auto& t = fn.blocks[static_cast<std::size_t>(x)].term;
                if (t.kind == Term::Jmp) st.push_back(t.t);
                if (t.kind == Term::Br) {
                    st.push_back(t.t);
                    st.push_back(t.f);
                }
            }
            return false;
        };
        for (std::size_t b = 0; b < n; ++b) {
            auto& t = fn.blocks[b].term;
            if (t.kind != Term::Br || t.cond.is_const) continue;
            bool in_loop = false, doms = false;
            for (std::size_t L = 0; L < loops.size(); ++L) {
                in_loop = in_loop || in(L, static_cast<int>(b));
                doms = doms || (*dom_)[static_cast<std::size_t>(loops[L].header)][b];
            }
            if (in_loop || !doms) continue;
            auto* c = def[static_cast<std::size_t>(t.cond.var)];
            if (!c || !is_cmp(c->op) || c->args.size() != 2) continue;
            Op op = c->op;
            Arg x = c->args[0], k = c->args[1];
            if (x.is_const && !k.is_const) {
                std::swap(x, k);
                switch (op) {  // K op v  ==  v op' K
                    case Op::Slt: op = Op::Sgt; break;
                    case Op::Sle: op = Op::Sge; break;
                    case Op::Sgt: op = Op::Slt; break;
                    case Op::Sge: op = Op::Sle; break;
                    case Op::Ult: op = Op::Ugt; break;
                    case Op::Ule: op = Op::Uge; break;
                    case Op::Ugt: op = Op::Ult; break;
                    case Op::Uge: op = Op::Ule; break;
                    default: break;
                }
            }
            if (x.is_const || x.var != v || !k.is_const) continue;
            // the side that cannot reach a loop is left: the other holds in the loops
            const bool t_out = !reaches_loop(t.t), f_out = !reaches_loop(t.f);
            if (t_out == f_out) continue;
            const bool holds = f_out;  // cond holds in the loops when the false side leaves
            const i128 K = as_signed(k.bits, k.width);
            const i128 KU = static_cast<i128>(k.bits & wmask(k.width));
            auto meet = [&](i128 l, i128 h) {
                lo = std::max(lo, l);
                hi = std::min(hi, h);
                any = true;
            };
            const i128 big = static_cast<i128>(1) << 100;
            Op e = op;
            if (!holds) switch (op) {  // negate
                    case Op::Slt: e = Op::Sge; break;
                    case Op::Sle: e = Op::Sgt; break;
                    case Op::Sgt: e = Op::Sle; break;
                    case Op::Sge: e = Op::Slt; break;
                    case Op::Ult: e = Op::Uge; break;
                    case Op::Ule: e = Op::Ugt; break;
                    case Op::Ugt: e = Op::Ule; break;
                    case Op::Uge: e = Op::Ult; break;
                    default: e = Op::Copy; break;
                }
            switch (e) {
                case Op::Slt: meet(-big, K - 1); break;
                case Op::Sle: meet(-big, K); break;
                case Op::Sgt: meet(K + 1, big); break;
                case Op::Sge: meet(K, big); break;
                case Op::Ult: meet(0, KU - 1); break;
                case Op::Ule: meet(0, KU); break;
                default: break;
            }
        }
        if (any && lo <= hi) res = std::make_pair(lo, hi);
        return res;
    }
    std::vector<std::vector<char>> dominators_of() const;

    // ---- per-loop facts (memoised)
    struct PhiFacts {
        std::vector<Upd> upd;
        std::optional<uint64_t> entry_const;  // the one constant entry value
        int entry_var = -1;                   // the one variable entry value
    };
    struct LoopFacts {
        std::vector<PhiFacts> phis;
        std::vector<Guard> guards;
        std::vector<Access> acc;
        int exits = 0;
    };
    mutable std::map<std::size_t, LoopFacts> memo;

    const LoopFacts& facts(std::size_t L) const {
        if (auto it = memo.find(L); it != memo.end()) return it->second;
        LoopFacts F;
        const auto& H = hblock(L);
        for (std::size_t i = 0; i < H.phis.size(); ++i) {
            PhiFacts pf;
            const int P = H.phis[i].dst;
            bool one_entry = true;
            for (auto& [pb, a] : H.phis[i].in) {
                if (in(L, pb)) {
                    std::set<int> seen;
                    walk(L, P, a, seen, pf.upd, 0);
                } else {
                    if (a.is_const) {
                        if (pf.entry_const && *pf.entry_const != a.bits) one_entry = false;
                        pf.entry_const = a.bits;
                    } else {
                        if (pf.entry_var >= 0 && pf.entry_var != a.var) one_entry = false;
                        pf.entry_var = a.var;
                    }
                }
            }
            if (!one_entry || (pf.entry_const && pf.entry_var >= 0)) {
                pf.entry_const.reset();
                pf.entry_var = -1;
            }
            F.phis.push_back(std::move(pf));
        }
        for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
            if (!in(L, static_cast<int>(b))) continue;
            auto& bl = fn.blocks[b];
            if (bl.term.kind == Term::Br && !bl.term.cond.is_const) {
                const bool ex = !in(L, bl.term.t) || !in(L, bl.term.f);
                if (ex) ++F.exits;
                auto* s = def[static_cast<std::size_t>(bl.term.cond.var)];
                if (s && s->kind == Stmt::Assign && is_cmp(s->op) && s->args.size() == 2)
                    F.guards.push_back({s->op, lin(s->args[0]), lin(s->args[1]), ex});
            } else if (bl.term.kind == Term::Ret || bl.term.kind == Term::Stop) {
                ++F.exits;
            }
            for (auto& s : bl.stmts) {
                if (s.kind != Stmt::Load && s.kind != Stmt::Store) continue;
                if (s.args.empty() || s.args[0].is_const) continue;
                Access ac;
                ac.store = s.kind == Stmt::Store;
                const unsigned bits = ac.store ? s.args[1].width : width(s.dst);
                ac.bytes = (bits + 7) / 8;
                if (ac.store && s.args[1].is_const) {
                    ac.stores_const = true;
                    ac.stored_const = s.args[1].bits;
                }
                address(L, s.args[0].var, ac);
                F.acc.push_back(ac);
            }
        }
        return memo.emplace(L, std::move(F)).first->second;
    }

    // base + idx * eb with base defined before the loop
    void address(std::size_t L, int v, Access& ac) const {
        for (int guard = 0; v >= 0 && guard < 16; ++guard) {
            const int d = def_block[static_cast<std::size_t>(v)];
            if (d < 0 || !in(L, d)) {
                ac.base = v;
                return;
            }
            auto* s = def[static_cast<std::size_t>(v)];
            if (!s || s->args.empty() || s->args[0].is_const) return;
            if (s->op == Op::Copy) {
                v = s->args[0].var;
                continue;
            }
            if (!(s->ptr_arith && s->op == Op::Add && s->args.size() == 2)) return;
            if (ac.idx < 0 && !s->args[1].is_const) index(s->args[1], ac);
            v = s->args[0].var;
        }
    }
    void index(const Arg& off, Access& ac) const {
        Arg x = off;
        if (auto* s = def[static_cast<std::size_t>(x.var)]; s && s->op == Op::Mul && s->args.size() == 2 &&
                                                            s->args[1].is_const && !s->args[0].is_const) {
            ac.eb = static_cast<unsigned>(s->args[1].bits);
            x = s->args[0];
        }
        if (auto* s = def[static_cast<std::size_t>(x.var)];
            s && (s->op == Op::ZExt || s->op == Op::SExt) && !s->args[0].is_const) {
            ac.sext = s->op == Op::SExt;
            x = s->args[0];
        }
        if (auto* s = def[static_cast<std::size_t>(x.var)]; s && s->op == Op::And && s->args.size() == 2 &&
                                                            s->args[1].is_const && !s->args[0].is_const) {
            ac.masked = true;
            ac.m = s->args[1].bits;
        }
        ac.idx = x.var;
        ac.lin = lin(x);
    }

    void walk(std::size_t L, int P, const Arg& a, std::set<int>& seen, std::vector<Upd>& out, int depth) const {
        auto push = [&](Upd u) {
            for (auto& o : out)
                if (o.k == u.k && o.c == u.c && o.m == u.m && o.vars == u.vars && o.inner == u.inner &&
                    o.inner_phi == u.inner_phi && o.op == u.op)
                    return;
            out.push_back(std::move(u));
        };
        if (a.is_const) {
            Upd u;
            u.k = Upd::SetC;
            u.c = as_signed(a.bits, a.width);
            push(u);
            return;
        }
        const int v = a.var;
        if (v == P) {
            push(mk(Upd::Same));
            return;
        }
        if (depth > 12 || !seen.insert(v).second) return;
        if (auto [b, i] = phi_at[static_cast<std::size_t>(v)]; b >= 0) {
            if (b == header(L) || !in(L, b)) {
                push(mk(Upd::Other));
                return;
            }
            if (const int M = loop_of_header(b); M >= 0 && inside(static_cast<std::size_t>(M), L)) {
                Upd u;
                u.k = Upd::Inner;
                u.inner = M;
                u.inner_phi = i;
                push(u);
                return;
            }
            for (auto& [pb, x] : fn.blocks[static_cast<std::size_t>(b)].phis[static_cast<std::size_t>(i)].in) {
                (void)pb;
                walk(L, P, x, seen, out, depth + 1);
            }
            return;
        }
        auto* s = def[static_cast<std::size_t>(v)];
        if (!s) {
            push(mk(Upd::Other));
            return;
        }
        auto self_plus = [&](const Arg& x, int64_t& c) {
            auto l = lin(x);
            if (l.leaves.size() != 1 || l.leaves[0] != std::make_pair(P, 1)) return false;
            c = as_signed(l.c, l.w);
            return true;
        };
        switch (s->op) {
            case Op::Copy: walk(L, P, s->args[0], seen, out, depth + 1); return;
            case Op::Select: walk(L, P, s->args[1], seen, out, depth + 1); walk(L, P, s->args[2], seen, out, depth + 1); return;
            case Op::Add:
            case Op::Sub: {
                auto l = lin(Arg::v(v, width(v)));
                if (l.count(P, 1) == 1 && l.count(P, -1) == 0) {
                    Upd u;
                    u.c = as_signed(l.c, l.w);
                    u.vars = l.others(P);
                    u.k = u.vars.empty() ? Upd::AddC : Upd::AddV;
                    push(u);
                } else {
                    Upd u;
                    u.op = s->op;
                    push(u);
                }
                return;
            }
            case Op::And:
            case Op::URem: {
                int64_t c = 0;
                if (s->args.size() == 2 && s->args[1].is_const && self_plus(s->args[0], c)) {
                    Upd u;
                    u.k = s->op == Op::And ? Upd::MaskAdd : Upd::ModAdd;
                    u.c = c;
                    u.m = s->args[1].bits;
                    push(u);
                    return;
                }
                Upd u;
                u.op = s->op;
                push(u);
                return;
            }
            case Op::LShr:
            case Op::AShr:
            case Op::Shl:
                if (s->args.size() == 2 && !s->args[0].is_const && s->args[0].var == P && s->args[1].is_const) {
                    Upd u;
                    u.k = Upd::Shift;
                    u.op = s->op;
                    u.c = static_cast<int64_t>(s->args[1].bits);
                    push(u);
                    return;
                }
                [[fallthrough]];
            default: {
                Upd u;
                u.op = s->op;
                push(u);
            }
        }
    }

    // A unit up-counter from a constant with a constant exit bound: its range
    // in the body [c0, K - 1] (heuristic, intervals only).
    std::optional<std::pair<i128, i128>> counter_range(std::size_t L, int i) const {
        auto& F = facts(L);
        auto& pf = F.phis[static_cast<std::size_t>(i)];
        if (!pf.entry_const || pf.upd.empty()) return std::nullopt;
        for (auto& u : pf.upd)
            if (!(u.k == Upd::AddC && u.c == 1)) return std::nullopt;
        const int P = phi_var(L, i);
        const unsigned w = width(P);
        const i128 c0 = as_signed(*pf.entry_const, w);
        for (auto& g : F.guards) {
            if (!g.exit || !(g.op == Op::Slt || g.op == Op::Ult)) continue;
            if (g.l.leaves.size() == 1 && g.l.leaves[0] == std::make_pair(P, 1) && g.r.leaves.empty()) {
                const i128 K = as_signed(g.r.c, g.r.w) - as_signed(g.l.c, g.l.w);
                if (K > c0) return std::make_pair(c0, K - 1);
            }
        }
        return std::nullopt;
    }
    // exact trip count of a unit counter loop with one exit (its guard)
    std::optional<int64_t> trip_count(std::size_t L, int* counter = nullptr) const {
        auto& F = facts(L);
        if (F.exits != 1) return std::nullopt;
        for (std::size_t i = 0; i < F.phis.size(); ++i)
            if (auto r = counter_range(L, static_cast<int>(i))) {
                if (counter) *counter = static_cast<int>(i);
                return static_cast<int64_t>(r->second - r->first + 1);
            }
        return std::nullopt;
    }
};

// ---- shapes

std::string upd_text(const Upd& u) {
    switch (u.k) {
        case Upd::Same: return "=";
        case Upd::AddC: return u.c == 1 ? "+1" : u.c == -1 ? "-1" : "+c";
        case Upd::AddV: return "+v";
        case Upd::MaskAdd: return std::string(u.c == 1 ? "+1" : "+c") + "&" + (pow2(u.m + 1) ? "m" : "k");
        case Upd::ModAdd: return std::string(u.c == 1 ? "+1" : "+c") + "%" + (pow2(u.m) ? "m" : "k");
        case Upd::SetC: return "c";
        case Upd::Shift: return std::string(u.op == Op::Shl ? "<<" : ">>") + (u.c == 1 ? "1" : "c");
        case Upd::Inner: return "inner";
        case Upd::Other: return std::string("?") + (u.op == Op::Copy ? "" : op_name(u.op));
    }
    return "?";
}

std::string lin_text(const Ana& A, std::size_t L, const Lin& l) {
    std::vector<std::string> parts;
    for (auto& [v, s] : l.leaves) {
        std::string t;
        const int i = A.hphi(L, v);
        if (i >= 0) t = "p" + std::to_string(i);
        else if (A.def_block[static_cast<std::size_t>(v)] < 0 || !A.in(L, A.def_block[static_cast<std::size_t>(v)]))
            t = "s";
        else {
            // an outer loop's phi reads as s; a body value
            t = "x";
        }
        parts.push_back((s < 0 ? "-" : parts.empty() ? "" : "+") + t);
    }
    std::sort(parts.begin(), parts.end());
    std::string r;
    for (auto& p : parts) r += p;
    if (l.c != 0) r += (r.empty() ? "c" : "+c");
    return r.empty() ? "0" : r;
}

std::string shape_text(const Ana& A, std::size_t L) {
    auto& F = A.facts(L);
    std::ostringstream o;
    o << "d" << A.depth(L) << " phis[";
    for (std::size_t i = 0; i < F.phis.size(); ++i) {
        const int P = A.phi_var(L, static_cast<int>(i));
        auto& var = A.fn.vars[static_cast<std::size_t>(P)];
        if (i) o << ' ';
        o << (var.fp ? "f" : "i") << var.width << ':';
        std::vector<std::string> us;
        for (auto& u : F.phis[i].upd) us.push_back(upd_text(u));
        std::sort(us.begin(), us.end());
        us.erase(std::unique(us.begin(), us.end()), us.end());
        for (std::size_t k = 0; k < us.size(); ++k) o << (k ? "|" : "") << us[k];
    }
    o << "] exit[";
    std::vector<std::string> gs;
    for (auto& g : F.guards)
        if (g.exit) gs.push_back(std::string(op_name(g.op)) + "(" + lin_text(A, L, g.l) + "," + lin_text(A, L, g.r) + ")");
    std::sort(gs.begin(), gs.end());
    for (std::size_t k = 0; k < gs.size(); ++k) o << (k ? " " : "") << gs[k];
    o << "] mem[";
    std::vector<std::string> ms;
    for (auto& a : F.acc) {
        std::string ix = a.idx < 0 ? std::string("-") : lin_text(A, L, a.lin);
        if (a.masked) ix = "(" + ix + ")&m";
        ms.push_back(std::string(a.store ? "st" : "ld") + std::to_string(a.bytes) + "[" + ix + "]");
    }
    std::sort(ms.begin(), ms.end());
    ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
    for (std::size_t k = 0; k < ms.size(); ++k) o << (k ? " " : "") << ms[k];
    o << "]";
    return o.str();
}

// ---- matchers

struct Builder {
    const Ana& A;
    const Context& ctx;
    std::size_t L;
    std::vector<Match> out;

    TTerm phi(int i) const { return {TTerm::Phi, i, 0, A.width(A.phi_var(L, i))}; }
    TTerm entry(int i) const { return {TTerm::Entry, i, 0, A.width(A.phi_var(L, i))}; }
    TTerm cst(uint64_t c, unsigned w) const { return {TTerm::Const, -1, c & wmask(w), w}; }
    TTerm scope(int v) const { return {TTerm::Scope, v, 0, A.width(v)}; }
    bool sgn(int i) const { return ctx.signed_use && ctx.signed_use(A.phi_var(L, i)); }
    Match& m(const std::string& name) {
        for (auto& x : out)
            if (x.idiom == name) return x;
        out.push_back({name, {}});
        return out.back();
    }
    void cmp(const std::string& name, int i, Op r, TTerm b) {
        TCand c;
        c.kind = TCand::Cmp;
        c.loop = static_cast<int>(L);
        c.rel = r;
        c.a = phi(i);
        c.b = b;
        c.idiom = name;
        m(name).cands.push_back(c);
    }
    void scale(const std::string& name, int a, int b, int64_t k, Op r, bool sx) {
        TCand c;
        c.kind = TCand::Scale;
        c.loop = static_cast<int>(L);
        c.rel = r;
        c.a = phi(a);
        c.b = phi(b);
        c.mask = static_cast<uint64_t>(k) & wmask(c.a.w);
        c.sext = sx;
        c.idiom = name;
        m(name).cands.push_back(c);
    }
    // bounds of lin(P)+c compared with a constant or a scope value in the loop's guards
    std::vector<std::pair<Op, TTerm>> bounds(int i) const {
        std::vector<std::pair<Op, TTerm>> r;
        auto& F = A.facts(L);
        const int P = A.phi_var(L, i);
        const unsigned w = A.width(P);
        auto add_b = [&](Op op, const Lin& other, int64_t off) {
            if (other.leaves.empty()) {
                r.push_back({op, cst(static_cast<uint64_t>(as_signed(other.c, other.w) - off), w)});
            } else if (other.leaves.size() == 1 && other.leaves[0].second == 1) {  // P + c <= n + d: P <= n (weaker, Houdini decides)
                const int v = other.leaves[0].first;
                if (A.width(v) == w && ctx.scope && ctx.scope(static_cast<int>(L), v)) r.push_back({op, scope(v)});
            }
        };
        for (auto& g : F.guards) {
            const bool lp = g.l.leaves.size() == 1 && g.l.leaves[0] == std::make_pair(P, 1);
            const bool rp = g.r.leaves.size() == 1 && g.r.leaves[0] == std::make_pair(P, 1);
            if (lp) add_b(g.op, g.r, as_signed(g.l.c, g.l.w));
            if (rp) add_b(g.op, g.l, as_signed(g.r.c, g.r.w));
        }
        return r;
    }

    void run() {
        auto& F = A.facts(L);
        const int np = static_cast<int>(F.phis.size());
        auto only = [&](int i, std::initializer_list<Upd::K> ks) {
            auto& u = F.phis[static_cast<std::size_t>(i)].upd;
            if (u.empty()) return false;
            for (auto& x : u)
                if (std::find(ks.begin(), ks.end(), x.k) == ks.end()) return false;
            return true;
        };
        auto has = [&](int i, Upd::K k) {
            for (auto& x : F.phis[static_cast<std::size_t>(i)].upd)
                if (x.k == k) return true;
            return false;
        };
        auto scalar = [&](int i) {
            auto& var = A.fn.vars[static_cast<std::size_t>(A.phi_var(L, i))];
            return !var.fp && var.width > 1 && var.width <= 64;
        };
        auto unit_counter = [&](int i, int64_t step) {
            if (!only(i, {Upd::AddC})) return false;
            for (auto& x : F.phis[static_cast<std::size_t>(i)].upd)
                if (x.c != step) return false;
            return true;
        };
        // addend range of an AddV update (the leaves other than the phi)
        auto addend = [&](const Upd& u, unsigned w) -> std::optional<std::pair<i128, i128>> {
            i128 lo = as_signed(static_cast<uint64_t>(u.c), w), hi = lo;
            for (auto& [v, s] : u.vars) {
                auto r = A.interval(Arg::v(v, A.width(v)));
                if (!r) return std::nullopt;
                if (s > 0) {
                    lo += r->first;
                    hi += r->second;
                } else {
                    lo -= r->second;
                    hi -= r->first;
                }
            }
            return std::make_pair(lo, hi);
        };
        auto fits = [](i128 k, unsigned w) {
            return w >= 2 && k >= -(static_cast<i128>(1) << (w - 1)) && k < (static_cast<i128>(1) << (w - 1));
        };
        // monotone phis that grow by >= 1 every iteration: "positions"
        std::vector<int> positions, counters;
        for (int i = 0; i < np; ++i) {
            if (!scalar(i)) continue;
            if (unit_counter(i, 1)) {
                counters.push_back(i);
                positions.push_back(i);
                continue;
            }
            if (!only(i, {Upd::AddC, Upd::AddV})) continue;
            bool grows = true;
            for (auto& u : F.phis[static_cast<std::size_t>(i)].upd) {
                auto r = addend(u, A.width(A.phi_var(L, i)));
                if (!r || r->first < 1) grows = false;
            }
            if (grows) positions.push_back(i);
        }

        for (int i = 0; i < np; ++i) {
            if (!scalar(i)) continue;
            const unsigned w = A.width(A.phi_var(L, i));
            auto& pf = F.phis[static_cast<std::size_t>(i)];
            const bool s = sgn(i);
            // counters (unit steps of one sign), and monotone positions
            const bool up = only(i, {Upd::Same, Upd::AddC, Upd::AddV}) && has(i, Upd::AddC) + has(i, Upd::AddV) > 0 &&
                            [&] {
                                for (auto& u : pf.upd) {
                                    if (u.k == Upd::AddC && u.c < 0) return false;
                                    if (u.k == Upd::AddV) {
                                        auto r = addend(u, w);
                                        if (!r || r->first < 0) return false;
                                    }
                                }
                                return true;
                            }();
            const bool down = only(i, {Upd::Same, Upd::AddC}) && [&] {
                for (auto& u : pf.upd)
                    if (u.k == Upd::AddC && u.c >= 0) return false;
                return has(i, Upd::AddC);
            }();
            const bool updown = !up && !down && only(i, {Upd::Same, Upd::AddC}) && [&] {
                for (auto& u : pf.upd)
                    if (u.k == Upd::AddC && (u.c > 1 || u.c < -1)) return false;
                return true;
            }();
            if (up || down || updown) {
                const std::string name = up ? (has(i, Upd::AddV) ? "position" : "counter") : down ? "countdown" : "updown-count";
                if (up) cmp(name, i, s ? Op::Sge : Op::Uge, entry(i));
                if (down) cmp(name, i, s ? Op::Sle : Op::Ule, entry(i));
                if (s) cmp(name, i, Op::Sge, cst(0, w));
                for (auto& [op, b] : bounds(i)) {
                    // P < K / P <= K / P != K / P > K ...: the closed bound on the side the loop stays in
                    if (up || updown) {
                        cmp(name, i, s ? Op::Sle : Op::Ule, b);
                        if (b.kind == TTerm::Scope) {
                            TCand c;
                            c.kind = TCand::MaxLe;
                            c.loop = static_cast<int>(L);
                            c.a = phi(i);
                            c.b = b;
                            c.sext = s;
                            c.idiom = name;
                            m(name).cands.push_back(c);
                        }
                    }
                    if (down || updown) cmp(name, i, s ? Op::Sge : Op::Uge, b);
                    (void)op;
                }
            }
            // masked / modular ring index
            if (only(i, {Upd::Same, Upd::MaskAdd, Upd::SetC}) && has(i, Upd::MaskAdd)) {
                uint64_t mm = 0;
                for (auto& u : pf.upd)
                    if (u.k == Upd::MaskAdd) mm = std::max(mm, u.m);
                cmp("masked-index", i, Op::Ule, cst(mm, w));
            }
            if (only(i, {Upd::Same, Upd::ModAdd, Upd::SetC}) && has(i, Upd::ModAdd)) {
                uint64_t mm = 0;
                for (auto& u : pf.upd)
                    if (u.k == Upd::ModAdd) mm = std::max(mm, u.m);
                cmp("mod-index", i, Op::Ult, cst(mm, w));
            }
            // state variable: assigned constants only
            if (only(i, {Upd::Same, Upd::SetC}) && has(i, Upd::SetC)) {
                int64_t lo = INT64_MAX, hi = INT64_MIN;
                for (auto& u : pf.upd)
                    if (u.k == Upd::SetC) {
                        lo = std::min(lo, u.c);
                        hi = std::max(hi, u.c);
                    }
                if (pf.entry_const) {
                    lo = std::min(lo, as_signed(*pf.entry_const, w));
                    hi = std::max(hi, as_signed(*pf.entry_const, w));
                }
                if (lo >= 0) {
                    cmp("state", i, Op::Ule, cst(static_cast<uint64_t>(hi), w));
                    cmp("state", i, Op::Uge, cst(static_cast<uint64_t>(lo), w));
                } else {
                    cmp("state", i, Op::Sle, cst(static_cast<uint64_t>(hi), w));
                    cmp("state", i, Op::Sge, cst(static_cast<uint64_t>(lo), w));
                }
            }
            // bit scan: x >>= c
            if (only(i, {Upd::Same, Upd::Shift}) && has(i, Upd::Shift)) {
                bool r = true;
                for (auto& u : pf.upd)
                    if (u.k == Upd::Shift && u.op != Op::LShr) r = false;
                if (r) cmp("bit-scan", i, Op::Ule, entry(i));
            }
            // folds: acc += x with x in [lo, hi], once per iteration of a counter / position
            if (only(i, {Upd::Same, Upd::AddV, Upd::AddC}) && has(i, Upd::AddV) &&
                std::find(positions.begin(), positions.end(), i) == positions.end()) {
                std::optional<std::pair<i128, i128>> r;
                std::optional<int> single_scope;
                for (auto& u : pf.upd) {
                    if (u.k == Upd::Same) continue;
                    auto x = addend(u, w);
                    if (u.k == Upd::AddV && u.c == 0 && u.vars.size() == 1 && u.vars[0].second == 1 && ctx.scope &&
                        ctx.scope(static_cast<int>(L), u.vars[0].first))
                        single_scope = u.vars[0].first;
                    if (!x) {
                        r.reset();
                        break;
                    }
                    r = r ? std::make_pair(std::min(r->first, x->first), std::max(r->second, x->second)) : *x;
                }
                if (has(i, Upd::Same) && r) r = std::make_pair(std::min<i128>(r->first, 0), std::max<i128>(r->second, 0));
                for (int b : positions) {
                    if (b == i) continue;
                    const bool sx = s || (r && r->first < 0);
                    if (r) {
                        const i128 hi = std::max<i128>(r->second, 0), lo = std::min<i128>(r->first, 0);
                        if (fits(hi, w)) scale("fold", i, b, static_cast<int64_t>(hi), Op::Sle, sx);
                        if (fits(lo, w)) scale("fold", i, b, static_cast<int64_t>(lo), Op::Sge, sx);
                    }
                    if (!r && single_scope && std::find(counters.begin(), counters.end(), b) != counters.end()) {
                        TCand c;
                        c.kind = TCand::ScaleV;
                        c.loop = static_cast<int>(L);
                        c.a = phi(i);
                        c.b = phi(b);
                        c.v = scope(*single_scope);
                        c.sext = s;
                        c.idiom = "fold";
                        m("fold").cands.push_back(c);
                    }
                }
            }
            // nested fold: acc updated by an inner loop that adds x per inner iteration
            if (only(i, {Upd::Same, Upd::Inner}) && has(i, Upd::Inner)) {
                for (auto& u : pf.upd) {
                    if (u.k != Upd::Inner) continue;
                    const auto M = static_cast<std::size_t>(u.inner);
                    auto& FM = A.facts(M);
                    auto& ipf = FM.phis[static_cast<std::size_t>(u.inner_phi)];
                    if (ipf.entry_var != A.phi_var(L, i)) continue;
                    // the inner phi's additions
                    std::optional<std::pair<i128, i128>> r;
                    std::optional<int> sv;
                    bool okk = !ipf.upd.empty();
                    for (auto& iu : ipf.upd) {
                        if (iu.k == Upd::Same) continue;
                        if (iu.k != Upd::AddV && iu.k != Upd::AddC) {
                            okk = false;
                            break;
                        }
                        if (iu.k == Upd::AddV && iu.c == 0 && iu.vars.size() == 1 && iu.vars[0].second == 1 &&
                            ctx.scope && ctx.scope(static_cast<int>(L), iu.vars[0].first))
                            sv = iu.vars[0].first;
                        i128 lo = as_signed(static_cast<uint64_t>(iu.c), w), hi = lo;
                        bool known = true;
                        for (auto& [v, sg] : iu.vars) {
                            auto x = A.interval(Arg::v(v, A.width(v)));
                            if (!x) {
                                known = false;
                                break;
                            }
                            if (sg > 0) {
                                lo += x->first;
                                hi += x->second;
                            } else {
                                lo -= x->second;
                                hi -= x->first;
                            }
                        }
                        if (!known) {
                            r.reset();
                            okk = okk && sv.has_value();
                            break;
                        }
                        r = r ? std::make_pair(std::min(r->first, lo), std::max(r->second, hi)) : std::make_pair(lo, hi);
                    }
                    if (!okk) continue;
                    auto trip = A.trip_count(M);
                    // no constant trip count: the inner loop is taken to run at
                    // most as far as a position of this loop moves (TLV walk:
                    // sum <= 255 * off); bounds per unit of the position
                    if (!trip && r)
                        for (int b : positions) {
                            if (std::find(counters.begin(), counters.end(), b) != counters.end()) continue;
                            const i128 hi = std::max<i128>(r->second, 0), lo = std::min<i128>(r->first, 0);
                            const bool sx = s || lo < 0;
                            if (fits(hi, w)) scale("nested-fold", i, b, static_cast<int64_t>(hi), Op::Sle, sx);
                            if (fits(lo, w)) scale("nested-fold", i, b, static_cast<int64_t>(lo), Op::Sge, sx);
                        }
                    if (!trip || *trip <= 0) continue;
                    for (int b : counters) {
                        if (r) {
                            const i128 hi = std::max<i128>(r->second, 0) * *trip, lo = std::min<i128>(r->first, 0) * *trip;
                            const bool sx = s || lo < 0;
                            if (fits(hi, w)) scale("nested-fold", i, b, static_cast<int64_t>(hi), Op::Sle, sx);
                            if (fits(lo, w)) scale("nested-fold", i, b, static_cast<int64_t>(lo), Op::Sge, sx);
                        }
                        if (!r && sv && A.width(*sv) == w) {  // nonlinear: only without a range
                            TCand c;
                            c.kind = TCand::ScaleVK;
                            c.loop = static_cast<int>(L);
                            c.rel = Op::Eq;
                            c.a = phi(i);
                            c.b = phi(b);
                            c.v = scope(*sv);
                            c.mask = static_cast<uint64_t>(*trip);
                            c.sext = s || sgn(b);
                            c.idiom = "nested-fold";
                            m("nested-fold").cands.push_back(c);
                        }
                    }
                }
            }
        }
        // a counter bounded by a position (recs <= off)
        for (int c : counters)
            for (int p : positions)
                if (p != c && A.width(A.phi_var(L, p)) == A.width(A.phi_var(L, c)))
                    scale("position", c, p, 1, Op::Sle, sgn(c) || sgn(p));

        // free-running ring counters: head - tail compared with a constant
        for (auto& g : F.guards) {
            for (const Lin* side : {&g.l, &g.r}) {
                const Lin& other = side == &g.l ? g.r : g.l;
                if (side->leaves.size() != 2 || !other.leaves.empty()) continue;
                int a = -1, b = -1;
                for (auto& [v, sg] : side->leaves) {
                    const int k = A.hphi(L, v);
                    if (k < 0) continue;
                    (sg > 0 ? a : b) = k;
                }
                if (a < 0 || b < 0 || !scalar(a) || A.width(A.phi_var(L, a)) != A.width(A.phi_var(L, b))) continue;
                if (!only(a, {Upd::Same, Upd::AddC}) || !only(b, {Upd::Same, Upd::AddC})) continue;
                const unsigned w = A.width(A.phi_var(L, a));
                const uint64_t K = (other.c - side->c) & wmask(w);
                TCand c;
                c.kind = TCand::DiffCmp;
                c.loop = static_cast<int>(L);
                c.rel = Op::Ule;
                c.a = phi(a);
                c.b = phi(b);
                c.v = cst(K, w);
                c.idiom = "ring-counters";
                m("ring-counters").cands.push_back(c);
            }
        }

        // ring with an occupancy count: head, tail (masked/modular), count (+1/-1)
        for (int h = 0; h < np; ++h) {
            if (!scalar(h)) continue;
            auto ring_step = [&](int i, uint64_t& cap) {
                if (!only(i, {Upd::Same, Upd::MaskAdd, Upd::ModAdd})) return false;
                bool any = false;
                for (auto& u : F.phis[static_cast<std::size_t>(i)].upd) {
                    if (u.k == Upd::Same) continue;
                    if (u.c != 1) return false;
                    const uint64_t k = u.k == Upd::MaskAdd ? u.m + 1 : u.m;
                    if (cap && cap != k) return false;
                    cap = k;
                    any = true;
                }
                return any && pow2(cap);
            };
            uint64_t cap = 0;
            if (!ring_step(h, cap)) continue;
            // a store at p[head]
            for (auto& st : F.acc) {
                if (!st.store || st.idx < 0 || A.hphi(L, st.idx) != h || st.base < 0 || st.eb != st.bytes) continue;
                for (int t = 0; t < np; ++t) {
                    uint64_t cap2 = cap;
                    if (t == h || !ring_step(t, cap2) || cap2 != cap) continue;
                    bool loads_tail = false;
                    for (auto& ld : F.acc)
                        if (!ld.store && ld.idx >= 0 && A.hphi(L, ld.idx) == t && ld.base == st.base && ld.eb == st.eb)
                            loads_tail = true;
                    if (!loads_tail) continue;
                    for (int n = 0; n < np; ++n) {
                        if (n == h || n == t || !scalar(n) || !only(n, {Upd::Same, Upd::AddC})) continue;
                        bool unit = true;
                        for (auto& u : F.phis[static_cast<std::size_t>(n)].upd)
                            if (u.k == Upd::AddC && u.c != 1 && u.c != -1) unit = false;
                        if (!unit) continue;
                        const unsigned w = A.width(A.phi_var(L, n));
                        if (A.width(A.phi_var(L, h)) != w || A.width(A.phi_var(L, t)) != w) continue;
                        auto& mt = m("ring-occupancy");
                        TCand c;
                        c.loop = static_cast<int>(L);
                        c.idiom = "ring-occupancy";
                        c.kind = TCand::Cmp;
                        c.rel = Op::Ule;
                        c.a = phi(n);
                        c.b = cst(cap, w);
                        mt.cands.push_back(c);
                        c.kind = TCand::ModLin;
                        c.a = phi(h);
                        c.b = phi(t);
                        c.v = phi(n);
                        c.mask = cap - 1;
                        mt.cands.push_back(c);
                        c = TCand{};
                        c.loop = static_cast<int>(L);
                        c.idiom = "ring-occupancy";
                        c.kind = TCand::MemRingInit;
                        c.p = st.base;
                        c.ip = t;
                        c.ip2 = n;
                        c.mask = cap;
                        c.eb = st.eb;
                        mt.cands.push_back(c);
                    }
                }
            }
        }

        // counted fill: p[i] written by a unit counter i
        for (auto& st : F.acc) {
            if (!st.store || st.idx < 0 || st.base < 0 || st.masked) continue;
            const int i = A.hphi(L, st.idx);
            if (i < 0 || !unit_counter(i, 1) || st.eb != st.bytes) continue;
            if (!(st.eb == 1 || st.eb == 2 || st.eb == 4 || st.eb == 8)) continue;
            TCand c;
            c.loop = static_cast<int>(L);
            c.idiom = "counted-fill";
            c.kind = TCand::MemInit;
            c.p = st.base;
            c.ip = i;
            c.sext = st.sext;
            c.eb = st.eb;
            m("counted-fill").cands.push_back(c);
            if (st.stores_const) {
                c.kind = TCand::MemConst;
                c.v = cst(st.stored_const, 8 * st.eb);
                m("counted-fill").cands.push_back(c);
            }
        }
        // drop idioms whose templates all failed to instantiate
        out.erase(std::remove_if(out.begin(), out.end(), [](const Match& x) { return x.cands.empty(); }), out.end());
    }
};

std::vector<std::vector<char>> dominators(const Function& fn, const std::vector<std::vector<int>>& preds) {
    const std::size_t n = fn.blocks.size();
    std::vector<std::vector<char>> dom(n, std::vector<char>(n, 1));
    if (n == 0) return dom;
    dom[0].assign(n, 0);
    dom[0][0] = 1;
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t b = 1; b < n; ++b) {
            std::vector<char> d(n, 1);
            bool any = false;
            for (int p : preds[b]) {
                any = true;
                for (std::size_t k = 0; k < n; ++k) d[k] = d[k] && dom[static_cast<std::size_t>(p)][k];
            }
            if (!any) d.assign(n, 0);
            d[b] = 1;
            if (d != dom[b]) {
                dom[b] = std::move(d);
                changed = true;
            }
        }
    }
    return dom;
}

std::vector<std::vector<char>> Ana::dominators_of() const { return dominators(fn, preds); }

}  // namespace

std::vector<LoopView> find_loops(const Function& fn) {
    const std::vector<LoopView> none;
    Ana A(fn, none);
    auto dom = dominators(fn, A.preds);
    std::map<int, LoopView> by_header;
    for (std::size_t b = 0; b < fn.blocks.size(); ++b)
        for (int h : {fn.blocks[b].term.t, fn.blocks[b].term.f}) {
            if (h < 0 || static_cast<std::size_t>(h) >= fn.blocks.size()) continue;
            if (fn.blocks[b].term.kind == Term::Jmp && h != fn.blocks[b].term.t) continue;
            if (fn.blocks[b].term.kind != Term::Jmp && fn.blocks[b].term.kind != Term::Br) continue;
            if (!dom[b][static_cast<std::size_t>(h)]) continue;
            auto& lv = by_header[h];
            lv.header = h;
            lv.body.resize(fn.blocks.size(), 0);
            lv.body[static_cast<std::size_t>(h)] = 1;
            std::vector<int> st{static_cast<int>(b)};
            while (!st.empty()) {
                const int x = st.back();
                st.pop_back();
                if (lv.body[static_cast<std::size_t>(x)]) continue;
                lv.body[static_cast<std::size_t>(x)] = 1;
                for (int p : A.preds[static_cast<std::size_t>(x)]) st.push_back(p);
            }
        }
    std::vector<LoopView> r;
    for (auto& [h, lv] : by_header) r.push_back(std::move(lv));
    return r;
}

Context default_context(const Function& fn, const std::vector<LoopView>& loops) {
    auto A = std::make_shared<Ana>(fn, loops);
    auto dom = std::make_shared<std::vector<std::vector<char>>>(dominators(fn, A->preds));
    auto sgn = std::make_shared<std::vector<char>>(fn.vars.size(), 0);
    for (auto& b : fn.blocks)
        for (auto& st : b.stmts) {
            if (st.kind != Stmt::Assign) continue;
            switch (st.op) {
                case Op::Slt: case Op::Sle: case Op::Sgt: case Op::Sge: case Op::SAddOvf: case Op::SSubOvf:
                case Op::SMulOvf: case Op::SDiv: case Op::SRem: case Op::SDivOvf: case Op::AShr: case Op::SExt:
                case Op::SMax: case Op::SMin: case Op::ShlSOvf: case Op::ShlNswOvf:
                    for (auto& a : st.args)
                        if (!a.is_const) (*sgn)[static_cast<std::size_t>(a.var)] = 1;
                    break;
                default: break;
            }
        }
    Context c;
    c.scope = [A, dom, &fn](int L, int v) {
        if (v < 0) return false;
        if (std::find(fn.params.begin(), fn.params.end(), v) != fn.params.end()) return true;
        const int d = A->def_block[static_cast<std::size_t>(v)];
        const auto Lz = static_cast<std::size_t>(L);
        if (d < 0 || A->in(Lz, d)) return false;
        const int H = A->header(Lz);
        if (!(*dom)[static_cast<std::size_t>(H)][static_cast<std::size_t>(d)]) return false;
        for (std::size_t M = 0; M < A->loops.size(); ++M)
            if (A->in(M, d) && !A->in(M, H)) return false;
        return true;
    };
    c.signed_use = [sgn](int v) { return v >= 0 && static_cast<std::size_t>(v) < sgn->size() && (*sgn)[static_cast<std::size_t>(v)]; };
    return c;
}

std::string loop_shape(const Function& fn, const std::vector<LoopView>& loops, std::size_t L) {
    Ana A(fn, loops);
    return shape_text(A, L);
}

std::vector<Match> match_loop(const Function& fn, const std::vector<LoopView>& loops, std::size_t L,
                              const Context& ctx) {
    Ana A(fn, loops);
    Builder b{A, ctx, L, {}};
    b.run();
    return std::move(b.out);
}

namespace {
std::string key(const TCand& c) {
    std::ostringstream o;
    auto t = [&](const TTerm& x) { o << static_cast<int>(x.kind) << ':' << x.idx << ':' << x.c << ':' << x.w << ' '; };
    o << static_cast<int>(c.kind) << ' ' << c.loop << ' ' << static_cast<int>(c.rel) << ' ';
    t(c.a);
    t(c.b);
    t(c.v);
    o << c.mask << ' ' << c.sext << ' ' << c.p << ' ' << c.ip << ' ' << c.ip2 << ' ' << c.eb;
    return o.str();
}
}  // namespace

std::vector<TCand> instantiate(const Function& fn, const std::vector<LoopView>& loops, const Context& ctx,
                               std::vector<std::string>* matched) {
    Ana A(fn, loops);
    std::vector<TCand> out;
    std::set<std::string> seen;
    std::set<std::string> names;
    for (std::size_t L = 0; L < loops.size(); ++L) {
        Builder b{A, ctx, L, {}};
        b.run();
        for (auto& mt : b.out) {
            names.insert(mt.idiom);
            for (auto& c : mt.cands)
                if (seen.insert(key(c)).second) out.push_back(c);
        }
    }
    if (matched) matched->assign(names.begin(), names.end());
    return out;
}

std::string loop_shapes_json(const Function& fn) {
    auto loops = find_loops(fn);
    auto ctx = default_context(fn, loops);
    Ana A(fn, loops);
    nlohmann::json j = nlohmann::json::array();
    for (std::size_t L = 0; L < loops.size(); ++L) {
        Builder b{A, ctx, L, {}};
        b.run();
        nlohmann::json names = nlohmann::json::array();
        for (auto& mt : b.out) names.push_back(mt.idiom);
        int line = 0;
        if (auto it = fn.loop_locs.find(loops[L].header); it != fn.loop_locs.end()) line = it->second.first;
        j.push_back({{"header", loops[L].header}, {"line", line}, {"shape", shape_text(A, L)}, {"idioms", names}});
    }
    return j.dump();
}

const std::vector<std::pair<std::string, std::string>>& catalogue() {
    static const std::vector<std::pair<std::string, std::string>> k = {
        {"counter", "unit up-counter: i >= entry(i), i <= its guard bound (constant, or max(entry, n) for a scope n)"},
        {"countdown", "down-counter: n <= entry(n), n >= its guard bound"},
        {"updown-count", "occupancy count moved by +1/-1: bounded by the constants it is compared with"},
        {"position", "offset grown by a non-negative step (TLV walk): off >= entry, off <= len; a counter <= position"},
        {"masked-index", "ring index (i + c) & m: i <= m"},
        {"mod-index", "ring index (i + c) % m: i < m"},
        {"state", "state variable assigned constants only: min <= s <= max"},
        {"bit-scan", "x >>= c: x <= entry(x)"},
        {"fold", "acc += x, x in [lo, hi]: lo*(i - entry(i)) <= acc - entry(acc) <= hi*(i - entry(i)); acc += v: == v*(i - entry(i))"},
        {"nested-fold", "acc updated by an inner loop of N iterations: bounds N*lo, N*hi per outer step; acc += v: == N*v*(i - entry(i))"},
        {"ring-counters", "free-running head/tail compared by difference: head - tail <= cap (wrapping)"},
        {"ring-occupancy", "head/tail masked ring with a count: count <= cap, head - tail - count == const (mod cap), "
                           "slots [tail, tail + count) initialised"},
        {"counted-fill", "p[i] written by a unit counter: forall x < i: p[x] initialised (== v for a constant v)"},
    };
    return k;
}

}  // namespace prism::pir::idioms
