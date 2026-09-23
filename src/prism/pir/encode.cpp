// PIR -> Z3 bitvector formulas.
//
// Bounded unrolling over the loop nest: every block is instantiated once per
// vector of iteration counts of the loops that contain it (outer -> inner).
// A back edge increments its loop's count; reaching count == unwind is an
// unwinding cut. Values follow SSA dominance (LCSSA guarantees that a use
// sees its definition in the same iteration of every shared loop).
//
// Verdicts (never merged, Law 2):
//   FAILED            some check is satisfiable (concrete parameter values)
//   PROVED            no check fails and no path reaches an unwinding cut
//                     (loop-free, or every loop closed within the bound)
//   BOUNDED           no check fails within the bound; a cut is reachable
//   PROVED-UNBOUNDED  BOUNDED + a single loop whose k-induction step closes
//   UNKNOWN           solver unknown / timeout / unrolling too large

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#ifdef PRISM_HAS_Z3
#  include <z3++.h>
#endif

namespace prism::pir {

namespace {

uint64_t wmask(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }

int64_t as_signed(uint64_t v, unsigned w) {
    if (w == 0) return 0;
    if (w >= 64) return static_cast<int64_t>(v);
    uint64_t sign = uint64_t{1} << (w - 1);
    v &= wmask(w);
    return static_cast<int64_t>((v ^ sign) - sign);
}

struct Loop {
    int header = -1;
    std::vector<char> body;
    int size = 0;
};

struct CfgInfo {
    int n = 0;
    std::vector<std::vector<int>> succ;
    std::vector<char> reachable;
    std::vector<std::vector<char>> dom;  // dom[b][d]: d dominates b
    std::vector<Loop> loops;
    std::vector<std::vector<int>> loops_of;  // outer -> inner
    std::vector<int> def_block;              // per var; -1 = parameter / undefined
    std::string unencoded;
};

std::vector<int> succs(const Block& b) {
    switch (b.term.kind) {
        case Term::Jmp: return {b.term.t};
        case Term::Br: return {b.term.t, b.term.f};
        default: return {};
    }
}

CfgInfo analyze(const Function& fn) {
    CfgInfo g;
    g.n = static_cast<int>(fn.blocks.size());
    g.succ.resize(static_cast<std::size_t>(g.n));
    for (int b = 0; b < g.n; ++b) g.succ[static_cast<std::size_t>(b)] = succs(fn.blocks[static_cast<std::size_t>(b)]);
    g.reachable.assign(static_cast<std::size_t>(g.n), 0);
    // DFS (iterative) for reachability and retreating edges
    std::vector<int> state(static_cast<std::size_t>(g.n), 0);  // 0 new, 1 on stack, 2 done
    std::vector<std::pair<int, int>> retreating;
    if (g.n > 0) {
        std::vector<std::pair<int, std::size_t>> st{{0, 0}};
        state[0] = 1;
        g.reachable[0] = 1;
        while (!st.empty()) {
            auto& [b, k] = st.back();
            auto& ss = g.succ[static_cast<std::size_t>(b)];
            if (k < ss.size()) {
                int s = ss[k++];
                if (state[static_cast<std::size_t>(s)] == 0) {
                    state[static_cast<std::size_t>(s)] = 1;
                    g.reachable[static_cast<std::size_t>(s)] = 1;
                    st.emplace_back(s, 0);
                } else if (state[static_cast<std::size_t>(s)] == 1) {
                    retreating.emplace_back(b, s);
                }
            } else {
                state[static_cast<std::size_t>(b)] = 2;
                st.pop_back();
            }
        }
    }
    // dominators (iterative data flow over reachable blocks)
    std::vector<std::vector<int>> preds(static_cast<std::size_t>(g.n));
    for (int b = 0; b < g.n; ++b)
        if (g.reachable[static_cast<std::size_t>(b)])
            for (int s : g.succ[static_cast<std::size_t>(b)]) preds[static_cast<std::size_t>(s)].push_back(b);
    g.dom.assign(static_cast<std::size_t>(g.n), std::vector<char>(static_cast<std::size_t>(g.n), 1));
    if (g.n > 0) {
        g.dom[0].assign(static_cast<std::size_t>(g.n), 0);
        g.dom[0][0] = 1;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < g.n; ++b) {
            if (!g.reachable[static_cast<std::size_t>(b)]) continue;
            std::vector<char> nd(static_cast<std::size_t>(g.n), 1);
            bool any = false;
            for (int p : preds[static_cast<std::size_t>(b)]) {
                any = true;
                for (int d = 0; d < g.n; ++d)
                    nd[static_cast<std::size_t>(d)] = static_cast<char>(nd[static_cast<std::size_t>(d)] &&
                                                                        g.dom[static_cast<std::size_t>(p)][static_cast<std::size_t>(d)]);
            }
            if (!any) nd.assign(static_cast<std::size_t>(g.n), 0);
            nd[static_cast<std::size_t>(b)] = 1;
            if (nd != g.dom[static_cast<std::size_t>(b)]) {
                g.dom[static_cast<std::size_t>(b)] = std::move(nd);
                changed = true;
            }
        }
    }
    // back edges; a retreating edge that is not a back edge = irreducible
    std::map<int, std::vector<int>> latches;
    for (auto [u, h] : retreating) {
        if (!g.dom[static_cast<std::size_t>(u)][static_cast<std::size_t>(h)]) {
            g.unencoded = "UNENCODED: irreducible control flow";
            return g;
        }
    }
    for (int u = 0; u < g.n; ++u) {
        if (!g.reachable[static_cast<std::size_t>(u)]) continue;
        for (int h : g.succ[static_cast<std::size_t>(u)])
            if (g.dom[static_cast<std::size_t>(u)][static_cast<std::size_t>(h)]) latches[h].push_back(u);
    }
    for (auto& [h, ls] : latches) {
        Loop L;
        L.header = h;
        L.body.assign(static_cast<std::size_t>(g.n), 0);
        L.body[static_cast<std::size_t>(h)] = 1;
        std::vector<int> work;
        for (int l : ls)
            if (!L.body[static_cast<std::size_t>(l)]) {
                L.body[static_cast<std::size_t>(l)] = 1;
                work.push_back(l);
            }
        while (!work.empty()) {
            int x = work.back();
            work.pop_back();
            for (int p : preds[static_cast<std::size_t>(x)])
                if (!L.body[static_cast<std::size_t>(p)]) {
                    L.body[static_cast<std::size_t>(p)] = 1;
                    work.push_back(p);
                }
        }
        L.size = static_cast<int>(std::count(L.body.begin(), L.body.end(), 1));
        g.loops.push_back(std::move(L));
    }
    std::vector<int> order(g.loops.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return g.loops[static_cast<std::size_t>(a)].size > g.loops[static_cast<std::size_t>(b)].size;
    });
    g.loops_of.assign(static_cast<std::size_t>(g.n), {});
    for (int b = 0; b < g.n; ++b)
        for (int li : order)
            if (g.loops[static_cast<std::size_t>(li)].body[static_cast<std::size_t>(b)])
                g.loops_of[static_cast<std::size_t>(b)].push_back(li);
    // definitions
    g.def_block.assign(fn.vars.size(), -1);
    for (int b = 0; b < g.n; ++b) {
        auto& bl = fn.blocks[static_cast<std::size_t>(b)];
        for (auto& p : bl.phis) g.def_block[static_cast<std::size_t>(p.dst)] = b;
        for (auto& s : bl.stmts)
            if (s.kind == Stmt::Assign) g.def_block[static_cast<std::size_t>(s.dst)] = b;
    }
    return g;
}

}  // namespace

