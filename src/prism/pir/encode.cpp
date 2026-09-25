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
//   PROVED-CERTIFIED  (--certified) PROVED, and every VC of the function
//                     (each property + the unwinding assertion) has a
//                     CaDiCaL LRAT proof that cake_lpr accepted
//
// Every property VC and the unwinding assertion is one query to
// prism::solver::solve (portfolio, query cache, certified mode); the
// k-induction step is answered by Z3 in-process.

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include "fp.hpp"
#include "memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef PRISM_HAS_Z3
#  include "prism/solver.hpp"
#  include <nlohmann/json.hpp>
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
            for (int d : {s.dst, s.dst2, s.dst3})
                if (d >= 0 && (s.kind == Stmt::Assign || s.kind == Stmt::Alloc || s.kind == Stmt::Load ||
                               s.kind == Stmt::StackSave))
                    g.def_block[static_cast<std::size_t>(d)] = b;
    }
    return g;
}

}  // namespace

std::string format_cex(const Function& fn, const std::vector<uint64_t>& args) {
    std::string out;
    for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        auto& v = fn.vars[static_cast<std::size_t>(fn.params[i])];
        if (i) out += ", ";
        out += v.name + "=" + (v.fp ? fp::format(args[i], v.width) : std::to_string(as_signed(args[i], v.width)));
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

// A memory write (store / memcpy / memset) of one block instance: the
// object its target points into when the provenance analysis knows it
// (SymMem::known), and whether it sets every byte it writes initialised.
struct WriteInst {
    int block;
    std::optional<uint64_t> obj;
    bool inits;
    // the target pointer's variable and the variables it is derived from by
    // copies and checked pointer arithmetic (each stays in the object of the
    // next; loop-cut footprints, houdini.inc)
    std::vector<int> chain;
};

// Write footprint of the k-induction loop (docs/PIR.md "k-induction with
// memory"): the objects havocked at the step's header. all = every object
// allocated before the loop except `const` ones (a store target the
// provenance analysis does not resolve). keep_init: every write in the loop
// initialises what it writes, so a havocked byte's initialised flag is
// "initialised before or arbitrary"; otherwise it is arbitrary.
struct Footprint {
    bool all = false;
    std::set<uint64_t> objs;
    bool keep_init = true;
    // loop-cut mode only: the objects these pointer variables (defined
    // before the loop) point to at the header
    std::set<int> base_vars;
    bool empty() const { return !all && objs.empty() && base_vars.empty(); }
};

struct Encoding {
    z3::context& c;
    const Function& fn;
    const CfgInfo& g;
    int unwind;
    int step_loop = -1;  // k-induction: havoc this loop's header phis at count 0
    Footprint step_fp;   // ... and these objects' bytes (memory written by the loop)
    std::size_t step_havocked = 0;
    std::vector<WriteInst> writes;

    // Loop-cut mode (houdini.inc, docs/PIR.md "Loop invariants"): unwind 1,
    // every loop header instance records its entry state, then havocs its
    // phis and its loop's write footprint (loop_fp) and records that state;
    // every back edge (an unwinding cut) records the next state. Positions
    // are counted in encoding (= topological) order: `props_before` is the
    // number of property instances encoded before the point.
    bool cut_mode = false;
    std::vector<Footprint> loop_fp;
    struct CutLatch {
        int node = -1;
        z3::expr guard;
        std::vector<z3::expr> next;
        mem::SymMem::Mark mem;
        std::size_t props_before = 0;
        int seq = 0;
    };
    struct CutLoop {
        int node = -1;  // header instance (-1: not reached in the unrolling)
        std::vector<z3::expr> entry, havoc;
        mem::SymMem::Mark entry_mem, havoc_mem;
        std::size_t props_before = 0;
        int seq = 0;
        std::size_t havocked = 0;
        std::vector<CutLatch> latches;
    };
    std::vector<CutLoop> cut_loops;
    int seq_counter = 0;

    std::vector<Node> nodes;
    std::map<std::pair<int, std::vector<int>>, int> ids;
    std::vector<std::vector<std::tuple<int, int, int>>> in_edges;  // (from node, succ idx, to)
    std::vector<z3::expr> reach;
    // Guard at the end of each node: its reach strengthened by every Assume
    // in the node. Successor edges start from it, so an assumption restricts
    // only what executes after it (a check earlier in the same block is not
    // constrained by a later assume).
    std::vector<z3::expr> exit_reach;
    std::vector<std::unordered_map<int, z3::expr>> vals;
    std::vector<z3::expr> params;
    std::vector<PropInst> props;
    std::vector<z3::expr> assumptions;
    std::vector<z3::expr> cuts;
    int fresh = 0;
    // The program's __VERIFIER_nondet_* calls (Stmt::nondet_fn), one per
    // block instance, and each instance's position in topological order: on
    // the one path a model makes reachable that order is execution order.
    struct NondetInst {
        const Stmt* stmt;
        int node;
        std::size_t idx;  // statement index in the block
        z3::expr v;
    };
    std::vector<NondetInst> nondets;
    std::vector<int> topo_pos;
    EncodeOptions eo;
    std::optional<mem::SymMem> mem;

    Encoding(z3::context& cc, const Function& f, const CfgInfo& gg, int k, EncodeOptions o = {})
        : c(cc), fn(f), g(gg), unwind(k), eo(o) {
        if (fn.uses_memory) {
            bool tags = false;
            for (auto& b : fn.blocks)
                for (auto& s : b.stmts)
                    if ((s.kind == Stmt::Load || s.kind == Stmt::Store) && s.tag) tags = true;
            mem.emplace(c, eo.memory, tags, assumptions);
        }
    }

    mem::SymMem& M() {
        if (!mem) throw EncodeFail{std::string(laws::ERROR), "internal: memory statement without memory model"};
        return *mem;
    }

    // Memory statements (docs/PIR.md "Memory model"); every update is
    // guarded by the reach condition of this block instance.
    void encode_mem(const Stmt& s, int id, const z3::expr& r, std::unordered_map<int, z3::expr>& m) {
        auto arg = [&](std::size_t i) { return lookup(s.args[i], id); };
        switch (s.kind) {
            case Stmt::Alloc: m.insert_or_assign(s.dst, M().alloc(r, arg(0), s.mkind, s.align, s.init)); break;
            case Stmt::Free: M().free(r, arg(0)); break;
            case Stmt::Revive: M().revive(r, arg(0)); break;
            case Stmt::Load: {
                auto ptr = arg(0);
                auto ld = M().load(ptr, fn.vars[static_cast<std::size_t>(s.dst)].width, s.tag);
                m.insert_or_assign(s.dst, ld.val);
                if (s.dst2 >= 0)
                    m.insert_or_assign(s.dst2, fn.vars[static_cast<std::size_t>(s.dst2)].width == 1 ? ld.uninit : ld.mask);
                if (s.dst3 >= 0) m.insert_or_assign(s.dst3, ld.tagbad);
                break;
            }
            case Stmt::Store: {
                auto p = arg(0);
                auto& in = s.args[2];
                bool inits = in.is_const && in.bits == wmask(in.width);
                writes.push_back(WriteInst{nodes[static_cast<std::size_t>(id)].block, M().known(p), inits, chain_of(s.args[0])});
                M().store(r, p, arg(1), s.args[1].width, arg(2), s.tag);
                break;
            }
            case Stmt::MemCpy: {
                auto p = arg(0);
                writes.push_back(WriteInst{nodes[static_cast<std::size_t>(id)].block, M().known(p), false, chain_of(s.args[0])});
                M().copy(r, p, arg(1), arg(2));
                break;
            }
            case Stmt::MemSet: {
                auto p = arg(0);
                writes.push_back(WriteInst{nodes[static_cast<std::size_t>(id)].block, M().known(p), true, chain_of(s.args[0])});
                M().set(r, p, arg(1), arg(2));
                break;
            }
            case Stmt::StackSave: m.insert_or_assign(s.dst, M().stack_save()); break;
            case Stmt::StackRestore: M().stack_restore(r, arg(0)); break;
            default: break;
        }
    }

    // The pointer variable of a write and the variables it is derived from
    // by copies and checked pointer arithmetic (not selects).
    std::vector<const Stmt*> def_stmt;
    std::vector<int> chain_of(const Arg& a) {
        std::vector<int> out;
        if (!cut_mode || a.is_const) return out;
        if (def_stmt.empty()) {
            def_stmt.assign(fn.vars.size(), nullptr);
            for (auto& b : fn.blocks)
                for (auto& st : b.stmts)
                    if (st.kind == Stmt::Assign && st.dst >= 0) def_stmt[static_cast<std::size_t>(st.dst)] = &st;
        }
        int v = a.var;
        while (v >= 0 && out.size() < 64) {
            out.push_back(v);
            auto* d = def_stmt[static_cast<std::size_t>(v)];
            if (!d || d->args.empty() || d->args[0].is_const) break;
            if (!(d->op == Op::Copy || (d->ptr_arith && d->op != Op::Select))) break;
            v = d->args[0].var;
        }
        return out;
    }

    // Provenance for pointer arithmetic (memory.hpp SymMem::note).
    void note_prov(const Stmt& s, const z3::expr& v, int node) {
        if (!mem || !s.ptr_arith || s.args.empty()) return;
        std::optional<uint64_t> k;
        if (s.op == Op::Select && s.args.size() == 3) {
            auto k1 = mem->known(lookup(s.args[1], node));
            auto k2 = mem->known(lookup(s.args[2], node));
            if (!(k1 && k2 && *k1 == *k2)) return;
            k = k1;
        } else {
            k = mem->known(lookup(s.args[0], node));
        }
        if (k) mem->note(v, *k);
    }

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
        topo_pos.assign(nodes.size(), 0);
        for (std::size_t i = 0; i < order.size(); ++i) topo_pos[static_cast<std::size_t>(order[i])] = static_cast<int>(i);
        reach.assign(nodes.size(), c.bool_val(false));
        exit_reach.assign(nodes.size(), c.bool_val(false));
        vals.assign(nodes.size(), {});
        for (int id : order) encode_node(id, out_edges[static_cast<std::size_t>(id)]);
    }

    // A use outside the loop of its definition. LLVM's LCSSA form never has
    // one, but the translator adds some on paths that leave a loop through an
    // inlined callee (the end of an unwound frame's stack objects at a throw
    // caught by a cleanup outside the loop). The value is the one of the
    // iteration the path left from: the implicit LCSSA phi over the node's
    // incoming edges (memoised in the node's values).
    z3::expr lcssa_value(const Arg& a, int node) {
        auto& m = vals[static_cast<std::size_t>(node)];
        if (auto it = m.find(a.var); it != m.end()) return it->second;
        const auto& ins = in_edges[static_cast<std::size_t>(node)];
        if (ins.empty())
            throw EncodeFail{std::string(laws::NEEDS_HARNESS), "UNENCODED: value used outside its loop (not LCSSA)"};
        std::optional<z3::expr> acc;
        for (auto it = ins.rbegin(); it != ins.rend(); ++it) {
            auto [from, k, to] = *it;
            (void)to;
            z3::expr v = lookup(a, from);
            acc = acc ? z3::ite(edge_guard(from, k), v, *acc) : v;
        }
        m.insert_or_assign(a.var, *acc);
        return *acc;
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
                return lcssa_value(a, node);
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
            case Op::ObjSize: return M().size(a[0]);
            case Op::ObjLive: return M().live(a[0]);
            case Op::ObjKind: return M().kind(a[0]);
            case Op::ObjAlign: return M().align(a[0]);
            default: return fp_expr(s, a, w);
        }
        throw EncodeFail{std::string(laws::ERROR), "internal: unknown PIR op"};
    }

    // ---- floating point (Z3 FPA theory; docs/PIR.md "Floating point") ------
    z3::sort fsort(unsigned w) {
        if (!fp::is_fp_width(w)) throw EncodeFail{std::string(laws::ERROR), "internal: FP op on width " + std::to_string(w)};
        return c.fpa_sort(fp::ebits(w), fp::sbits(w));
    }
    z3::expr to_fp(const z3::expr& bits, unsigned w) { return z3::expr(c, Z3_mk_fpa_to_fp_bv(c, bits, fsort(w))); }
    z3::expr rm(Z3_ast r) { return z3::expr(c, r); }
    z3::expr rne() { return rm(Z3_mk_fpa_rne(c)); }
    // IEEE bits of an FP term: a fresh bitvector whose value is that term.
    // `(= ((_ to_fp e s) b) f)` holds for every NaN encoding b when f is NaN,
    // so a NaN result has an unspecified payload (LLVM's NaN semantics).
    z3::expr bits_of(const z3::expr& f, unsigned w) {
        auto b = fresh_const("fp", w);
        assumptions.push_back(z3::expr(c, Z3_mk_eq(c, to_fp(b, w), f)));
        return b;
    }
    z3::expr fresh_nan_bits(unsigned w) {
        auto b = fresh_const("nan", w);
        assumptions.push_back(z3::expr(c, Z3_mk_fpa_is_nan(c, to_fp(b, w))));
        return b;
    }
    z3::expr is_nan(const z3::expr& f) { return z3::expr(c, Z3_mk_fpa_is_nan(c, f)); }
    z3::expr is_zero(const z3::expr& f) { return z3::expr(c, Z3_mk_fpa_is_zero(c, f)); }
    z3::expr is_inf(const z3::expr& f) { return z3::expr(c, Z3_mk_fpa_is_infinite(c, f)); }
    z3::expr is_neg(const z3::expr& f) { return z3::expr(c, Z3_mk_fpa_is_negative(c, f)); }
    z3::expr flt(const z3::expr& x, const z3::expr& y) { return z3::expr(c, Z3_mk_fpa_lt(c, x, y)); }
    z3::expr fnum(double d, unsigned w) { return z3::expr(c, Z3_mk_fpa_numeral_double(c, d, fsort(w))); }

    z3::expr fp_expr(const Stmt& s, const std::vector<z3::expr>& a, unsigned w) {
        auto aw = [&](std::size_t i) { return s.args[i].width; };
        auto F = [&](std::size_t i) { return to_fp(a[i], aw(i)); };
        auto bin = [&](Z3_ast (*mk)(Z3_context, Z3_ast, Z3_ast, Z3_ast)) {
            return bits_of(z3::expr(c, mk(c, rne(), F(0), F(1))), w);
        };
        auto round = [&](Z3_ast mode) { return bits_of(z3::expr(c, Z3_mk_fpa_round_to_integral(c, rm(mode), F(0))), w); };
        switch (s.op) {
            case Op::FAdd: return bin(Z3_mk_fpa_add);
            case Op::FSub: return bin(Z3_mk_fpa_sub);
            case Op::FMul: return bin(Z3_mk_fpa_mul);
            case Op::FDiv: return bin(Z3_mk_fpa_div);
            case Op::FRem: {
                // C fmod from the IEEE remainder r: same sign as x (or r + sign(x)|y|,
                // exact), a zero carries the sign of x.
                auto x = F(0), y = F(1);
                auto r = z3::expr(c, Z3_mk_fpa_rem(c, x, y));
                auto ay = z3::expr(c, Z3_mk_fpa_abs(c, y));
                auto sy = z3::ite(is_neg(x), z3::expr(c, Z3_mk_fpa_neg(c, ay)), ay);
                auto adj = z3::expr(c, Z3_mk_fpa_add(c, rne(), r, sy));
                auto zero = z3::ite(is_neg(x), z3::expr(c, Z3_mk_fpa_zero(c, fsort(w), true)),
                                    z3::expr(c, Z3_mk_fpa_zero(c, fsort(w), false)));
                auto res = z3::ite(is_nan(r), r, z3::ite(is_zero(r), zero, z3::ite(is_neg(r) == is_neg(x), r, adj)));
                return bits_of(res, w);
            }
            case Op::FSqrt: return bits_of(z3::expr(c, Z3_mk_fpa_sqrt(c, rne(), F(0))), w);
            case Op::FFma: return bits_of(z3::expr(c, Z3_mk_fpa_fma(c, rne(), F(0), F(1), F(2))), w);
            case Op::FMulAdd: {
                auto fused = z3::expr(c, Z3_mk_fpa_fma(c, rne(), F(0), F(1), F(2)));
                auto m = z3::expr(c, Z3_mk_fpa_mul(c, rne(), F(0), F(1)));
                auto split = z3::expr(c, Z3_mk_fpa_add(c, rne(), m, F(2)));
                auto pick = c.bool_const(("fuse!" + std::to_string(fresh++)).c_str());
                return bits_of(z3::ite(pick, fused, split), w);
            }
            case Op::FMinNum:
            case Op::FMaxNum: {
                auto x = F(0), y = F(1);
                auto pick = c.bool_const(("pick!" + std::to_string(fresh++)).c_str());
                bool mn = s.op == Op::FMinNum;
                auto first = mn ? flt(x, y) : flt(y, x);
                auto second = mn ? flt(y, x) : flt(x, y);
                return z3::ite(is_nan(x), a[1],
                               z3::ite(is_nan(y), a[0],
                                       z3::ite(first, a[0], z3::ite(second, a[1], z3::ite(pick, a[0], a[1])))));
            }
            case Op::FMinimum:
            case Op::FMaximum: {
                auto x = F(0), y = F(1);
                bool mn = s.op == Op::FMinimum;
                auto first = mn ? flt(x, y) : flt(y, x);
                auto second = mn ? flt(y, x) : flt(x, y);
                // equal values: -0 is the smaller of the zeros; otherwise the bits agree
                auto zpick = mn ? is_neg(x) : !is_neg(x);
                return z3::ite(is_nan(x) || is_nan(y), fresh_nan_bits(w),
                               z3::ite(first, a[0], z3::ite(second, a[1], z3::ite(zpick, a[0], a[1]))));
            }
            case Op::FFloor: return round(Z3_mk_fpa_rtn(c));
            case Op::FCeil: return round(Z3_mk_fpa_rtp(c));
            case Op::FTruncI: return round(Z3_mk_fpa_rtz(c));
            case Op::FRoundA: return round(Z3_mk_fpa_rna(c));
            case Op::FRoundE: return round(Z3_mk_fpa_rne(c));
            case Op::FOeq: return b2bv(z3::expr(c, Z3_mk_fpa_eq(c, F(0), F(1))));
            case Op::FOlt: return b2bv(flt(F(0), F(1)));
            case Op::FOle: return b2bv(z3::expr(c, Z3_mk_fpa_leq(c, F(0), F(1))));
            case Op::FUno: return b2bv(is_nan(F(0)) || is_nan(F(1)));
            case Op::FIsNaN: return b2bv(is_nan(F(0)));
            case Op::FIsZero: return b2bv(is_zero(F(0)));
            case Op::FIsInf: return b2bv(is_inf(F(0)));
            case Op::FToSI: return z3::expr(c, Z3_mk_fpa_to_sbv(c, rm(Z3_mk_fpa_rtz(c)), F(0), w));
            case Op::FToUI: return z3::expr(c, Z3_mk_fpa_to_ubv(c, rm(Z3_mk_fpa_rtz(c)), F(0), w));
            case Op::SIToF: return bits_of(z3::expr(c, Z3_mk_fpa_to_fp_signed(c, rne(), a[0], fsort(w))), w);
            case Op::UIToF: return bits_of(z3::expr(c, Z3_mk_fpa_to_fp_unsigned(c, rne(), a[0], fsort(w))), w);
            case Op::FConv:
                if (w == aw(0)) return a[0];
                return bits_of(z3::expr(c, Z3_mk_fpa_to_fp_float(c, rne(), F(0), fsort(w))), w);
            case Op::FToSIOvf:
            case Op::FToUIOvf: {
                auto k = static_cast<unsigned>(s.args.at(1).bits);
                auto x = F(0);
                auto t = z3::expr(c, Z3_mk_fpa_round_to_integral(c, rm(Z3_mk_fpa_rtz(c)), x));
                auto fw = aw(0);
                z3::expr lo = s.op == Op::FToSIOvf ? fnum(-std::ldexp(1.0, static_cast<int>(k) - 1), fw)
                                                   : z3::expr(c, Z3_mk_fpa_zero(c, fsort(fw), false));
                z3::expr hi = fnum(std::ldexp(1.0, static_cast<int>(s.op == Op::FToSIOvf ? k - 1 : k)), fw);
                auto below = s.op == Op::FToSIOvf ? flt(t, lo) : (flt(t, lo));  // -0 is not below +0
                auto above = z3::expr(c, Z3_mk_fpa_geq(c, t, hi));
                return b2bv(is_nan(x) || is_inf(x) || below || above);
            }
            case Op::FLibm: {
                // unmodelled libm function: an unconstrained value, except for
                // range facts every IEEE libm keeps (and the interpreter's host
                // library satisfies): |sin|, |cos|, |tanh| <= 1; exp, exp2, cosh >= +0
                auto r = fresh_const("libm", w);
                auto n = s.msg;
                if (w == 32 && n.size() > 1 && n.back() == 'f') n.pop_back();
                auto R = to_fp(r, w);
                if (n == "sin" || n == "cos" || n == "tanh")
                    assumptions.push_back(is_nan(R) || (z3::expr(c, Z3_mk_fpa_leq(c, fnum(-1.0, w), R)) &&
                                                        z3::expr(c, Z3_mk_fpa_leq(c, R, fnum(1.0, w)))));
                if (n == "exp" || n == "exp2" || n == "cosh") assumptions.push_back(is_nan(R) || !is_neg(R));
                return r;
            }
            default: break;
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
        int cut_L = -1;
        if (cut_mode)
            for (std::size_t L = 0; L < g.loops.size(); ++L)
                if (g.loops[L].header == nd.block) cut_L = static_cast<int>(L);
        const int seq = seq_counter++;
        bool havoc_phis = step_loop >= 0 && nd.block == g.loops[static_cast<std::size_t>(step_loop)].header &&
                          !nd.ctx.empty() && nd.ctx.back() == 0;
        if (havoc_phis && mem && !step_fp.empty()) {
            // the memory the loop may have written since the prefix: arbitrary
            std::vector<uint64_t> ids(step_fp.objs.begin(), step_fp.objs.end());
            step_havocked = mem->havoc_objects(reach[static_cast<std::size_t>(id)], ids, step_fp.all,
                                               step_fp.keep_init);
        }
        std::vector<std::pair<int, z3::expr>> phi_vals;
        for (auto& p : bl.phis) {
            unsigned w = fn.vars[static_cast<std::size_t>(p.dst)].width;
            if (havoc_phis) {
                phi_vals.emplace_back(p.dst, fresh_const("step_" + fn.vars[static_cast<std::size_t>(p.dst)].name, w));
                continue;
            }
            std::optional<z3::expr> acc;
            std::optional<uint64_t> prov;
            bool prov_ok = mem && w == 64;
            for (auto it = guards.rbegin(); it != guards.rend(); ++it) {
                int pb = nodes[static_cast<std::size_t>(it->second)].block;
                const Arg* src = nullptr;
                for (auto& [pred, a] : p.in)
                    if (pred == pb) {
                        src = &a;
                        break;
                    }
                z3::expr v = src ? lookup(*src, it->second) : fresh_const("phi_undef", w);
                if (prov_ok) {
                    auto k = mem->known(v);
                    if (!k || (prov && *prov != *k)) prov_ok = false;
                    else prov = k;
                }
                acc = acc ? z3::ite(it->first, v, *acc) : v;
            }
            if (acc && prov_ok && prov) mem->note(*acc, *prov);
            phi_vals.emplace_back(p.dst, acc ? *acc : fresh_const("phi_dead", w));
        }
        if (cut_L >= 0) {
            // entry state, then the arbitrary state of some later header visit
            auto& CL = cut_loops[static_cast<std::size_t>(cut_L)];
            CL.node = id;
            CL.seq = seq;
            CL.props_before = props.size();
            for (auto& pv : phi_vals) CL.entry.push_back(pv.second);
            if (mem) CL.entry_mem = mem->mark();
            const auto& fpL = loop_fp[static_cast<std::size_t>(cut_L)];
            if (mem && !fpL.empty()) {
                std::vector<uint64_t> ids(fpL.objs.begin(), fpL.objs.end());
                CL.havocked = mem->havoc_objects(reach[static_cast<std::size_t>(id)], ids, fpL.all, fpL.keep_init);
                if (!fpL.all) {
                    std::vector<z3::expr> ptrs;
                    for (int bv : fpL.base_vars) ptrs.push_back(lookup(Arg::v(bv, 64), id));
                    CL.havocked += mem->havoc_pointed(reach[static_cast<std::size_t>(id)], ptrs, fpL.keep_init);
                }
            }
            if (mem) CL.havoc_mem = mem->mark();
            for (auto& pv : phi_vals) {
                pv.second = fresh_const("cut_" + fn.vars[static_cast<std::size_t>(pv.first)].name,
                                        fn.vars[static_cast<std::size_t>(pv.first)].width);
                CL.havoc.push_back(pv.second);
            }
        }
        for (auto& [d, e] : phi_vals) m.insert_or_assign(d, e);
        // The guard of the statement being encoded: the node's reach,
        // strengthened by each Assume already passed in this node.
        z3::expr r = reach[static_cast<std::size_t>(id)];
        for (std::size_t si = 0; si < bl.stmts.size(); ++si) {
            const auto& s = bl.stmts[si];
            switch (s.kind) {
                case Stmt::Assign: {
                    auto v = op_expr(s, id);
                    if (s.op == Op::Havoc && !s.nondet_fn.empty()) nondets.push_back(NondetInst{&s, id, si, v});
                    note_prov(s, v, id);
                    m.insert_or_assign(s.dst, v);
                    break;
                }
                case Stmt::Check: props.push_back(PropInst{&s, id, r && is1(lookup(s.args[0], id))}); break;
                case Stmt::Assume: r = r && is1(lookup(s.args[0], id)); break;
                default: encode_mem(s, id, r, m); break;
            }
        }
        exit_reach[static_cast<std::size_t>(id)] = r;
        for (auto& [from, k, to] : outs) {
            if (to >= 0) continue;
            cuts.push_back(edge_guard(from, k));
            if (!cut_mode) continue;
            // a back edge: the next state of its loop's header
            const int h = g.succ[static_cast<std::size_t>(nd.block)][static_cast<std::size_t>(k)];
            int L = -1;
            for (std::size_t j = 0; j < g.loops.size(); ++j)
                if (g.loops[j].header == h) L = static_cast<int>(j);
            if (L < 0) throw EncodeFail{std::string(laws::ERROR), "internal: unwinding cut that is not a back edge"};
            CutLatch cl{id, cuts.back(), {}, mem ? mem->mark() : mem::SymMem::Mark{}, props.size(), seq};
            for (auto& p : fn.blocks[static_cast<std::size_t>(h)].phis) {
                const Arg* src = nullptr;
                for (auto& [pred, a] : p.in)
                    if (pred == nd.block) {
                        src = &a;
                        break;
                    }
                cl.next.push_back(src ? lookup(*src, id)
                                      : fresh_const("phi_undef", fn.vars[static_cast<std::size_t>(p.dst)].width));
            }
            cut_loops[static_cast<std::size_t>(L)].latches.push_back(std::move(cl));
        }
    }

    z3::expr edge_guard(int from, int k) {
        auto& nd = nodes[static_cast<std::size_t>(from)];
        auto& t = fn.blocks[static_cast<std::size_t>(nd.block)].term;
        const auto& r = exit_reach[static_cast<std::size_t>(from)];
        if (t.kind != Term::Br) return r;
        auto cnd = is1(lookup(t.cond, from));
        return r && (k == 0 ? cnd : !cnd);
    }
};

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