std::string format_cex(const Function& fn, const std::vector<uint64_t>& args) {
    std::string out;
    for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        auto& v = fn.vars[static_cast<std::size_t>(fn.params[i])];
        if (i) out += ", ";
        out += v.name + "=" + std::to_string(as_signed(args[i], v.width));
    }
    return out;
}

#ifdef PRISM_HAS_Z3

bool z3_available() { return true; }

namespace {

constexpr int kMaxInstances = 6000;

struct Node {
    int block;
    std::vector<int> ctx;
};

struct PropInst {
    const Stmt* stmt;
    int node;
    z3::expr viol;
};

struct EncodeFail {
    std::string status;
    std::string msg;
};

struct Encoding {
    z3::context& c;
    const Function& fn;
    const CfgInfo& g;
    int unwind;
    int step_loop = -1;  // k-induction: havoc this loop's header phis at count 0

    std::vector<Node> nodes;
    std::map<std::pair<int, std::vector<int>>, int> ids;
    std::vector<std::vector<std::tuple<int, int, int>>> in_edges;  // (from node, succ idx, to)
    std::vector<z3::expr> reach;
    std::vector<std::unordered_map<int, z3::expr>> vals;
    std::vector<z3::expr> params;
    std::vector<PropInst> props;
    std::vector<z3::expr> assumptions;
    std::vector<z3::expr> cuts;
    int fresh = 0;