// Loop footprint check: every write of a loop-body block instance in `e`
// goes to an object of fp (or fp.all). Provenance is structural (allocation
// results, checked pointer arithmetic, phis that agree), never read from
// memory or from the havocked header phis, so a target it knows in the
// step encoding is the target of that write in every iteration of every
// violation-free run. keep_init needs every write to initialise its bytes.
bool footprint_covers(const Footprint& fp, const std::vector<WriteInst>& writes, const std::vector<char>& body) {
    for (auto& w : writes) {
        if (!body[static_cast<std::size_t>(w.block)]) continue;
        if (fp.keep_init && !w.inits) return false;
        if (fp.all) continue;
        if (!w.obj || !fp.objs.count(*w.obj)) return false;
    }
    return true;
}

// Footprint of the loop from the writes of an encoding of the function.
Footprint footprint_of(const std::vector<WriteInst>& writes, const std::vector<char>& body) {
    Footprint fp;
    for (auto& w : writes) {
        if (!body[static_cast<std::size_t>(w.block)]) continue;
        if (!w.inits) fp.keep_init = false;
        if (w.obj) fp.objs.insert(*w.obj);
        else fp.all = true;
    }
    if (fp.all) fp.objs.clear();
    return fp;
}

struct StepAnswer {
    std::optional<bool> closed;  // true = step closed, false = open, nullopt = unknown
    Footprint fp;                // footprint actually havocked
    std::size_t havocked = 0;    // objects havocked
};