    Encoding(z3::context& cc, const Function& f, const CfgInfo& gg, int k)
        : c(cc), fn(f), g(gg), unwind(k) {}

    z3::expr bv(uint64_t v, unsigned w) { return c.bv_val(static_cast<uint64_t>(v & wmask(w)), w); }
    z3::expr b2bv(const z3::expr& b) { return z3::ite(b, c.bv_val(1, 1), c.bv_val(0, 1)); }
    z3::expr is1(const z3::expr& e) { return e == c.bv_val(1, 1); }

    int node_id(int block, const std::vector<int>& ctx) {
        auto key = std::make_pair(block, ctx);
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        if (static_cast<int>(nodes.size()) >= kMaxInstances)
            throw EncodeFail{std::string(laws::UNKNOWN),
                             "unrolling exceeds " + std::to_string(kMaxInstances) + " block instances"};
        int id = static_cast<int>(nodes.size());
        nodes.push_back(Node{block, ctx});
        ids.emplace(key, id);
        in_edges.emplace_back();
        return id;
    }

    // Context of successor s entered from node (b, ctx); nullopt: unwinding cut.
    std::optional<std::vector<int>> succ_ctx(int b, const std::vector<int>& ctx, int s) {
        const auto& Lb = g.loops_of[static_cast<std::size_t>(b)];
        const auto& Ls = g.loops_of[static_cast<std::size_t>(s)];
        std::vector<int> out;
        for (int L : Ls) {
            auto pos = std::find(Lb.begin(), Lb.end(), L);
            int cnt;
            if (pos != Lb.end()) {
                cnt = ctx[static_cast<std::size_t>(pos - Lb.begin())];
                if (s == g.loops[static_cast<std::size_t>(L)].header) ++cnt;  // back edge
            } else {
                if (s != g.loops[static_cast<std::size_t>(L)].header)
                    throw EncodeFail{std::string(laws::NEEDS_HARNESS), "UNENCODED: irreducible control flow"};
                cnt = 0;
            }
            if (cnt >= unwind) return std::nullopt;
            out.push_back(cnt);
        }
        return out;
    }

    void build() {
        std::deque<int> q;
        q.push_back(node_id(0, {}));
        std::vector<std::tuple<int, int, int>> edges;  // from, succ idx, to (-1 cut)
        std::set<int> seen{0};
        while (!q.empty()) {
            int id = q.front();
            q.pop_front();
            auto nb = nodes[static_cast<std::size_t>(id)];
            const auto& ss = g.succ[static_cast<std::size_t>(nb.block)];
            for (std::size_t k = 0; k < ss.size(); ++k) {
                auto ctx = succ_ctx(nb.block, nb.ctx, ss[k]);
                if (!ctx) {
                    edges.emplace_back(id, static_cast<int>(k), -1);
                    continue;
                }
                int to = node_id(ss[k], *ctx);
                edges.emplace_back(id, static_cast<int>(k), to);
                if (seen.insert(to).second) q.push_back(to);
            }
        }
        // topological order (Kahn)
        std::vector<int> indeg(nodes.size(), 0);
        std::vector<std::vector<std::tuple<int, int, int>>> out_edges(nodes.size());
        for (auto& e : edges) {
            auto [from, k, to] = e;
            out_edges[static_cast<std::size_t>(from)].push_back(e);
            if (to >= 0) {
                ++indeg[static_cast<std::size_t>(to)];
                in_edges[static_cast<std::size_t>(to)].push_back(e);
            }
        }
        std::vector<int> order;
        std::deque<int> ready;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (indeg[i] == 0) ready.push_back(static_cast<int>(i));
        while (!ready.empty()) {
            int x = ready.front();
            ready.pop_front();
            order.push_back(x);
            for (auto& [from, k, to] : out_edges[static_cast<std::size_t>(x)])
                if (to >= 0 && --indeg[static_cast<std::size_t>(to)] == 0) ready.push_back(to);
        }
        if (order.size() != nodes.size())
            throw EncodeFail{std::string(laws::ERROR), "internal: unrolled graph is not acyclic"};

        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            auto& v = fn.vars[static_cast<std::size_t>(fn.params[i])];
            params.push_back(c.bv_const(v.name.c_str(), v.width));
        }
        reach.assign(nodes.size(), c.bool_val(false));
        vals.assign(nodes.size(), {});
        for (int id : order) encode_node(id, out_edges[static_cast<std::size_t>(id)]);
    }