// k-induction step for the single loop L: from an arbitrary header state
// (header phis and the bytes of the loop's write footprint havocked), k
// violation-free iterations that loop back imply iteration k (and the code
// after the loop) is violation-free.
//
// Memory (docs/PIR.md "k-induction with memory"): the prefix is encoded
// concretely, then at the header every byte of every object in the loop's
// write footprint gets an arbitrary value (and an arbitrary or
// "old | arbitrary" initialised flag). The loop allocates and frees nothing
// (checked by the caller), so on every run the memory at any header visit
// is the prefix's except in those bytes: the havocked state covers it. The
// footprint comes from the caller's encoding and is re-checked on the step
// encoding; if it does not cover a write there, the step is re-encoded with
// every object havocked (sound for any write target: a store that reports
// no violation writes an object that exists at the header).
StepAnswer kinduction_step(const Function& fn, const CfgInfo& g, int k, unsigned timeout_ms, Footprint fp,
                           const EncodeOptions& eo) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        z3::context c;
        Encoding e(c, fn, g, k + 1, eo);
        e.step_loop = 0;
        e.step_fp = fp;
        e.build();
        if (!footprint_covers(fp, e.writes, g.loops[0].body)) {
            if (attempt == 1) return StepAnswer{std::nullopt, fp, e.step_havocked};  // cannot happen: all covers
            auto more = footprint_of(e.writes, g.loops[0].body);
            fp.all = true;
            fp.objs.clear();
            fp.keep_init = fp.keep_init && more.keep_init;
            continue;
        }
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
        if (hk == e.ids.end()) return StepAnswer{true, fp, e.step_havocked};  // header@k unreachable: nothing to show
        s.add(e.reach[static_cast<std::size_t>(hk->second)]);
        s.add(any_of(c, goal));
        auto r = s.check();
        if (r == z3::unsat) return StepAnswer{true, fp, e.step_havocked};
        if (r == z3::sat) return StepAnswer{false, fp, e.step_havocked};
        return StepAnswer{std::nullopt, fp, e.step_havocked};
    }
    return StepAnswer{std::nullopt, fp, 0};
}

}  // namespace

namespace {

// SMT-LIB bitvector literal (#x.., #b.., true/false, decimal) -> bits (low 64).
uint64_t literal_bits(const std::string& v) {
    try {
        if (v == "true") return 1;
        if (v == "false") return 0;
        if (v.rfind("#x", 0) == 0) {
            auto h = v.substr(2);
            if (h.size() > 16) h = h.substr(h.size() - 16);
            return std::stoull(h, nullptr, 16);
        }
        if (v.rfind("#b", 0) == 0) {
            auto b = v.substr(2);
            if (b.size() > 64) b = b.substr(b.size() - 64);
            return std::stoull(b, nullptr, 2);
        }
        return std::stoull(v);
    } catch (...) {
        return 0;
    }
}

std::string join_s(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (auto& x : v) out += (out.empty() ? "" : sep) + x;
    return out;
}

// The program's __VERIFIER_nondet_* values on the violating path, in call
// order, as extra["nondet"] ("fn=value, ...", decimal, signed per the C
// type; the same shape as the bmc stage) and extra["nondet_loc"]
// ("line:col, ...", the call's debug location, 0:0 when unknown). A call is
// on the path when the model makes its block instance reachable, and it is
// listed only when it runs before the violated check (the reachable instances
// form one chain; topological order is execution order along it).
//
// The model comes from an in-process Z3 query of the same VC with the
// counterexample's parameters and nondet values pinned to the winning
// solver's model, so every value and every reach flag is read from one model
// of the violation (an external solver's model need not list every constant).
// No model (timeout): no trace, and extra["nondet_note"] says why; a
// refutation without a trace is never replayed as one (tools/svcomp).
void nondet_trace(Encoding& e, const PropInst& hit, const z3::expr& base, const solver::SolveResult& r,
                  double timeout_s, Verdict& v) {
    if (e.nondets.empty()) return;
    auto& c = e.c;
    z3::solver s(c);
    z3::params p(c);
    p.set("timeout", static_cast<unsigned>(std::min(30.0, std::max(1.0, timeout_s)) * 1000));
    s.set(p);
    s.add(base && hit.viol);
    auto pin = [&](const z3::expr& x) {
        auto it = r.model.find(x.decl().name().str());
        if (it == r.model.end()) return;
        unsigned w = x.get_sort().bv_size();
        s.add(x == c.bv_val(static_cast<uint64_t>(literal_bits(it->second) & wmask(w)), w));
    };
    for (auto& pe : e.params) pin(pe);
    for (auto& n : e.nondets) pin(n.v);
    if (s.check() != z3::sat) {
        v.extra["nondet_note"] = "nondet values not reported: the pinned re-query found no model";
        return;
    }
    auto m = s.get_model();
    const auto& hb = e.fn.blocks[static_cast<std::size_t>(e.nodes[static_cast<std::size_t>(hit.node)].block)];
    const auto hidx = static_cast<std::size_t>(hit.stmt - hb.stmts.data());
    const int hpos = e.topo_pos[static_cast<std::size_t>(hit.node)];
    std::vector<std::string> vals, locs;
    for (auto& n : e.nondets) {
        const int pos = e.topo_pos[static_cast<std::size_t>(n.node)];
        if (pos > hpos || (pos == hpos && n.idx >= hidx)) continue;
        if (!m.eval(e.reach[static_cast<std::size_t>(n.node)], true).is_true()) continue;
        uint64_t raw = 0;
        if (!m.eval(n.v, true).is_numeral_u64(raw)) {
            v.extra["nondet_note"] = "nondet values not reported: a value is not a numeral";
            return;
        }
        const auto& var = e.fn.vars[static_cast<std::size_t>(n.stmt->dst)];
        const unsigned w = var.width;
        std::string val;
        if (var.fp) val = fp::format(raw, w);
        else if (n.stmt->nondet_unsigned || w == 1) val = std::to_string(raw & wmask(w));
        else val = std::to_string(as_signed(raw, w));
        vals.push_back(n.stmt->nondet_fn + "=" + val);
        locs.push_back(std::to_string(n.stmt->line) + ":" + std::to_string(n.stmt->col));
    }
    v.extra["nondet"] = join_s(vals, ", ");
    v.extra["nondet_loc"] = join_s(locs, ", ");
}

// One verification condition answered by the solver library.
struct VcAnswer {
    std::string label;  // "ovf+@12" / "unwind"
    solver::SolveResult r;
};

struct VcBook {
    std::deque<VcAnswer> done;  // stable references
    std::map<std::string, int> winners;
    int cache_hits = 0;