    z3::expr lookup(const Arg& a, int node) {
        if (a.is_const) return bv(a.bits, a.width);
        for (std::size_t i = 0; i < fn.params.size(); ++i)
            if (fn.params[i] == a.var) return params[i];
        int d = g.def_block[static_cast<std::size_t>(a.var)];
        if (d < 0) throw EncodeFail{std::string(laws::ERROR), "internal: use of undefined PIR variable"};
        auto& nd = nodes[static_cast<std::size_t>(node)];
        int target = node;
        if (d != nd.block) {
            const auto& Ld = g.loops_of[static_cast<std::size_t>(d)];
            const auto& Lb = g.loops_of[static_cast<std::size_t>(nd.block)];
            if (Ld.size() > Lb.size() || !std::equal(Ld.begin(), Ld.end(), Lb.begin()))
                throw EncodeFail{std::string(laws::NEEDS_HARNESS),
                                 "UNENCODED: value used outside its loop (not LCSSA)"};
            std::vector<int> ctx(nd.ctx.begin(), nd.ctx.begin() + static_cast<std::ptrdiff_t>(Ld.size()));
            auto it = ids.find({d, ctx});
            if (it == ids.end())
                throw EncodeFail{std::string(laws::ERROR), "internal: definition instance missing"};
            target = it->second;
        }
        auto& m = vals[static_cast<std::size_t>(target)];
        auto it = m.find(a.var);
        if (it == m.end()) throw EncodeFail{std::string(laws::ERROR), "internal: value used before definition"};
        return it->second;
    }

    z3::expr fresh_const(const std::string& base, unsigned w) {
        return c.bv_const((base + "!" + std::to_string(fresh++)).c_str(), w);
    }