    const solver::SolveResult& add(std::string label, solver::SolveResult r) {
        if (!r.winner.empty()) ++winners[r.winner + (r.cache_hit ? " (cache)" : "")];
        if (r.cache_hit) ++cache_hits;
        done.push_back(VcAnswer{std::move(label), std::move(r)});
        return done.back().r;
    }
    std::string summary() const {
        std::vector<std::string> w;
        for (auto& [k, n] : winners) w.push_back(k + ":" + std::to_string(n));
        return std::to_string(done.size()) + " VCs; answered by " + (w.empty() ? "none" : join_s(w, ", ")) +
               "; cache hits " + std::to_string(cache_hits);
    }
};

double detail_now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string vc_label(const PropInst& p) {
    return p.stmt->prop + (p.stmt->line ? "@" + std::to_string(p.stmt->line) : std::string());
}

// Certified mode: PROVED becomes PROVED-CERTIFIED only when every VC of the
// function (every property and the unwinding assertion) is certified.
void certify(Verdict& v, const VcBook& book) {
    v.extra["certificate_vcs"] = std::to_string(book.done.size());
    if (book.done.empty()) {
        // No VC at all (no property inserted, no loop cut): there is no
        // solver answer to check, so there is no certificate. A certificate
        // that checks nothing is not labelled certified; the verdict stays
        // PROVED (its claim rests on the PIR encoder alone).
        v.extra["certify_note"] = "no verification conditions (nothing to certify)";
        return;
    }
    std::vector<std::string> infos, shas;
    for (auto& a : book.done) {
        if (!a.r.certified || a.r.kind != solver::SolveResult::Unsat) {
            std::string why = a.r.note;
            auto p = why.find("not certif");
            if (p != std::string::npos) why = why.substr(p);
            v.extra["certify_note"] = "VC " + a.label + " " +
                                      (why.empty() ? std::string("not certified") : why) + " (" +
                                      std::to_string(infos.size()) + "/" + std::to_string(book.done.size()) +
                                      " VCs certified before it); verdict stays PROVED";
            return;
        }
        infos.push_back(a.label + ": " + a.r.certificate_info);
        shas.push_back(a.r.cnf_sha256);
    }
    v.status = std::string(laws::PROVED_CERTIFIED);
    v.extra[std::string(laws::CERTIFICATE_KEY)] = std::string(laws::CERTIFICATE_CHECKED);
    int lean = 0;
    for (auto& a : book.done)
        if (a.r.certificate_info.find("bitblast: lean-proved") != std::string::npos) ++lean;
    v.extra["certificate_bitblast"] = std::to_string(lean) + "/" + std::to_string(book.done.size()) + " lean-proved";
    v.extra["certificate_info"] = std::to_string(book.done.size()) +
                                  " VCs, each an LRAT proof checked by cake_lpr: " + join_s(infos, " | ");
    v.extra["cnf_sha256"] = join_s(shas, ",");
    v.extra["certificate_scope"] = "per-vc";
    v.extra["certificate_proofs"] = std::to_string(book.done.size());
    v.message += "; every VC certified (" + std::to_string(book.done.size()) + " LRAT proofs checked by cake_lpr)";
}

// Certified mode, one certificate per batch of VCs: each batch's query
// base && (viol_i || ...) was answered UNSAT and its LRAT proof accepted by
// the checkers. UNSAT of a disjunction is UNSAT of every disjunct, and the
// batches partition the function's VCs, so this is the claim the per-VC
// certificates make; the record says which VCs each proof covers. One batch
// (every VC at once) is scope "combined"; several are scope "batched".
struct CertBatch {
    std::vector<std::string> labels;
    solver::SolveResult r;
};

void certify_batches(Verdict& v, const std::vector<CertBatch>& batches) {
    std::size_t nvc = 0;
    int lean = 0;
    std::vector<std::string> covers, infos, shas;
    for (auto& bt : batches) {
        nvc += bt.labels.size();
        if (bt.r.certificate_info.find("bitblast: lean-proved") != std::string::npos) ++lean;
        covers.push_back("[" + join_s(bt.labels, ", ") + "]");
        infos.push_back("combined[" + join_s(bt.labels, ", ") + "]: " + bt.r.certificate_info);
        shas.push_back(bt.r.cnf_sha256);
    }
    const auto n = std::to_string(nvc), np = std::to_string(batches.size());
    const bool one = batches.size() == 1;
    v.status = std::string(laws::PROVED_CERTIFIED);
    v.extra[std::string(laws::CERTIFICATE_KEY)] = std::string(laws::CERTIFICATE_CHECKED);
    v.extra["certificate_vcs"] = n;
    v.extra["certificate_scope"] = one ? "combined" : "batched";
    v.extra["certificate_proofs"] = np;
    v.extra["certificate_covers"] = one ? join_s(batches[0].labels, ", ") : join_s(covers, " ");
    v.extra["certificate_bitblast"] = std::to_string(lean) + "/" + np + " lean-proved (" + np + " CNF" +
                                      (one ? "" : "s") + " for " + n + " VCs)";
    v.extra["certificate_info"] = n + " VCs, " + (one ? std::string("one LRAT proof of their disjunction")
                                                      : np + " LRAT proofs, each of the disjunction of a batch") +
                                  " (UNSAT iff every VC in it is UNSAT) checked by cake_lpr: " + join_s(infos, " | ");
    v.extra["cnf_sha256"] = join_s(shas, ",");
    v.message += one ? "; every VC certified (one LRAT proof of the disjunction of all " + n +
                           " VCs, checked by cake_lpr)"
                     : "; every VC certified (" + np + " LRAT proofs of batches covering all " + n +
                           " VCs, checked by cake_lpr)";
}

// The batch query was UNSAT and CaDiCaL wrote a proof, but a checker did
// not finish checking it in time or ran out of memory (a large proof):
// smaller batches have smaller proofs. Any other reason (no answer, SAT, CaDiCaL itself did not
// finish, a tool missing, a rejected proof) is not helped by splitting.
bool checker_ran_out(const solver::SolveResult& r) {
    if (r.kind != solver::SolveResult::Unsat || r.certified) return false;
    return r.note.find("checker timed out") != std::string::npos ||
           r.note.find("ran out of memory") != std::string::npos || r.note.find("was killed (SIGKILL") != std::string::npos;
}

#include "houdini.inc"

}  // namespace

Verdict check_function(const Function& fn, int unwind, double timeout_s) {
    CheckOptions o;
    o.unwind = unwind;
    o.timeout_s = timeout_s;
    o.use_cache = false;
    return check_function(fn, o);
}

Verdict check_function(const Function& fn, int unwind, double timeout_s, const EncodeOptions& eo) {
    CheckOptions o;
    o.unwind = unwind;
    o.timeout_s = timeout_s;
    o.use_cache = false;
    o.encode = eo;
    return check_function(fn, o);
}

namespace {

Verdict check_function_at(const Function& fn, const CheckOptions& opt);
// set while the first, smaller unwind is tried: a BOUNDED answer there is
// not refined by k-induction (the requested unwind decides it)
thread_local bool tl_first_unwind = false;

}  // namespace

// The bound tried first (roadmap 9.3 "which unwind is tried first"): the
// unrolled program grows with the unwind to the power of the loop nesting
// depth (a lookup loop inside an insertion loop of a std::map model), so a
// function with loops is first checked at a small unwind. Only two answers
// are taken from it (same time limit per query), both exact at the requested unwind as well: PROVED with
// every loop closed (no path reaches the unwinding cut, so a larger unwind
// adds no path) and FAILED (the violating path exists at any larger unwind).
// Anything else (BOUNDED, unknown, a timeout of the shorter first attempt)
// is decided at the requested unwind, as before. Certified mode keeps the
// requested unwind (one certificate per function).
Verdict check_function(const Function& fn, const CheckOptions& opt) {
    constexpr int kFirstUnwind = 4;
    if (opt.certified || opt.unwind <= kFirstUnwind + 1) return check_function_at(fn, opt);
    {
        auto g = analyze(fn);
        if (!g.unencoded.empty() || g.loops.empty()) return check_function_at(fn, opt);
    }
    CheckOptions first = opt;
    first.unwind = kFirstUnwind;
    tl_first_unwind = true;
    Verdict v;
    try {
        v = check_function_at(fn, first);
    } catch (...) {
        tl_first_unwind = false;
        throw;
    }
    tl_first_unwind = false;
    auto closed = v.extra.find("unwind_closed");
    const bool proved = v.status == laws::PROVED && closed != v.extra.end() && closed->second == "true";
    if (proved || v.status == laws::FAILED) {
        v.extra["unwind_requested"] = std::to_string(opt.unwind);
        return v;
    }
    Verdict full = check_function_at(fn, opt);
    full.extra["unwind_first_tried"] = std::to_string(kFirstUnwind) + " (" + v.status + ")";
    return full;
}