    z3::expr op_expr(const Stmt& s, int node) {
        unsigned w = fn.vars[static_cast<std::size_t>(s.dst)].width;
        std::vector<z3::expr> a;
        for (auto& x : s.args) a.push_back(lookup(x, node));
        auto aw = [&](std::size_t i) { return s.args[i].width; };
        switch (s.op) {
            case Op::Copy: return a[0];
            case Op::Havoc: return fresh_const(s.uninit ? "uninit" : s.nondet ? "nondet" : "havoc", w);
            case Op::Add: return a[0] + a[1];
            case Op::Sub: return a[0] - a[1];
            case Op::Mul: return a[0] * a[1];
            case Op::UDiv: return z3::udiv(a[0], a[1]);
            case Op::SDiv: return a[0] / a[1];
            case Op::URem: return z3::urem(a[0], a[1]);
            case Op::SRem: return z3::srem(a[0], a[1]);
            case Op::Shl: return z3::shl(a[0], a[1]);
            case Op::LShr: return z3::lshr(a[0], a[1]);
            case Op::AShr: return z3::ashr(a[0], a[1]);
            case Op::And: return a[0] & a[1];
            case Op::Or: return a[0] | a[1];
            case Op::Xor: return a[0] ^ a[1];
            case Op::Eq: return b2bv(a[0] == a[1]);
            case Op::Ne: return b2bv(a[0] != a[1]);
            case Op::Ult: return b2bv(z3::ult(a[0], a[1]));
            case Op::Ule: return b2bv(z3::ule(a[0], a[1]));
            case Op::Ugt: return b2bv(z3::ugt(a[0], a[1]));
            case Op::Uge: return b2bv(z3::uge(a[0], a[1]));
            case Op::Slt: return b2bv(a[0] < a[1]);
            case Op::Sle: return b2bv(a[0] <= a[1]);
            case Op::Sgt: return b2bv(a[0] > a[1]);
            case Op::Sge: return b2bv(a[0] >= a[1]);
            case Op::Select: return z3::ite(is1(a[0]), a[1], a[2]);
            case Op::ZExt: return w == aw(0) ? a[0] : z3::zext(a[0], w - aw(0));
            case Op::SExt: return w == aw(0) ? a[0] : z3::sext(a[0], w - aw(0));
            case Op::Trunc: return w == aw(0) ? a[0] : a[0].extract(w - 1, 0);
            case Op::SMax: return z3::ite(a[0] >= a[1], a[0], a[1]);
            case Op::SMin: return z3::ite(a[0] <= a[1], a[0], a[1]);
            case Op::UMax: return z3::ite(z3::uge(a[0], a[1]), a[0], a[1]);
            case Op::UMin: return z3::ite(z3::ule(a[0], a[1]), a[0], a[1]);
            case Op::Abs: return z3::ite(a[0] < bv(0, w), -a[0], a[0]);
            case Op::Ctlz: {
                z3::expr r = bv(w, w);
                for (unsigned i = 0; i < w; ++i) r = z3::ite(a[0].extract(i, i) == c.bv_val(1, 1), bv(w - 1 - i, w), r);
                return r;
            }
            case Op::Cttz: {
                z3::expr r = bv(w, w);
                for (int i = static_cast<int>(w) - 1; i >= 0; --i) {
                    auto u = static_cast<unsigned>(i);
                    r = z3::ite(a[0].extract(u, u) == c.bv_val(1, 1), bv(u, w), r);
                }
                return r;
            }
            case Op::Ctpop: {
                z3::expr r = bv(0, w);
                for (unsigned i = 0; i < w; ++i) r = r + z3::zext(a[0].extract(i, i), w - 1);
                return r;
            }
            case Op::Bswap: {
                z3::expr r = a[0].extract(7, 0);
                for (unsigned i = 1; i < w / 8; ++i) r = z3::concat(r, a[0].extract(i * 8 + 7, i * 8));
                return r;
            }
            case Op::SAddOvf: {
                auto x = z3::sext(a[0], 1), y = z3::sext(a[1], 1);
                return b2bv(x + y != z3::sext(a[0] + a[1], 1));
            }
            case Op::SSubOvf: {
                auto x = z3::sext(a[0], 1), y = z3::sext(a[1], 1);
                return b2bv(x - y != z3::sext(a[0] - a[1], 1));
            }
            case Op::SMulOvf:
                // Z3's dedicated no-overflow predicates bit-blast far smaller
                // than a 2w-bit multiplier (same semantics).
                return b2bv(!(z3::bvmul_no_overflow(a[0], a[1], true) && z3::bvmul_no_underflow(a[0], a[1])));
            case Op::UAddOvf: {
                auto x = z3::zext(a[0], 1), y = z3::zext(a[1], 1);
                return b2bv(x + y != z3::zext(a[0] + a[1], 1));
            }
            case Op::USubOvf: return b2bv(z3::ult(a[0], a[1]));
            case Op::UMulOvf: return b2bv(!z3::bvmul_no_overflow(a[0], a[1], false));
            case Op::SDivOvf: {
                unsigned n = aw(0);
                return b2bv(a[0] == bv(uint64_t{1} << (n - 1), n) && a[1] == bv(wmask(n), n));
            }
            case Op::ShiftOob: return b2bv(z3::uge(a[1], bv(aw(0), aw(1))));
            case Op::ShlSOvf: {
                unsigned n = aw(0);
                auto oob = z3::uge(a[1], bv(n, n));
                auto neg = a[0] < bv(0, n);
                auto lost = z3::lshr(a[0], bv(n - 1, n) - a[1]) != bv(0, n);
                return b2bv(oob || neg || lost);
            }
            case Op::ShlNswOvf: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a[1], bv(n, n)) || z3::ashr(z3::shl(a[0], a[1]), a[1]) != a[0]);
            }
            case Op::ShlNuwOvf: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a[1], bv(n, n)) || z3::lshr(z3::shl(a[0], a[1]), a[1]) != a[0]);
            }
            case Op::LostBitsL: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a[1], bv(n, n)) || z3::shl(z3::lshr(a[0], a[1]), a[1]) != a[0]);
            }
            case Op::LostBitsA: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a[1], bv(n, n)) || z3::shl(z3::ashr(a[0], a[1]), a[1]) != a[0]);
            }
            case Op::InexactU: {
                unsigned n = aw(0);
                return b2bv(a[1] != bv(0, n) && z3::urem(a[0], a[1]) != bv(0, n));
            }
            case Op::InexactS: {
                unsigned n = aw(0);
                auto ovf = a[0] == bv(uint64_t{1} << (n - 1), n) && a[1] == bv(wmask(n), n);
                return b2bv(a[1] != bv(0, n) && !ovf && z3::srem(a[0], a[1]) != bv(0, n));
            }
        }
        throw EncodeFail{std::string(laws::ERROR), "internal: unknown PIR op"};
    }

    void encode_node(int id, const std::vector<std::tuple<int, int, int>>& outs) {
        auto& nd = nodes[static_cast<std::size_t>(id)];
        auto& bl = fn.blocks[static_cast<std::size_t>(nd.block)];
        auto& m = vals[static_cast<std::size_t>(id)];
        // reach + phi guards
        std::vector<std::pair<z3::expr, int>> guards;  // (guard, pred node)
        for (auto& [from, k, to] : in_edges[static_cast<std::size_t>(id)]) {
            (void)to;
            guards.emplace_back(edge_guard(from, k), from);
        }
        if (id == 0 && nd.block == 0 && nd.ctx.empty()) {
            reach[static_cast<std::size_t>(id)] = c.bool_val(true);
        } else {
            z3::expr_vector gs(c);
            for (auto& [gd, _] : guards) gs.push_back(gd);
            reach[static_cast<std::size_t>(id)] = gs.empty() ? c.bool_val(false) : z3::mk_or(gs);
        }
        bool havoc_phis = step_loop >= 0 && nd.block == g.loops[static_cast<std::size_t>(step_loop)].header &&
                          !nd.ctx.empty() && nd.ctx.back() == 0;
        std::vector<std::pair<int, z3::expr>> phi_vals;
        for (auto& p : bl.phis) {
            unsigned w = fn.vars[static_cast<std::size_t>(p.dst)].width;
            if (havoc_phis) {
                phi_vals.emplace_back(p.dst, fresh_const("step_" + fn.vars[static_cast<std::size_t>(p.dst)].name, w));
                continue;
            }
            std::optional<z3::expr> acc;
            for (auto it = guards.rbegin(); it != guards.rend(); ++it) {
                int pb = nodes[static_cast<std::size_t>(it->second)].block;
                const Arg* src = nullptr;
                for (auto& [pred, a] : p.in)
                    if (pred == pb) {
                        src = &a;
                        break;
                    }
                z3::expr v = src ? lookup(*src, it->second) : fresh_const("phi_undef", w);
                acc = acc ? z3::ite(it->first, v, *acc) : v;
            }
            phi_vals.emplace_back(p.dst, acc ? *acc : fresh_const("phi_dead", w));
        }
        for (auto& [d, e] : phi_vals) m.insert_or_assign(d, e);
        const auto& r = reach[static_cast<std::size_t>(id)];
        for (auto& s : bl.stmts) {
            switch (s.kind) {
                case Stmt::Assign: m.insert_or_assign(s.dst, op_expr(s, id)); break;
                case Stmt::Check: props.push_back(PropInst{&s, id, r && is1(lookup(s.args[0], id))}); break;
                case Stmt::Assume: assumptions.push_back(z3::implies(r, is1(lookup(s.args[0], id)))); break;
            }
        }
        for (auto& [from, k, to] : outs)
            if (to < 0) cuts.push_back(edge_guard(from, k));
    }

    z3::expr edge_guard(int from, int k) {
        auto& nd = nodes[static_cast<std::size_t>(from)];
        auto& t = fn.blocks[static_cast<std::size_t>(nd.block)].term;
        const auto& r = reach[static_cast<std::size_t>(from)];
        if (t.kind != Term::Br) return r;
        auto cnd = is1(lookup(t.cond, from));
        return r && (k == 0 ? cnd : !cnd);
    }
};