namespace {

Verdict check_function_at(const Function& fn, const CheckOptions& opt) {
    const EncodeOptions& eo = opt.encode;
    Verdict v;
    const int unwind = std::max(1, opt.unwind);
    const double timeout_s = std::max(1.0, opt.timeout_s);
    const auto timeout_ms = static_cast<unsigned>(timeout_s * 1000.0);
    v.extra["unwind"] = std::to_string(unwind);
    auto g = analyze(fn);
    if (!g.unencoded.empty()) {
        v.status = std::string(laws::NEEDS_HARNESS);
        v.message = g.unencoded;
        return v;
    }
    v.extra["loops"] = std::to_string(g.loops.size());
    solver::SolveOptions so;
    so.timeout_s = timeout_s;
    so.portfolio = opt.portfolio;
    // Answers come from plain queries; certified mode asks for certificates
    // only once the function is PROVED (certify_proved below): a function
    // that is FAILED, BOUNDED, UNKNOWN or NEEDS-HARNESS pays for none.
    so.certified = false;
    so.use_cache = opt.use_cache;
    so.cache_dir = opt.cache_dir;
    so.max_parallel = opt.max_parallel;
    so.tool_dirs = opt.tool_dirs;
    so.search_default_tools = opt.search_default_tools;
    so.check_timeout_s = opt.check_timeout_s;
    VcBook book;
    auto finish = [&](Verdict& r) -> Verdict {
        r.extra["solver"] = book.summary();
        r.extra["certified_mode"] = opt.certified ? "on" : "off";
        return r;
    };
    try {
        z3::context c;
        Encoding e(c, fn, g, unwind, eo);
        e.build();
        if (fn.uses_memory) {
            v.extra["memory"] = eo.memory == MemEncoding::Array ? "array" : "bv";
            v.extra["objects"] = std::to_string(e.mem->objects());
        }
        v.extra["instances"] = std::to_string(e.nodes.size());
        v.extra["properties"] = std::to_string(e.props.size());
        z3::expr_vector av(c);
        for (auto& a : e.assumptions) av.push_back(a);
        const z3::expr base = av.empty() ? c.bool_val(true) : z3::mk_and(av);
        // "Soft" checks mark a path PIR cannot follow (an exception reaching
        // catch/cleanup code): reachable means NEEDS-HARNESS, never FAILED,
        // and never proved away. They are answered after every hard property.
        auto soft = [](const PropInst& p) { return p.stmt->prop == "throw-unmodelled" || p.stmt->prop == "unmodelled"; };
        // One query per property: SAT <=> that property is violated.
        const PropInst* hit = nullptr;
        std::optional<solver::SolveResult> hit_r;
        std::string no_answer;
        // Many properties (memory checks of inlined library, exception and
        // coroutine code): outside certified mode, one query for "some hard
        // property is violated" first. UNSAT there means every per-property
        // VC is UNSAT (sound), so they are skipped; SAT or unknown falls
        // through to the per-property queries, which find the violation.
        // A SAT group is split in halves until one property is left (its own
        // query gives the counterexample), so a violation costs O(log n)
        // queries instead of up to n.
        std::vector<const PropInst*> hard;
        for (auto& p : e.props)
            if (!soft(p)) hard.push_back(&p);
        // Every VC of the function: each property (soft ones included) and
        // the unwinding assertion. PROVED means every one is UNSAT, and
        // PROVED-CERTIFIED means every one is covered by a checked proof.
        std::vector<z3::expr> all;
        std::vector<std::string> labels;
        for (auto& p : e.props) {
            all.push_back(p.viol);
            labels.push_back(vc_label(p));
        }
        if (!e.cuts.empty()) {
            all.push_back(any_of(c, e.cuts));
            labels.push_back("unwind");
        }
        // Certified mode: certificates for a function whose plain answers
        // made it PROVED. First one certificate of the disjunction of all
        // VCs (split into batches while only the checker runs out of time on
        // a proof), else one per VC. Only certified UNSAT answers count;
        // every certificate query already knows its plain answer
        // (known_unsat), so only CaDiCaL-with-LRAT runs for it.
        std::optional<solver::SolveResult::Kind> plain_all;  // the plain combined query's answer
        VcBook cbook;                                         // certificate queries
        double cert_t0 = 0;  // when certification of this function started
        const double cert_budget = opt.certify_budget_s > 0 ? opt.certify_budget_s : 0.0;
        auto certify_proved = [&](Verdict& vr) {
            cert_t0 = detail_now_s();
            vr.extra["certificate_budget_s"] = cert_budget > 0 ? std::to_string(static_cast<int>(cert_budget)) : "none";
            if (all.empty()) {
                certify(vr, cbook);  // "no verification conditions (nothing to certify)"
                return;
            }
            solver::SolveOptions ko = so;
            ko.certified = true;
            ko.known_unsat = true;
            // Budget: the certificate queries of this function stop once it
            // is spent; the verdict then stays PROVED with a note. Each query
            // gets at most what is left (its own certificate budget too).
            auto left = [&]() { return cert_budget > 0 ? cert_budget - (detail_now_s() - cert_t0) : 1e18; };
            auto out_of_budget = [&](std::size_t done_vcs) {
                char buf[240];
                std::snprintf(buf, sizeof buf,
                              "not certified: the certification budget of this function (%.0f s) was spent after "
                              "%zu/%zu VCs were certified; verdict stays PROVED",
                              cert_budget, done_vcs, all.size());
                vr.extra["certify_note"] = buf;
                vr.extra["certificate_vcs"] = std::to_string(all.size());
            };
            auto budgeted = [&](solver::SolveOptions o) {
                if (cert_budget > 0) {
                    const double l = std::max(1.0, left());
                    const double cap = o.cert_timeout_s > 0 ? o.cert_timeout_s : std::max(60.0, 4.0 * o.timeout_s);
                    o.cert_timeout_s = std::min(cap, l);
                    const double ck = o.check_timeout_s > 0 ? o.check_timeout_s : std::max(60.0, 4.0 * o.timeout_s);
                    o.check_timeout_s = std::min(ck, l);
                    o.timeout_s = std::min(o.timeout_s, l);
                }
                return o;
            };
            if (opt.certify_combined && all.size() >= 2 &&
                !(plain_all && *plain_all == solver::SolveResult::Sat)) {
                std::vector<CertBatch> got;
                std::string failed, uncertifiable;
                std::function<bool(std::size_t, std::size_t)> batch = [&](std::size_t lo, std::size_t hi) -> bool {
                    if (left() <= 0) {
                        failed = "certification budget spent";
                        return false;
                    }
                    const auto l0 = static_cast<std::ptrdiff_t>(lo), l1 = static_cast<std::ptrdiff_t>(hi);
                    std::vector<z3::expr> part(all.begin() + l0, all.begin() + l1);
                    auto cr = solver::solve(c, base && any_of(c, part), budgeted(ko));
                    if (cr.kind == solver::SolveResult::Unsat && cr.certified) {
                        got.push_back(CertBatch{std::vector<std::string>(labels.begin() + l0, labels.begin() + l1),
                                                std::move(cr)});
                        return true;
                    }
                    if (hi - lo >= 4 && checker_ran_out(cr)) {
                        const std::size_t mid = lo + (hi - lo) / 2;
                        return batch(lo, mid) && batch(mid, hi);
                    }
                    std::string why = cr.note;
                    if (auto q = why.find("not certif"); q != std::string::npos) why = why.substr(q);
                    if (auto q = why.find("; ran:"); q != std::string::npos) why = why.substr(0, q);
                    // Not certifiable (floating point, arrays, ...): some VC
                    // of the batch contains what the chain cannot bit-blast,
                    // so the function cannot be certified VC by VC either.
                    if (why.rfind("not certifiable", 0) == 0 && uncertifiable.empty()) uncertifiable = why;
                    failed = "one certificate for " + std::to_string(hi - lo) + " VCs" +
                             (hi - lo == all.size() ? std::string()
                                                    : " (a batch of the " + std::to_string(all.size()) + ")") +
                             " not obtained (" + std::string(solver::kind_name(cr.kind)) +
                             (cr.kind == solver::SolveResult::Unsat ? ", " + why : "") + ")";
                    return false;
                };
                if (batch(0, all.size())) {
                    for (auto& bt : got) cbook.add("combined[" + std::to_string(bt.labels.size()) + " VCs]", bt.r);
                    certify_batches(vr, got);
                    vr.extra["certificate_solver"] = cbook.summary();
                    return;
                }
                if (!uncertifiable.empty()) {
                    vr.extra["certificate_combined"] = failed;
                    vr.extra["certificate_solver"] = cbook.summary();
                    vr.extra["certify_note"] = "not certified: a VC is not certifiable (" +
                                              uncertifiable.substr(std::string_view("not certifiable: ").size()) +
                                              "); verdict stays PROVED";
                    vr.extra["certificate_vcs"] = std::to_string(all.size());
                    return;
                }
                vr.extra["certificate_combined"] = failed + "; one certificate per VC instead";
            }
            // One certificate per VC, in order; the first VC that is not
            // certified ends it (certify() then names it).
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (left() <= 0) {
                    vr.extra["certificate_solver"] = cbook.summary();
                    out_of_budget(i);
                    return;
                }
                const auto& r = cbook.add(labels[i], solver::solve(c, base && all[i], budgeted(ko)));
                if (!(r.kind == solver::SolveResult::Unsat && r.certified)) break;
            }
            vr.extra["certificate_solver"] = cbook.summary();
            certify(vr, cbook);
            vr.extra["certificate_vcs"] = std::to_string(all.size());
        };
        auto proved_message = [&] {
            return e.cuts.empty() ? (g.loops.empty() ? std::string("encoded properties hold on every path (loop-free)")
                                                     : "encoded properties hold; every loop closes within unwind " +
                                                           std::to_string(unwind))
                                  : "encoded properties hold; unwinding assertion proved at unwind " +
                                        std::to_string(unwind);
        };
        // Certified mode, 2+ VCs: one plain query for "some VC is violated"
        // first. UNSAT: every VC is UNSAT (a disjunction), the function is
        // PROVED and goes to certification. Otherwise the per-VC queries
        // below decide the verdict; SAT (a validated model) means some VC is
        // violated, so the function cannot be PROVED and no combined
        // certificate is attempted.
        if (opt.certified && opt.certify_combined && all.size() >= 2) {
            const auto& pr = book.add("all[" + std::to_string(all.size()) + " VCs]",
                                      solver::solve(c, base && any_of(c, all), so));
            plain_all = pr.kind;
            if (pr.kind == solver::SolveResult::Unsat) {
                v.status = std::string(laws::PROVED);
                v.extra["unwind_closed"] = "true";
                v.extra["k_induction"] = "not-needed";
                v.message = proved_message();
                certify_proved(v);
                return finish(v);
            }
            v.extra["certificate_combined"] =
                "one certificate for " + std::to_string(all.size()) + " VCs not obtained (" +
                std::string(solver::kind_name(pr.kind)) +
                (pr.kind == solver::SolveResult::Sat
                     ? ": some VC is violated, so no certificate is attempted; the per-VC queries decide)"
                     : "; the per-VC queries decide, and certify only a PROVED verdict)");
        }
        bool all_unsat = false;
        // properties an UNSAT group query already answered (skipped below)
        std::set<const PropInst*> answered;
        if (hard.size() > 16) {
            std::function<int(std::size_t, std::size_t)> group = [&](std::size_t lo, std::size_t hi) -> int {
                // 1 SAT (hit set), 0 UNSAT, -1 no answer
                if (hi - lo == 1) {
                    const auto& r = book.add(vc_label(*hard[lo]), solver::solve(c, base && hard[lo]->viol, so));
                    if (r.kind == solver::SolveResult::Sat) {
                        hit = hard[lo];
                        hit_r = r;
                        return 1;
                    }
                    return r.kind == solver::SolveResult::Unsat ? 0 : -1;
                }
                std::vector<z3::expr> vs;
                for (std::size_t i = lo; i < hi; ++i) vs.push_back(hard[i]->viol);
                const auto& r = book.add("properties[" + std::to_string(lo) + "," + std::to_string(hi) + ")",
                                         solver::solve(c, base && any_of(c, vs), so));
                if (r.kind == solver::SolveResult::Unsat) {
                    for (std::size_t i = lo; i < hi; ++i) answered.insert(hard[i]);
                    return 0;
                }
                std::size_t mid = lo + (hi - lo) / 2;
                if (r.kind != solver::SolveResult::Sat) {
                    // no answer for the group: its halves are smaller queries
                    // (down to 16 properties; the rest are asked one by one
                    // below, skipping the ones a group answered UNSAT)
                    if (hi - lo <= 16) return -1;
                    int a = group(lo, mid);
                    if (a == 1) return 1;
                    int b = group(mid, hi);
                    if (b == 1) return 1;
                    return a == 0 && b == 0 ? 0 : -1;
                }
                int a = group(lo, mid);
                if (a == 1) return 1;
                int b = group(mid, hi);
                if (b == 1) return 1;
                // SAT group whose halves are both UNSAT: the answers disagree;
                // never read that as "all UNSAT" (fall back to per-property
                // VCs, every one of the group asked again)
                if (a == 0 && b == 0)
                    for (std::size_t i = lo; i < hi; ++i) answered.erase(hard[i]);
                return -1;
            };
            int g = group(0, hard.size());
            all_unsat = g != -1;  // 0: every property UNSAT; 1: hit found (loop below is skipped)
        }
        for (auto& p : e.props) {
            if (soft(p) || all_unsat || answered.count(&p)) continue;
            const auto& r = book.add(vc_label(p), solver::solve(c, base && p.viol, so));
            if (r.kind == solver::SolveResult::Sat) {
                hit = &p;
                hit_r = r;
                break;
            }
            if (r.kind != solver::SolveResult::Unsat && no_answer.empty())
                no_answer = "VC " + vc_label(p) + ": solver " + std::string(solver::kind_name(r.kind)) +
                            (r.note.empty() ? std::string() : " (" + r.note + ")");
        }
        if (hit) {
            v.status = std::string(laws::FAILED);
            v.prop = hit->stmt->prop;
            v.cls = hit->stmt->cls;
            v.line = hit->stmt->line;
            for (auto& pe : e.params) {
                auto it = hit_r->model.find(pe.decl().name().str());
                const uint64_t bits = it == hit_r->model.end() ? 0 : literal_bits(it->second);
                v.cex_args.push_back(bits & wmask(pe.get_sort().bv_size()));
            }
            for (std::size_t i = 0; i < fn.params.size() && i < v.cex_args.size(); ++i)
                v.cex[fn.vars[static_cast<std::size_t>(fn.params[i])].name] = v.cex_args[i];
            v.message = hit->stmt->prop + ": " + hit->stmt->cls + " (" + hit->stmt->msg + ")";
            v.extra["cex_solver"] = hit_r->winner + (hit_r->cache_hit ? " (cache, re-validated)" : "");
            nondet_trace(e, *hit, base, *hit_r, timeout_s, v);
            return finish(v);
        }
        if (!no_answer.empty()) {
            v.status = std::string(laws::UNKNOWN);
            v.message = no_answer;
            return finish(v);
        }
        for (auto& p : e.props) {
            if (!soft(p)) continue;
            const auto& r = book.add(vc_label(p), solver::solve(c, base && p.viol, so));
            if (r.kind == solver::SolveResult::Unsat) continue;
            v.status = std::string(r.kind == solver::SolveResult::Sat ? laws::NEEDS_HARNESS : laws::UNKNOWN);
            v.message = "UNENCODED: " + (p.stmt->msg.empty() ? std::string("exception path") : p.stmt->msg);
            return finish(v);
        }
        if (e.cuts.empty()) {
            v.status = std::string(laws::PROVED);
            v.extra["unwind_closed"] = "true";
            v.message = g.loops.empty() ? "encoded properties hold on every path (loop-free)"
                                        : "encoded properties hold; every loop closes within unwind " +
                                              std::to_string(unwind);
            v.extra["k_induction"] = "not-needed";
            if (opt.certified) certify_proved(v);
            return finish(v);
        }
        const auto& ur = book.add("unwind", solver::solve(c, base && any_of(c, e.cuts), so));
        if (ur.kind == solver::SolveResult::Unsat) {
            v.status = std::string(laws::PROVED);
            v.extra["unwind_closed"] = "true";
            v.message = "encoded properties hold; unwinding assertion proved at unwind " + std::to_string(unwind);
            v.extra["k_induction"] = "not-needed";
            if (opt.certified) certify_proved(v);
            return finish(v);
        }
        v.status = std::string(laws::BOUNDED);
        v.extra["unwind_closed"] = "false";
        v.message = "no violation within unwind " + std::to_string(unwind) +
                    (ur.kind == solver::SolveResult::Sat ? "; loops did not close" : "; unwinding assertion unknown");
        if (opt.certified)
            v.extra["certify_note"] =
                "not certified: certified mode certifies PROVED only (every loop must close within the unwind)";
        if (tl_first_unwind) {
            v.extra["k_induction"] = "not-attempted (first unwind)";
            return finish(v);
        }
        // Loop invariants (houdini.inc): tried when k-induction does not
        // close. Loops that allocate, free or restore the stack are not
        // attempted (the havoc of a header state does not cover a change in
        // the number or liveness of objects).
        auto loops_allocate = [&]() {
            for (auto& L : g.loops)
                for (std::size_t b = 0; b < L.body.size(); ++b) {
                    if (!L.body[b]) continue;
                    for (auto& s : fn.blocks[b].stmts)
                        if (s.kind == Stmt::Alloc || s.kind == Stmt::Free || s.kind == Stmt::StackRestore ||
                            s.kind == Stmt::Revive) return true;
                }
            return false;
        };
        auto try_invariants = [&](Verdict& r) -> bool {
            if (g.loops.empty()) return false;
            if (loops_allocate()) {
                r.extra["invariants_note"] = "not attempted (allocation or free in a loop)";
                return false;
            }
            // The run's Houdini budget (CheckOptions::houdini_budget): spent,
            // the function is not attempted; less left than the search's own
            // limit, the search gets what is left.
            double search_s = -1;
            if (opt.houdini_budget) {
                const double left = opt.houdini_budget->left();
                if (left <= 0) {
                    r.extra["invariants_note"] =
                        "not attempted (the run's Houdini budget of " +
                        std::to_string(static_cast<long>(opt.houdini_budget->total_s)) +
                        " s is spent; PRISM_HOUDINI_BUDGET)";
                    return false;
                }
                search_s = left;
            }
            HoudiniEnv henv;
            if (opt.use_cache)
                henv.cache_dir = opt.cache_dir.empty() ? solver::default_cache_dir() : opt.cache_dir;
            const auto th = std::chrono::steady_clock::now();
            Houdini hd(fn, g, eo, timeout_s, search_s, henv);
            auto h = hd.run();
            const double hs = std::chrono::duration<double>(std::chrono::steady_clock::now() - th).count();
            if (opt.houdini_budget) opt.houdini_budget->spend(hs);
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.2f", hs);
                r.extra["houdini_seconds"] = buf;
            }
            r.extra["houdini_rounds"] = std::to_string(h.rounds);
            r.extra["houdini_candidates"] = std::to_string(h.candidates);
            if (h.prechecks > 0) r.extra["houdini_prechecks"] = std::to_string(h.prechecks);
            if (!h.cache.empty()) r.extra["houdini_cache"] = h.cache;
            if (!h.proved) {
                r.extra["invariants_note"] = "no proof from loop invariants: " + h.why;
                std::size_t kept = 0;
                for (auto& l : h.invariants) kept += l.size();
                if (kept > 0) r.extra["invariants_inductive"] = std::to_string(kept);  // proved, but not enough
                return false;
            }
            std::size_t n = 0;
            nlohmann::json inv = nlohmann::json::array();
            for (auto& l : h.invariants) {
                n += l.size();
                inv.push_back(l);
            }
            r.status = std::string(laws::PROVED_UNBOUNDED);
            r.message = "every loop cut by " + std::to_string(n) +
                        " inductive invariant(s) (Houdini: base and step proved); no property violated in the cut "
                        "program; not a bounded-only result";
            r.extra["k_induction"] = "closed-invariants";
            r.extra["pir_invariants"] = inv.dump();
            r.extra["invariant_checker"] = "z3: Houdini over PIR templates + loop-cut induction (base and step)";
            r.extra["invariant_footprint"] = h.footprint;
            r.extra["unwind_closed"] = "true";
            export_invariants(fn, g, h, r);
            if (opt.certified)
                r.extra["certify_note"] =
                    "not certified: loop invariants are checked by Z3 alone; PROVED-UNBOUNDED is not certified";
            return true;
        };
        if (g.loops.size() != 1) {
            v.extra["k_induction"] = g.loops.empty() ? "not-needed" : "multiple-loops";
            try_invariants(v);
            return finish(v);
        }
        Footprint fp;
        if (fn.uses_memory) {
            // The step case havocs the header phis and the loop's write
            // footprint (docs/PIR.md "k-induction with memory"). A loop that
            // allocates, frees or restores the stack is not attempted: the
            // number and liveness of objects would change across iterations,
            // which the havoc does not cover (BOUNDED stays BOUNDED, Law 2).
            bool allocs = false;
            const auto& body = g.loops[0].body;
            for (std::size_t b = 0; b < body.size() && !allocs; ++b) {
                if (!body[b]) continue;
                for (auto& s : fn.blocks[b].stmts)
                    if (s.kind == Stmt::Alloc || s.kind == Stmt::Free || s.kind == Stmt::StackRestore ||
                            s.kind == Stmt::Revive) allocs = true;
            }
            if (allocs) {
                v.extra["k_induction"] = "not-attempted (allocation or free in the loop)";
                return finish(v);
            }
            fp = footprint_of(e.writes, body);
            v.extra["k_induction_memory"] = fp.empty() ? "read-only loop" : "write footprint havocked";
        }
        // The k-induction step is not a property VC: Z3 answers it in-process,
        // and PROVED-UNBOUNDED is never certified (docs/PIR.md "Solving").
        std::vector<std::string> tried;
        for (int k : {1, 2}) {
            if (k > unwind) break;
            tried.push_back(std::to_string(k));
            auto ans = kinduction_step(fn, g, k, timeout_ms, fp, eo);
            auto& closed = ans.closed;
            v.extra["k_induction_tried"] = join_s(tried, ",");
            if (!ans.fp.empty()) {
                v.extra["k_induction_footprint"] =
                    (ans.fp.all ? std::string("every object allocated before the loop (a store target is not "
                                              "resolved)")
                                : std::to_string(ans.fp.objs.size()) + " object(s)") +
                    ": " + std::to_string(ans.havocked) + " havocked; initialised flags " +
                    (ans.fp.keep_init ? "old or arbitrary" : "arbitrary");
            }
            if (closed && *closed) {
                v.status = std::string(laws::PROVED_UNBOUNDED);
                v.message = "k-induction step closed at k=" + std::to_string(k) + "; not a bounded-only result";
                v.extra["k_induction"] = "closed";
                v.extra["k_induction_k"] = std::to_string(k);
                v.extra["k_induction_solver"] = "z3 (in-process)";
                v.extra["unwind_closed"] = "true";
                if (opt.certified)
                    v.extra["certify_note"] =
                        "not certified: the k-induction step is answered by Z3 alone; PROVED-UNBOUNDED is not certified";
                return finish(v);
            }
            if (!closed) {
                v.extra["k_induction"] = "unknown";
                try_invariants(v);
                return finish(v);
            }
        }
        v.extra["k_induction"] = "step-open";
        try_invariants(v);
        return finish(v);
    } catch (const EncodeFail& f) {
        v.status = f.status;
        v.message = f.msg;
        return v;
    } catch (const mem::EncodeError& f) {
        v.status = f.status;
        v.message = f.msg;
        return v;
    } catch (const z3::exception& ex) {
        v.status = std::string(laws::ERROR);
        v.message = std::string("z3: ") + ex.msg();
        return v;
    }
}

}  // namespace

std::vector<Vc> pir_vcs(const Function& fn, int unwind) {
    EncodeOptions eo;
    eo.memory = MemEncoding::Bv;  // QF_BV: the certified back end cannot take arrays
    return pir_vcs(fn, unwind, eo);
}

std::vector<Vc> pir_vcs(const Function& fn, int unwind, const EncodeOptions& eo) {
    std::vector<Vc> out;
    auto g = analyze(fn);
    if (!g.unencoded.empty()) return out;
    try {
        z3::context c;
        Encoding e(c, fn, g, std::max(1, unwind), eo);
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

Verdict check_function(const Function&, const CheckOptions&) {
    Verdict v;
    v.status = std::string(laws::NOTRUN);
    v.message = "z3 not built";
    return v;
}

Verdict check_function(const Function& fn, int, double) { return check_function(fn, CheckOptions{}); }

Verdict check_function(const Function& fn, int, double, const EncodeOptions&) {
    return check_function(fn, CheckOptions{});
}

std::vector<Vc> pir_vcs(const Function&, int) { return {}; }
std::vector<Vc> pir_vcs(const Function&, int, const EncodeOptions&) { return {}; }

#endif

}  // namespace prism::pir