std::vector<uint64_t> model_args(const z3::model& mdl, Encoding& e) {
    std::vector<uint64_t> out;
    for (std::size_t i = 0; i < e.params.size(); ++i) {
        auto v = mdl.eval(e.params[i], true);
        uint64_t u = 0;
        if (!v.is_numeral_u64(u)) u = 0;
        out.push_back(u);
    }
    return out;
}

void add_all(z3::solver& s, const std::vector<z3::expr>& xs) {
    for (auto& x : xs) s.add(x);
}

z3::expr any_of(z3::context& c, const std::vector<z3::expr>& xs) {
    z3::expr_vector v(c);
    for (auto& x : xs) v.push_back(x);
    return v.empty() ? c.bool_val(false) : z3::mk_or(v);
}

// Blocks after the (single) loop: reachable from its exits, outside the body.
std::vector<char> post_loop_blocks(const CfgInfo& g, int L) {
    std::vector<char> after(static_cast<std::size_t>(g.n), 0);
    std::vector<int> work;
    auto& body = g.loops[static_cast<std::size_t>(L)].body;
    for (int b = 0; b < g.n; ++b)
        if (body[static_cast<std::size_t>(b)])
            for (int s : g.succ[static_cast<std::size_t>(b)])
                if (!body[static_cast<std::size_t>(s)] && !after[static_cast<std::size_t>(s)]) {
                    after[static_cast<std::size_t>(s)] = 1;
                    work.push_back(s);
                }
    while (!work.empty()) {
        int x = work.back();
        work.pop_back();
        for (int s : g.succ[static_cast<std::size_t>(x)])
            if (!body[static_cast<std::size_t>(s)] && !after[static_cast<std::size_t>(s)]) {
                after[static_cast<std::size_t>(s)] = 1;
                work.push_back(s);
            }
    }
    return after;
}

// k-induction step for the single loop L: from an arbitrary header state,
// k violation-free iterations that loop back imply iteration k (and the code
// after the loop) is violation-free. true = step closed.
std::optional<bool> kinduction_step(const Function& fn, const CfgInfo& g, int k, unsigned timeout_ms) {
    z3::context c;
    Encoding e(c, fn, g, k + 1);
    e.step_loop = 0;
    e.build();
    auto after = post_loop_blocks(g, 0);
    std::vector<z3::expr> goal;
    z3::solver s(c);
    s.set("timeout", timeout_ms);
    add_all(s, e.assumptions);
    for (auto& p : e.props) {
        auto& nd = e.nodes[static_cast<std::size_t>(p.node)];
        if (!nd.ctx.empty()) {
            if (nd.ctx[0] < k) s.add(!p.viol);
            else goal.push_back(p.viol);
        } else if (after[static_cast<std::size_t>(nd.block)]) {
            goal.push_back(p.viol);
        }
    }
    auto hk = e.ids.find({g.loops[0].header, std::vector<int>{k}});
    if (hk == e.ids.end()) return true;  // header@k unreachable in the unrolling: nothing to show
    s.add(e.reach[static_cast<std::size_t>(hk->second)]);
    s.add(any_of(c, goal));
    auto r = s.check();
    if (r == z3::unsat) return true;
    if (r == z3::sat) return false;
    return std::nullopt;
}

}  // namespace

Verdict check_function(const Function& fn, int unwind, double timeout_s) {
    Verdict v;
    if (unwind < 1) unwind = 1;
    auto timeout_ms = static_cast<unsigned>(std::max(1.0, timeout_s) * 1000.0);
    v.extra["unwind"] = std::to_string(unwind);
    auto g = analyze(fn);
    if (!g.unencoded.empty()) {
        v.status = std::string(laws::NEEDS_HARNESS);
        v.message = g.unencoded;
        return v;
    }
    v.extra["loops"] = std::to_string(g.loops.size());
    try {
        z3::context c;
        Encoding e(c, fn, g, unwind);
        e.build();
        v.extra["instances"] = std::to_string(e.nodes.size());
        v.extra["properties"] = std::to_string(e.props.size());
        z3::solver s(c);
        s.set("timeout", timeout_ms);
        add_all(s, e.assumptions);
        if (!e.props.empty()) {
            std::vector<z3::expr> viols;
            for (auto& p : e.props) viols.push_back(p.viol);
            s.push();
            s.add(any_of(c, viols));
            auto r = s.check();
            if (r == z3::sat) {
                auto mdl = s.get_model();
                const PropInst* hit = nullptr;
                for (auto& p : e.props)
                    if (mdl.eval(p.viol, true).is_true()) {
                        hit = &p;
                        break;
                    }
                if (!hit) hit = &e.props.front();
                v.status = std::string(laws::FAILED);
                v.prop = hit->stmt->prop;
                v.cls = hit->stmt->cls;
                v.line = hit->stmt->line;
                v.cex_args = model_args(mdl, e);
                for (std::size_t i = 0; i < fn.params.size(); ++i)
                    v.cex[fn.vars[static_cast<std::size_t>(fn.params[i])].name] = v.cex_args[i];
                v.message = hit->stmt->prop + ": " + hit->stmt->cls + " (" + hit->stmt->msg + ")";
                return v;
            }
            if (r == z3::unknown) {
                v.status = std::string(laws::UNKNOWN);
                v.message = "solver unknown: " + s.reason_unknown();
                return v;
            }
            s.pop();
        }
        if (e.cuts.empty()) {
            v.status = std::string(laws::PROVED);
            v.extra["unwind_closed"] = "true";
            v.message = g.loops.empty() ? "encoded properties hold on every path (loop-free)"
                                        : "encoded properties hold; every loop closes within unwind " +
                                              std::to_string(unwind);
            v.extra["k_induction"] = "not-needed";
            return v;
        }
        s.push();
        s.add(any_of(c, e.cuts));
        auto r = s.check();
        s.pop();
        if (r == z3::unsat) {
            v.status = std::string(laws::PROVED);
            v.extra["unwind_closed"] = "true";
            v.message = "encoded properties hold; unwinding assertion proved at unwind " + std::to_string(unwind);
            v.extra["k_induction"] = "not-needed";
            return v;
        }
        v.status = std::string(laws::BOUNDED);
        v.extra["unwind_closed"] = "false";
        v.message = "no violation within unwind " + std::to_string(unwind) +
                    (r == z3::sat ? "; loops did not close" : "; unwinding assertion unknown");
        if (g.loops.size() != 1) {
            v.extra["k_induction"] = g.loops.empty() ? "not-needed" : "multiple-loops";
            return v;
        }
        std::vector<std::string> tried;
        for (int k : {1, 2}) {
            if (k > unwind) break;
            tried.push_back(std::to_string(k));
            auto closed = kinduction_step(fn, g, k, timeout_ms);
            v.extra["k_induction_tried"] = [&] {
                std::string t;
                for (auto& x : tried) t += (t.empty() ? "" : ",") + x;
                return t;
            }();
            if (closed && *closed) {
                v.status = std::string(laws::PROVED_UNBOUNDED);
                v.message = "k-induction step closed at k=" + std::to_string(k) + "; not a bounded-only result";
                v.extra["k_induction"] = "closed";
                v.extra["k_induction_k"] = std::to_string(k);
                v.extra["unwind_closed"] = "true";
                return v;
            }
            if (!closed) {
                v.extra["k_induction"] = "unknown";
                return v;
            }
        }
        v.extra["k_induction"] = "step-open";
        return v;
    } catch (const EncodeFail& f) {
        v.status = f.status;
        v.message = f.msg;
        return v;
    } catch (const z3::exception& ex) {
        v.status = std::string(laws::ERROR);
        v.message = std::string("z3: ") + ex.msg();
        return v;
    }
}

std::vector<Vc> pir_vcs(const Function& fn, int unwind) {
    std::vector<Vc> out;
    auto g = analyze(fn);
    if (!g.unencoded.empty()) return out;
    try {
        z3::context c;
        Encoding e(c, fn, g, std::max(1, unwind));
        e.build();
        for (auto& p : e.props) {
            z3::solver s(c);
            add_all(s, e.assumptions);
            s.add(p.viol);
            Vc vc;
            vc.kind = "property";
            vc.prop = p.stmt->prop;
            vc.cls = p.stmt->cls;
            vc.msg = p.stmt->msg;
            vc.line = p.stmt->line;
            vc.smt2 = s.to_smt2();
            out.push_back(std::move(vc));
        }
        if (!e.cuts.empty()) {
            z3::solver s(c);
            add_all(s, e.assumptions);
            s.add(any_of(c, e.cuts));
            Vc vc;
            vc.kind = "unwind";
            vc.prop = "unwind";
            vc.msg = "a loop runs past unwind " + std::to_string(unwind);
            vc.smt2 = s.to_smt2();
            out.push_back(std::move(vc));
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

#else  // !PRISM_HAS_Z3

bool z3_available() { return false; }

Verdict check_function(const Function&, int, double) {
    Verdict v;
    v.status = std::string(laws::NOTRUN);
    v.message = "z3 not built";
    return v;
}

std::vector<Vc> pir_vcs(const Function&, int) { return {}; }

#endif

}  // namespace prism::pir
