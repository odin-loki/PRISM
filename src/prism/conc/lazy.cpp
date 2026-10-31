// Lazy sequentialisation of a concurrent PIR program into one Z3 formula
// (docs/CONCURRENCY.md).
//
// Each thread body is split so every visible operation starts a block, then
// unrolled into a DAG (loops to Options::unwind; a path that would run past
// the bound is cut, i.e. assumed away: the verdict is BOUNDED anyway). DAG
// nodes are numbered in topological order; these numbers are the program
// counter labels of the Lazy-CSeq scheme.
//
// The sequentialised program runs K rounds; in each round thread 0..N-1 in
// order; then thread 0 (the harness) gets one final slot (as in Lazy-CSeq),
// so the code after its joins runs. Thread t's slot in round r is one re-entry of its body:
//
//   runnable = active[t] && !exited && pc[t] != END
//   cs       = nondet, pc[t] <= cs <= END          (context-switch label)
//   reach(n) = runnable && (pc[t] == n || OR (exec(p) && edge p->n))
//   exec(n)  = reach(n) && n < cs                  (guarded jump / guard)
//   pc[t]'   = END if a return executed, else the n with reach(n) && n >= cs
//
// which is Lazy-CSeq's "if (pc > label) goto saved; if (label >= cs) return"
// written as a formula. Thread-local SSA values are "static": a value
// instance keeps its value across rounds (ite(exec(def), new, old)). Shared
// globals, mutex owners, pcs, active flags and the "last visible access"
// used by the race check are threaded through the slots in order.
//
// Properties (each a disjunct; the formula is satisfiable iff a violation is
// reachable within the bound):
//   * every PIR check (assertions, UB) in every thread;
//   * data race: an access to v by thread t whose immediately preceding
//     visible operation (in the global order) was a conflicting access to v
//     by another thread (one write, not both atomic). Locks, unlocks,
//     creates and joins are visible operations in between, so accesses
//     ordered by them are never adjacent;
//   * deadlock: at the end of a round every live thread is at a lock whose
//     mutex is held or at a join of an unfinished thread;
//   * unlock of a mutex the thread does not hold.

#include "prism/conc.hpp"
#include "prism/laws.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#ifdef PRISM_HAS_Z3
#  include <z3++.h>
#endif

namespace prism::conc {

namespace {

using pir::Arg;
using pir::Block;
using pir::Op;
using pir::Stmt;
using pir::Term;

struct Fail {
    std::string status;
    std::string reason;
};

struct Mark {
    int op = -1;
    bool tail = false;
};

Mark marker_of(const std::string& var) {
    static const std::string key = "__prism.conc.";
    auto p = var.rfind(key);
    if (p == std::string::npos) return {};
    auto s = var.substr(p + key.size());
    std::size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == 0 || i > 9) return {};
    auto rest = s.substr(i);
    if (!rest.empty() && rest != "s") return {};
    return {std::stoi(s.substr(0, i)), rest == "s"};
}

// ---------------------------------------------------------------------------
// Thread graph: split at visible operations, loop analysis, unrolling
// ---------------------------------------------------------------------------

struct Loop {
    int header = -1;
    std::vector<char> body;
    int size = 0;
};

struct Node {
    int block = -1;
    std::vector<int> ctx;                       // iteration counts, outer -> inner
    std::vector<std::pair<int, int>> out;       // (target node or -1 = unwinding cut, kind 0 jmp / 1 true / 2 false)
    std::vector<std::pair<int, int>> in;        // (pred node, kind)
    int head = -1;                              // visible op at the head of the node
};

struct Graph {
    const pir::Function* fn = nullptr;
    std::vector<Block> blocks;
    std::vector<int> head_op;                   // per block
    std::vector<Loop> loops;
    std::vector<std::vector<int>> loops_of;
    std::vector<int> def_block;                 // per var
    std::vector<Node> nodes;                    // topological order; id = index
    int end = 0;                                // END label = nodes.size()
    bool has_cut = false;
};

bool is_head(const pir::Function& fn, const Stmt& s) {
    if (s.kind != Stmt::Assign || s.dst < 0) return false;
    auto mk = marker_of(fn.vars[static_cast<std::size_t>(s.dst)].name);
    return mk.op >= 0 && !mk.tail;
}

std::vector<int> succs(const Block& b) {
    if (b.term.kind == Term::Jmp) return {b.term.t};
    if (b.term.kind == Term::Br) return {b.term.t, b.term.f};
    return {};
}

void split(Graph& g) {
    const auto& fn = *g.fn;
    std::size_t nb = fn.blocks.size();
    std::vector<Block> out(nb);
    std::vector<int> last(nb);
    for (std::size_t b = 0; b < nb; ++b) {
        const auto& B = fn.blocks[b];
        out.at(b).name = B.name;
        out.at(b).phis = B.phis;
        int cur = static_cast<int>(b);
        for (const auto& s : B.stmts) {
            if (is_head(fn, s) && !out.at(static_cast<std::size_t>(cur)).stmts.empty()) {
                Block nbk;
                nbk.name = B.name + ".v" + std::to_string(out.size());
                out.push_back(std::move(nbk));
                int j = static_cast<int>(out.size()) - 1;
                Term t;
                t.kind = Term::Jmp;
                t.t = j;
                out.at(static_cast<std::size_t>(cur)).term = t;
                cur = j;
            }
            out.at(static_cast<std::size_t>(cur)).stmts.push_back(s);
        }
        out.at(static_cast<std::size_t>(cur)).term = B.term;
        last.at(b) = cur;
    }
    for (auto& bl : out)
        for (auto& ph : bl.phis)
            for (auto& [pred, _] : ph.in)
                if (pred >= 0 && static_cast<std::size_t>(pred) < nb) pred = last.at(static_cast<std::size_t>(pred));
    g.blocks = std::move(out);
    g.head_op.assign(g.blocks.size(), -1);
    for (std::size_t b = 0; b < g.blocks.size(); ++b) {
        auto& st = g.blocks[b].stmts;
        if (!st.empty() && is_head(fn, st.at(0)))
            g.head_op[b] = marker_of(fn.vars[static_cast<std::size_t>(st.at(0).dst)].name).op;
    }
}

void analyze(Graph& g) {
    int n = static_cast<int>(g.blocks.size());
    std::vector<std::vector<int>> succ(static_cast<std::size_t>(n)), preds(static_cast<std::size_t>(n));
    for (int b = 0; b < n; ++b) succ.at(static_cast<std::size_t>(b)) = succs(g.blocks[static_cast<std::size_t>(b)]);
    std::vector<char> reach(static_cast<std::size_t>(n), 0);
    std::vector<int> state(static_cast<std::size_t>(n), 0);
    std::vector<std::pair<int, int>> retreating;
    if (n > 0) {
        std::vector<std::pair<int, std::size_t>> st{{0, 0}};
        state.at(0) = 1;
        reach.at(0) = 1;
        while (!st.empty()) {
            auto& [b, k] = st.back();
            auto& ss = succ.at(static_cast<std::size_t>(b));
            if (k < ss.size()) {
                int s = ss.at(k++);
                if (state.at(static_cast<std::size_t>(s)) == 0) {
                    state.at(static_cast<std::size_t>(s)) = 1;
                    reach.at(static_cast<std::size_t>(s)) = 1;
                    st.emplace_back(s, 0);
                } else if (state.at(static_cast<std::size_t>(s)) == 1) {
                    retreating.emplace_back(b, s);
                }
            } else {
                state.at(static_cast<std::size_t>(b)) = 2;
                st.pop_back();
            }
        }
    }
    for (int b = 0; b < n; ++b)
        if (reach.at(static_cast<std::size_t>(b)))
            for (int s : succ.at(static_cast<std::size_t>(b))) preds.at(static_cast<std::size_t>(s)).push_back(b);
    std::vector<std::vector<char>> dom(static_cast<std::size_t>(n), std::vector<char>(static_cast<std::size_t>(n), 1));
    if (n > 0) {
        dom.at(0).assign(static_cast<std::size_t>(n), 0);
        dom.at(0).at(0) = 1;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < n; ++b) {
            if (!reach.at(static_cast<std::size_t>(b))) continue;
            std::vector<char> nd(static_cast<std::size_t>(n), 1);
            for (int p : preds.at(static_cast<std::size_t>(b)))
                for (int d = 0; d < n; ++d) nd.at(static_cast<std::size_t>(d)) &= dom.at(static_cast<std::size_t>(p)).at(static_cast<std::size_t>(d));
            nd.at(static_cast<std::size_t>(b)) = 1;
            if (nd != dom.at(static_cast<std::size_t>(b))) {
                dom.at(static_cast<std::size_t>(b)) = std::move(nd);
                changed = true;
            }
        }
    }
    std::map<int, int> by_header;
    for (auto [b, h] : retreating) {
        if (!dom.at(static_cast<std::size_t>(b)).at(static_cast<std::size_t>(h)))
            throw Fail{std::string(laws::NEEDS_HARNESS), "UNENCODED: irreducible control flow"};
        auto it = by_header.find(h);
        if (it == by_header.end()) {
            Loop fresh;
            fresh.header = h;
            fresh.body.assign(static_cast<std::size_t>(n), 0);
            fresh.body[static_cast<std::size_t>(h)] = 1;
            g.loops.push_back(std::move(fresh));
            it = by_header.emplace(h, static_cast<int>(g.loops.size()) - 1).first;
        }
        auto& loop = g.loops[static_cast<std::size_t>(it->second)];
        std::vector<int> st{b};
        while (!st.empty()) {
            int x = st.back();
            st.pop_back();
            if (loop.body[static_cast<std::size_t>(x)]) continue;
            loop.body[static_cast<std::size_t>(x)] = 1;
            for (int p : preds[static_cast<std::size_t>(x)]) st.push_back(p);
        }
    }
    for (auto& L : g.loops) L.size = static_cast<int>(std::count(L.body.begin(), L.body.end(), 1));
    std::vector<int> order(g.loops.size());
    for (std::size_t i = 0; i < order.size(); ++i) order.at(i) = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return g.loops[static_cast<std::size_t>(a)].size > g.loops[static_cast<std::size_t>(b)].size;
    });
    g.loops_of.assign(static_cast<std::size_t>(n), {});
    for (int li : order)
        for (int b = 0; b < n; ++b)
            if (g.loops[static_cast<std::size_t>(li)].body[static_cast<std::size_t>(b)]) g.loops_of[static_cast<std::size_t>(b)].push_back(li);
    g.def_block.assign(g.fn->vars.size(), -1);
    for (int b = 0; b < n; ++b) {
        for (auto& ph : g.blocks[static_cast<std::size_t>(b)].phis) g.def_block[static_cast<std::size_t>(ph.dst)] = b;
        for (auto& s : g.blocks[static_cast<std::size_t>(b)].stmts)
            if (s.kind == Stmt::Assign && s.dst >= 0) g.def_block[static_cast<std::size_t>(s.dst)] = b;
    }
}

void unroll(Graph& g, int unwind, std::size_t max_nodes) {
    std::map<std::pair<int, std::vector<int>>, int> ids;
    std::vector<Node> tmp;
    std::deque<int> work;
    auto get = [&](int b, std::vector<int> loop_ctx) {
        auto key = std::make_pair(b, loop_ctx);
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        if (tmp.size() >= max_nodes)
            throw Fail{std::string(laws::UNKNOWN), "unrolled thread exceeds " + std::to_string(max_nodes) + " nodes"};
        Node nd;
        nd.block = b;
        nd.ctx = std::move(loop_ctx);
        nd.head = g.head_op[static_cast<std::size_t>(b)];
        tmp.push_back(std::move(nd));
        int id = static_cast<int>(tmp.size()) - 1;
        ids.emplace(key, id);
        work.push_back(id);
        return id;
    };
    get(0, std::vector<int>(g.loops_of[0].size(), 0));
    while (!work.empty()) {
        int id = work.front();
        work.pop_front();
        int b = tmp.at(static_cast<std::size_t>(id)).block;
        auto ctx_b = tmp.at(static_cast<std::size_t>(id)).ctx;
        const auto& bl = g.blocks[static_cast<std::size_t>(b)];
        auto ss = succs(bl);
        for (std::size_t k = 0; k < ss.size(); ++k) {
            int s = ss.at(k);
            int kind = bl.term.kind == Term::Jmp ? 0 : (k == 0 ? 1 : 2);
            const auto& Ls = g.loops_of[static_cast<std::size_t>(s)];
            const auto& Lb = g.loops_of[static_cast<std::size_t>(b)];
            std::vector<int> next_ctx;
            bool cut = false;
            for (int L : Ls) {
                auto pos = std::find(Lb.begin(), Lb.end(), L);
                int cnt = 0;
                if (pos != Lb.end()) {
                    cnt = ctx_b[static_cast<std::size_t>(pos - Lb.begin())];
                    if (g.loops[static_cast<std::size_t>(L)].header == s) ++cnt;  // back edge
                }
                if (cnt >= unwind) cut = true;
                next_ctx.push_back(cnt);
            }
            int to = cut ? -1 : get(s, next_ctx);
            if (cut) g.has_cut = true;
            tmp.at(static_cast<std::size_t>(id)).out.emplace_back(to, kind);
        }
    }
    // topological order (the unrolled graph is acyclic)
    std::vector<int> indeg(tmp.size(), 0);
    for (auto& nd : tmp)
        for (auto& [to, _] : nd.out)
            if (to >= 0) ++indeg.at(static_cast<std::size_t>(to));
    std::vector<int> order, ready{0};
    std::vector<int> newid(tmp.size(), -1);
    while (!ready.empty()) {
        int x = ready.back();
        ready.pop_back();
        newid.at(static_cast<std::size_t>(x)) = static_cast<int>(order.size());
        order.push_back(x);
        auto& o = tmp.at(static_cast<std::size_t>(x)).out;
        for (auto it = o.rbegin(); it != o.rend(); ++it)
            if (it->first >= 0 && --indeg.at(static_cast<std::size_t>(it->first)) == 0) ready.push_back(it->first);
    }
    if (order.size() != tmp.size()) throw Fail{std::string(laws::ERROR), "internal: unrolled thread graph has a cycle"};
    g.nodes.resize(tmp.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto nd = std::move(tmp.at(static_cast<std::size_t>(order.at(i))));
        for (auto& [to, _] : nd.out)
            if (to >= 0) to = newid.at(static_cast<std::size_t>(to));
        g.nodes[i] = std::move(nd);
    }
    for (std::size_t i = 0; i < g.nodes.size(); ++i)
        for (auto& [to, kind] : g.nodes[i].out)
            if (to >= 0) g.nodes[static_cast<std::size_t>(to)].in.emplace_back(static_cast<int>(i), kind);
    g.end = static_cast<int>(g.nodes.size());
}

Graph build_graph(const pir::Function& fn, const Options& opt) {
    Graph g;
    g.fn = &fn;
    if (fn.blocks.empty()) throw Fail{std::string(laws::NEEDS_HARNESS), "UNENCODED: empty thread body"};
    split(g);
    analyze(g);
    unroll(g, std::max(1, opt.unwind), opt.max_nodes);
    return g;
}

std::string op_text(const Program& p, const VisOp& o) {
    switch (o.kind) {
        case VisOp::Load:
            if (o.mem) return o.what + " " + (o.obj >= 0 ? p.objects[static_cast<std::size_t>(o.obj)].name + "[...]" : std::string("*ptr"));
            return o.what + " " + p.vars[static_cast<std::size_t>(o.var)].name;
        case VisOp::Store:
            if (o.mem) return o.what + " " + (o.obj >= 0 ? p.objects[static_cast<std::size_t>(o.obj)].name + "[...]" : std::string("*ptr"));
            return o.what + " " + p.vars[static_cast<std::size_t>(o.var)].name;
        case VisOp::Rmw:
            if (o.mem) return o.what + " " + (o.obj >= 0 ? p.objects[static_cast<std::size_t>(o.obj)].name + "[...]" : std::string("*ptr"));
            return o.what + " " + p.vars[static_cast<std::size_t>(o.var)].name;
        case VisOp::Lock:
            [[fallthrough]];
        case VisOp::TryLock:
            [[fallthrough]];
        case VisOp::Unlock:
            [[fallthrough]];
        case VisOp::MutexInit:
            return o.what + "(" + (o.mutex >= 0 ? p.mutexes[static_cast<std::size_t>(o.mutex)] : std::string("*ptr")) + ")";
        case VisOp::Create: return o.what + " T" + std::to_string(o.thread);
        case VisOp::Join: return o.what;
        case VisOp::CondWait:
            [[fallthrough]];
        case VisOp::CondSignal:
            return o.what + "(" + (o.cond >= 0 ? p.conds[static_cast<std::size_t>(o.cond)] : std::string("*ptr")) + ")";
        case VisOp::Alloc: return o.what + "(" + std::to_string(o.size) + ")";
        case VisOp::Free: return o.what;
        case VisOp::Arg: return o.what;
    }
    return o.what;
}

std::string at_line(int line) { return line > 0 ? " (line " + std::to_string(line) + ")" : ""; }

std::string tname(const Program& p, int t) {
    return "T" + std::to_string(t) + " " + p.threads[static_cast<std::size_t>(t)].name;
}

// An exception that leaves a non-main thread is stored on Function::throws and
// the block ends as Stop, with no CXX-UNCAUGHT check. Stop is a quiet exit, so
// the throw would become BOUNDED. It is not a sequentially consistent step.
std::string escaping_throw(const Program& prog) {
    for (const auto& th : prog.threads) {
        if (th.fn.throws.empty()) continue;
        bool reported = false;
        for (const auto& b : th.fn.blocks) {
            for (const auto& s : b.stmts) {
                if (s.kind != Stmt::Check) continue;
                if (s.prop == "uncaught" || s.prop == "throw-unmodelled" || s.cls == "CXX-UNCAUGHT" ||
                    s.cls == "CXX-THROW-NOEXCEPT")
                    reported = true;
            }
        }
        if (reported) continue;
        std::string types;
        for (const auto& ty : th.fn.throws) types += (types.empty() ? "" : ", ") + ty;
        const std::string who = th.name.empty() ? th.entry : th.name;
        return "UNENCODED: throw leaves " + who + " (" + types + ")";
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Readable rendering of the sequentialised program
// ---------------------------------------------------------------------------

std::string lazy_text(const Program& prog, const Options& opt) {
    std::ostringstream o;
    int N = static_cast<int>(prog.threads.size());
    o << "/* Lazy-CSeq sequentialisation: " << opt.rounds << " round(s), " << N << " thread(s), loop unwind "
      << opt.unwind << " */\n";
    o << "/* shared:";
    for (auto& v : prog.vars) o << " i" << v.width << " " << v.name << " = " << v.init << ";";
    for (auto& m : prog.mutexes) o << " mutex " << m << " (owner 0 = free);";
    for (auto& cv : prog.conds) o << " cond " << cv << " (signal count);";
    o << " */\n";
    o << "_Bool active[" << N << "] = {1};  unsigned pc[" << N << "];  _Bool exited;\n";
    for (int t = 0; t < N; ++t) {
        const auto& th = prog.threads[static_cast<std::size_t>(t)];
        Graph g;
        try {
            g = build_graph(th.fn, opt);
        } catch (const Fail& f) {
            o << "/* T" << t << ": " << f.reason << " */\n";
            continue;
        }
        o << "\nvoid seq_T" << t << "_" << th.name << "(void) {  /* re-entered every round; locals are static */\n";
        o << "  unsigned cs = nondet();  assume(pc[" << t << "] <= cs && cs <= " << g.end << ");\n";
        o << "  /* resume: goto the saved label pc[" << t << "] */\n";
        for (std::size_t n = 0; n < g.nodes.size(); ++n) {
            const auto& nd = g.nodes[n];
            if (nd.head < 0 && n != 0) continue;
            o << "  L" << n << ": if (" << n << " >= cs) { pc[" << t << "] = " << n << "; return; }";
            if (nd.head >= 0) {
                const auto& op = prog.ops[static_cast<std::size_t>(nd.head)];
                o << "  " << op_text(prog, op) << at_line(op.line);
            } else {
                o << "  entry";
            }
            if (!nd.ctx.empty()) {
                o << "  /* iteration";
                for (int c : nd.ctx) o << " " << c;
                o << " */";
            }
            o << "\n";
        }
        o << "  pc[" << t << "] = " << g.end << ";  /* END */\n}\n";
    }
    o << "\nint main(void) {\n  for (int r = 0; r < " << opt.rounds << "; ++r) {\n";
    for (int t = 0; t < N; ++t)
        o << "    if (active[" << t << "] && !exited) seq_T" << t << "_" << prog.threads[static_cast<std::size_t>(t)].name
          << "();\n";
    o << "    assert(!deadlock());\n  }\n";
    o << "  if (!exited) seq_T0_" << prog.threads.at(0).name << "();  /* main's final slot */\n";
    o << "  assert(!deadlock());\n}\n";
    o << "/* data race: assert at every access that the previous visible operation was not a conflicting\n"
         "   access of another thread; every PIR check (assertions, UB) is asserted in every thread */\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

#ifdef PRISM_HAS_Z3
namespace {

uint64_t wmask(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }

int64_t as_signed(uint64_t v, unsigned w) {
    if (w == 0) return 0;
    if (w >= 64) return static_cast<int64_t>(v);
    uint64_t sign = uint64_t{1} << (w - 1);
    v &= wmask(w);
    return static_cast<int64_t>((v ^ sign) - sign);
}

struct VRec {
    z3::expr cond;
    std::string kind, cls, msg, key;
    int line = 0;
    int thread = 0;
    int seq = 0;
    int var = -1;                      // race: the variable
    std::optional<z3::expr> other_op;  // race: the preceding access (op index + 1)
    std::vector<z3::expr> pcs;         // deadlock: pc of every thread
};

struct ERec {
    z3::expr exec;
    int round = 0, thread = 0, op = -1, seq = 0;
    std::optional<z3::expr> val;
    unsigned width = 0;
};

struct Enc {
    const Program& p;
    const Options& opt;
    z3::context c;
    std::vector<Graph> g;
    std::vector<std::map<std::pair<int, std::vector<int>>, z3::expr>> vals;  // per thread: value instances
    std::vector<z3::expr> G, owner, pc, active;
    std::vector<z3::expr> cgen;                 // per condition variable: signals so far
    std::vector<std::vector<z3::expr>> ticket;  // [thread][cond]: cgen when the thread began its wait
    z3::expr exited, last_tid, last_var, last_w, last_at, last_op, ok;
    // bounded memory: byte address -> word (the whole access, zero-extended)
    z3::expr M, maddr, last_addr, last_mem;
    // bounded heap: one slot per unrolled execution of an allocation site
    struct HeapSlot { uint64_t id; uint64_t size; z3::expr live; std::string name; };
    std::vector<HeapSlot> heap;
    std::vector<z3::expr> targ;
    // TSO store buffers (Options::tso): per thread pending / variable / value
    std::vector<z3::expr> sb_pend, sb_var, sb_val;
    std::map<std::pair<int, int>, int> create_slot;  // (thread, node) of a looped Create -> created thread
    // mutexes / condition variables addressed through the bounded memory
    z3::expr OwnA, CgenA;
    std::vector<z3::expr> tkd;  // per thread: CgenA[cv] when it released for a wait on a non-global cv  // per thread: the argument its creating call passed
    std::map<std::pair<int, int>, std::size_t> heap_of;  // (thread, node) -> heap index
    std::vector<z3::expr> hard;
    std::vector<VRec> viols;
    std::vector<ERec> events;
    int fresh = 0, seq = 0;

    Enc(const Program& prog, const Options& o)
        : p(prog), opt(o), exited(c.bool_val(false)), last_tid(c.bv_val(0, 8)), last_var(c.bv_val(0, 16)),
          last_w(c.bool_val(false)), last_at(c.bool_val(false)), last_op(c.bv_val(0, 16)), ok(c.bool_val(true)),
          M(z3::const_array(c.bv_sort(64), c.bv_val(0, 64))), maddr(c.bv_val(0, 64)), last_addr(c.bv_val(0, 64)),
          last_mem(c.bool_val(false)), OwnA(z3::const_array(c.bv_sort(64), c.bv_val(0, 8))),
          CgenA(z3::const_array(c.bv_sort(64), c.bv_val(0, 16))) {}

    z3::expr bv(uint64_t v, unsigned w) { return c.bv_val(static_cast<uint64_t>(v & wmask(w)), w); }
    z3::expr b2bv(const z3::expr& b) { return z3::ite(b, c.bv_val(1, 1), c.bv_val(0, 1)); }
    z3::expr is1(const z3::expr& x) { return x == c.bv_val(1, 1); }
    z3::expr fresh_bv(const std::string& base, unsigned w) {
        return c.bv_const((base + "!" + std::to_string(fresh++)).c_str(), w);
    }

    unsigned width_of(int t, const Arg& a) {
        if (a.is_const) return a.width;
        return p.threads[static_cast<std::size_t>(t)].fn.vars[static_cast<std::size_t>(a.var)].width;
    }

    std::pair<int, std::vector<int>> inst_key(int t, int node, int var) {
        const auto& gr = g[static_cast<std::size_t>(t)];
        int d = gr.def_block[static_cast<std::size_t>(var)];
        if (d < 0) return {var, {}};
        const auto& nd = gr.nodes[static_cast<std::size_t>(node)];
        const auto& Ld = gr.loops_of[static_cast<std::size_t>(d)];
        const auto& Lb = gr.loops_of[static_cast<std::size_t>(nd.block)];
        if (Ld.size() > Lb.size() || !std::equal(Ld.begin(), Ld.end(), Lb.begin()))
            throw Fail{std::string(laws::NEEDS_HARNESS), "UNENCODED: value used outside its loop (not LCSSA)"};
        return {var, std::vector<int>(nd.ctx.begin(), nd.ctx.begin() + static_cast<std::ptrdiff_t>(Ld.size()))};
    }

    z3::expr& slot(int t, const std::pair<int, std::vector<int>>& key) {
        auto& m = vals[static_cast<std::size_t>(t)];
        auto it = m.find(key);
        if (it == m.end()) {
            unsigned w = p.threads[static_cast<std::size_t>(t)].fn.vars[static_cast<std::size_t>(key.first)].width;
            it = m.emplace(key, fresh_bv("undef.T" + std::to_string(t), w)).first;
        }
        return it->second;
    }

    z3::expr lookup(int t, int node, const Arg& a) {
        if (a.is_const) return bv(a.bits, a.width);
        return slot(t, inst_key(t, node, a.var));
    }

    void define(int t, int node, int var, const z3::expr& e, const z3::expr& val) {
        auto& s = slot(t, inst_key(t, node, var));
        s = z3::ite(e, val, s);
    }

    z3::expr op_expr(int t, int node, const Stmt& s) {
        const auto& fn = p.threads[static_cast<std::size_t>(t)].fn;
        unsigned w = fn.vars[static_cast<std::size_t>(s.dst)].width;
        std::vector<z3::expr> a;
        for (auto& x : s.args) a.push_back(lookup(t, node, x));
        auto aw = [&](std::size_t i) { return width_of(t, s.args.at(i)); };
        switch (s.op) {
            case Op::Copy: return a.at(0);
            case Op::Havoc: return fresh_bv(s.uninit ? "uninit" : s.nondet ? "nondet" : "havoc", w);
            case Op::Add: return a.at(0) + a.at(1);
            case Op::Sub: return a.at(0) - a.at(1);
            case Op::Mul: return a.at(0) * a.at(1);
            case Op::UDiv: return z3::udiv(a.at(0), a.at(1));
            case Op::SDiv: return a.at(0) / a.at(1);
            case Op::URem: return z3::urem(a.at(0), a.at(1));
            case Op::SRem: return z3::srem(a.at(0), a.at(1));
            case Op::Shl: return z3::shl(a.at(0), a.at(1));
            case Op::LShr: return z3::lshr(a.at(0), a.at(1));
            case Op::AShr: return z3::ashr(a.at(0), a.at(1));
            case Op::And: return a.at(0) & a.at(1);
            case Op::Or: return a.at(0) | a.at(1);
            case Op::Xor: return a.at(0) ^ a.at(1);
            case Op::Eq: return b2bv(a.at(0) == a.at(1));
            case Op::Ne: return b2bv(a.at(0) != a.at(1));
            case Op::Ult: return b2bv(z3::ult(a.at(0), a.at(1)));
            case Op::Ule: return b2bv(z3::ule(a.at(0), a.at(1)));
            case Op::Ugt: return b2bv(z3::ugt(a.at(0), a.at(1)));
            case Op::Uge: return b2bv(z3::uge(a.at(0), a.at(1)));
            case Op::Slt: return b2bv(a.at(0) < a.at(1));
            case Op::Sle: return b2bv(a.at(0) <= a.at(1));
            case Op::Sgt: return b2bv(a.at(0) > a.at(1));
            case Op::Sge: return b2bv(a.at(0) >= a.at(1));
            case Op::Select: return z3::ite(is1(a.at(0)), a.at(1), a.at(2));
            case Op::ZExt: return w == aw(0) ? a.at(0) : z3::zext(a.at(0), w - aw(0));
            case Op::SExt: return w == aw(0) ? a.at(0) : z3::sext(a.at(0), w - aw(0));
            case Op::Trunc: return w == aw(0) ? a.at(0) : a.at(0).extract(w - 1, 0);
            case Op::SMax: return z3::ite(a.at(0) >= a.at(1), a.at(0), a.at(1));
            case Op::SMin: return z3::ite(a.at(0) <= a.at(1), a.at(0), a.at(1));
            case Op::UMax: return z3::ite(z3::uge(a.at(0), a.at(1)), a.at(0), a.at(1));
            case Op::UMin: return z3::ite(z3::ule(a.at(0), a.at(1)), a.at(0), a.at(1));
            case Op::Abs: return z3::ite(a.at(0) < bv(0, w), -a.at(0), a.at(0));
            case Op::Ctlz: {
                z3::expr r = bv(w, w);
                for (unsigned i = 0; i < w; ++i) r = z3::ite(a.at(0).extract(i, i) == c.bv_val(1, 1), bv(w - 1 - i, w), r);
                return r;
            }
            case Op::Cttz: {
                z3::expr r = bv(w, w);
                for (int i = static_cast<int>(w) - 1; i >= 0; --i) {
                    auto u = static_cast<unsigned>(i);
                    r = z3::ite(a.at(0).extract(u, u) == c.bv_val(1, 1), bv(u, w), r);
                }
                return r;
            }
            case Op::Ctpop: {
                z3::expr r = bv(0, w);
                for (unsigned i = 0; i < w; ++i) r = r + z3::zext(a.at(0).extract(i, i), w - 1);
                return r;
            }
            case Op::Bswap: {
                z3::expr r = a.at(0).extract(7, 0);
                for (unsigned i = 1; i < w / 8; ++i) r = z3::concat(r, a.at(0).extract(i * 8 + 7, i * 8));
                return r;
            }
            case Op::SAddOvf: {
                auto x = z3::sext(a.at(0), 1), y = z3::sext(a.at(1), 1);
                return b2bv(x + y != z3::sext(a.at(0) + a.at(1), 1));
            }
            case Op::SSubOvf: {
                auto x = z3::sext(a.at(0), 1), y = z3::sext(a.at(1), 1);
                return b2bv(x - y != z3::sext(a.at(0) - a.at(1), 1));
            }
            case Op::SMulOvf:
                return b2bv(!(z3::bvmul_no_overflow(a.at(0), a.at(1), true) && z3::bvmul_no_underflow(a.at(0), a.at(1))));
            case Op::UAddOvf: {
                auto x = z3::zext(a.at(0), 1), y = z3::zext(a.at(1), 1);
                return b2bv(x + y != z3::zext(a.at(0) + a.at(1), 1));
            }
            case Op::USubOvf: return b2bv(z3::ult(a.at(0), a.at(1)));
            case Op::UMulOvf: return b2bv(!z3::bvmul_no_overflow(a.at(0), a.at(1), false));
            case Op::SDivOvf: {
                unsigned n = aw(0);
                return b2bv(a.at(0) == bv(uint64_t{1} << (n - 1), n) && a.at(1) == bv(wmask(n), n));
            }
            case Op::ShiftOob: return b2bv(z3::uge(a.at(1), bv(aw(0), aw(1))));
            case Op::ShlSOvf: {
                unsigned n = aw(0);
                auto oob = z3::uge(a.at(1), bv(n, n));
                auto neg = a.at(0) < bv(0, n);
                auto lost = z3::lshr(a.at(0), bv(n - 1, n) - a.at(1)) != bv(0, n);
                return b2bv(oob || neg || lost);
            }
            case Op::ShlNswOvf: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a.at(1), bv(n, n)) || z3::ashr(z3::shl(a.at(0), a.at(1)), a.at(1)) != a.at(0));
            }
            case Op::ShlNuwOvf: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a.at(1), bv(n, n)) || z3::lshr(z3::shl(a.at(0), a.at(1)), a.at(1)) != a.at(0));
            }
            case Op::LostBitsL: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a.at(1), bv(n, n)) || z3::shl(z3::lshr(a.at(0), a.at(1)), a.at(1)) != a.at(0));
            }
            case Op::LostBitsA: {
                unsigned n = aw(0);
                return b2bv(z3::uge(a.at(1), bv(n, n)) || z3::shl(z3::ashr(a.at(0), a.at(1)), a.at(1)) != a.at(0));
            }
            case Op::InexactU: {
                unsigned n = aw(0);
                return b2bv(a.at(1) != bv(0, n) && z3::urem(a.at(0), a.at(1)) != bv(0, n));
            }
            case Op::InexactS: {
                unsigned n = aw(0);
                auto ovf = a.at(0) == bv(uint64_t{1} << (n - 1), n) && a.at(1) == bv(wmask(n), n);
                return b2bv(a.at(1) != bv(0, n) && !ovf && z3::srem(a.at(0), a.at(1)) != bv(0, n));
            }
            default:
                // IEEE floating point is encoded by the pir stage only
                if (s.op >= Op::FAdd)
                    throw Fail{std::string(laws::NEEDS_HARNESS),
                               std::string("UNENCODED: floating point (") + op_name(s.op) + ") in a threaded program"};
                break;
        }
        throw Fail{std::string(laws::ERROR), "internal: unknown PIR op"};
    }

    z3::expr end_of(int u) { return c.bv_val(g[static_cast<std::size_t>(u)].end, 16); }
    z3::expr done(int u) { return active[static_cast<std::size_t>(u)] && pc[static_cast<std::size_t>(u)] == end_of(u); }

    // the thread whose handle value is h has finished
    z3::expr joinable(const z3::expr& h) {
        z3::expr r = c.bool_val(false);
        unsigned w = h.get_sort().bv_size();
        for (std::size_t u = 1; u < p.threads.size(); ++u) r = r || (h == bv(u, w) && done(static_cast<int>(u)));
        return r;
    }

    void violation(const z3::expr& cond, std::string kind, std::string cls, std::string msg, int line, int t,
                   std::string key) {
        VRec v{ok && cond, std::move(kind), std::move(cls), std::move(msg), std::move(key), line, t, seq, -1, {}, {}};
        viols.push_back(std::move(v));
    }

    void touch(int t, const z3::expr& e, int op, int var, bool write, bool atomic) {
        last_tid = z3::ite(e, bv(static_cast<uint64_t>(t + 1), 8), last_tid);
        last_var = z3::ite(e, bv(static_cast<uint64_t>(var + 1), 16), last_var);
        last_w = z3::ite(e, c.bool_val(write), last_w);
        last_at = z3::ite(e, c.bool_val(atomic), last_at);
        last_op = z3::ite(e, bv(static_cast<uint64_t>(op + 1), 16), last_op);
        last_mem = z3::ite(e, c.bool_val(false), last_mem);
    }

    void touch_mem(int t, const z3::expr& e, int op, const z3::expr& addr, bool write, bool atomic) {
        touch(t, e, op, -1, write, atomic);
        last_mem = z3::ite(e, c.bool_val(true), last_mem);
        last_addr = z3::ite(e, addr, last_addr);
    }

    // A memory access: in bounds of its object (objects are 2^32 apart, so an
    // out-of-bounds address never reaches another object; it is UB itself),
    // and a race when the last visible operation was a conflicting access to
    // the same address by another thread.
    void mem_checks(int t, const z3::expr& e, int op, const VisOp& o, const z3::expr& addr, bool write) {
        auto id = z3::lshr(addr, c.bv_val(32, 64));
        auto off = addr & bv(0xffffffffu, 64);
        z3::expr inb = c.bool_val(false);
        for (std::size_t k = 0; k < p.objects.size(); ++k)
            inb = inb || (id == c.bv_val(static_cast<uint64_t>(k + 1), 64) &&
                          z3::ule(off + c.bv_val(static_cast<uint64_t>((o.width + 7) / 8), 64), c.bv_val(p.objects[k].size, 64)));
        z3::expr freed = c.bool_val(false);
        for (auto& h : heap) {
            auto here = id == c.bv_val(h.id, 64);
            inb = inb || (here && h.live &&
                          z3::ule(off + c.bv_val(static_cast<uint64_t>((o.width + 7) / 8), 64), c.bv_val(h.size, 64)));
            freed = freed || (here && !h.live);
        }
        auto what = o.obj >= 0 ? p.objects[static_cast<std::size_t>(o.obj)].name : std::string("memory");
        violation(e && freed, write ? "uaf-write" : "uaf-read", "MEM-USE-AFTER-FREE",
                  "use after free: " + o.what + " by " + tname(p, t) + at_line(o.line), o.line, t,
                  "uaf:" + std::to_string(op));
        violation(e && !freed && !inb, write ? "oob-write" : "oob-read", write ? "MEM-OOB-WRITE" : "MEM-OOB-READ",
                  "out-of-bounds " + o.what + " of " + what + " by " + tname(p, t) + at_line(o.line), o.line, t,
                  "oob:" + std::to_string(op));
        auto conflict = e && last_mem && last_addr == addr && last_tid != bv(0, 8) &&
                        last_tid != bv(static_cast<uint64_t>(t + 1), 8) && (last_w || c.bool_val(write)) &&
                        !(last_at && c.bool_val(o.atomic));
        VRec v{ok && conflict,
               "race",
               "CONC-DATA-RACE",
               "data race on " + what + ": " + o.what + " by " + tname(p, t) + at_line(o.line),
               "race:" + what + ":" + std::to_string(o.line),
               o.line,
               t,
               seq,
               -1,
               last_op,
               {}};
        viols.push_back(std::move(v));
    }

    void race_check(int t, const z3::expr& e, int op, const VisOp& o, bool write) {
        auto conflict = e && last_tid != bv(0, 8) && last_tid != bv(static_cast<uint64_t>(t + 1), 8) &&
                        last_var == bv(static_cast<uint64_t>(o.var + 1), 16) && (last_w || c.bool_val(write)) &&
                        !(last_at && c.bool_val(o.atomic));
        const auto& vn = p.vars[static_cast<std::size_t>(o.var)].name;
        VRec v{ok && conflict,
               "race",
               "CONC-DATA-RACE",
               "data race on " + vn + ": " + o.what + " by " + tname(p, t) + at_line(o.line),
               "race:" + vn + ":" + std::to_string(o.line),
               o.line,
               t,
               seq,
               o.var,
               last_op,
               {}};
        viols.push_back(std::move(v));
        (void)op;
    }

    void event(const z3::expr& e, int r, int t, int op, std::optional<z3::expr> val, unsigned w) {
        events.push_back(ERec{e, r, t, op, seq, std::move(val), w});
    }

    // commit thread u's buffered store when `when`
    void sb_flush(std::size_t u, const z3::expr& when) {
        auto go = when && sb_pend[u];
        for (std::size_t v = 0; v < p.vars.size(); ++v) {
            unsigned w = p.vars[v].width;
            auto val = w < 64 ? sb_val[u].extract(w - 1, 0) : sb_val[u];
            G[v] = z3::ite(go && sb_var[u] == bv(v, 16), val, G[v]);
        }
        sb_pend[u] = z3::ite(when, c.bool_val(false), sb_pend[u]);
    }

    void visible(int r, int t, int node, const Stmt& s, const Mark& mk, const z3::expr& e) {
        const auto& o = p.ops[static_cast<std::size_t>(mk.op)];
        auto tid = bv(static_cast<uint64_t>(t + 1), 8);
        bool plain_scalar = (o.kind == VisOp::Load || o.kind == VisOp::Store) && !o.mem && !o.atomic;
        if (opt.tso && !mk.tail) {
            for (std::size_t u = 0; u < sb_pend.size(); ++u) sb_flush(u, e && c.bool_const(("sbf!" + std::to_string(fresh++)).c_str()));
            if (!plain_scalar) sb_flush(static_cast<std::size_t>(t), e);
            if (o.kind == VisOp::Join) {  // a finished thread has drained its buffer
                auto h = lookup(t, node, s.args.at(0));
                unsigned hw = h.get_sort().bv_size();
                for (std::size_t u = 1; u < sb_pend.size(); ++u) sb_flush(u, e && h == bv(u, hw));
            }
        }
        if (opt.tso && plain_scalar) {
            const auto vi = static_cast<std::size_t>(o.var);
            const auto ti = static_cast<std::size_t>(t);
            auto& gv = G[vi];
            race_check(t, e, mk.op, o, o.kind == VisOp::Store);
            unsigned w = p.vars[vi].width;
            if (o.kind == VisOp::Store) {
                auto nv = lookup(t, node, s.args.at(0));
                event(e, r, t, mk.op, nv, o.width);
                sb_flush(ti, e);  // one entry: the older store commits first
                sb_pend[ti] = z3::ite(e, c.bool_val(true), sb_pend[ti]);
                sb_var[ti] = z3::ite(e, bv(vi, 16), sb_var[ti]);
                sb_val[ti] = z3::ite(e, w < 64 ? z3::zext(nv, 64 - w) : nv, sb_val[ti]);
                define(t, node, s.dst, e, nv);
            } else {
                auto fwd = sb_pend[ti] && sb_var[ti] == bv(vi, 16);
                auto val = z3::ite(fwd, w < 64 ? sb_val[ti].extract(w - 1, 0) : sb_val[ti], gv);
                event(e, r, t, mk.op, val, o.width);
                define(t, node, s.dst, e, val);
            }
            touch(t, e, mk.op, o.var, o.kind == VisOp::Store, false);
            return;
        }
        if (o.mem) {
            if (!mk.tail) {  // head: the address
                maddr = lookup(t, node, s.args.at(0));
                bool write = o.kind != VisOp::Load;
                mem_checks(t, e, mk.op, o, maddr, write);
                event(e, r, t, mk.op, maddr, 64);
                define(t, node, s.dst, e, maddr);
                touch_mem(t, e, mk.op, maddr, write, o.atomic);
                return;
            }
            if (o.kind == VisOp::Store) {
                auto nv = lookup(t, node, s.args.at(0));
                auto word = o.width < 64 ? z3::zext(nv, 64 - o.width) : nv;
                M = z3::ite(e, z3::store(M, maddr, word), M);
                define(t, node, s.dst, e, nv);
            } else {
                auto word = z3::select(M, maddr);
                define(t, node, s.dst, e, o.width < 64 ? word.extract(o.width - 1, 0) : word);
            }
            return;
        }
        if (o.kind == VisOp::Load || o.kind == VisOp::Store || o.kind == VisOp::Rmw) {
            auto& gv = G[static_cast<std::size_t>(o.var)];
            if (mk.tail) {  // Rmw write part, same atomic step as the head
                auto nv = lookup(t, node, s.args.at(0));
                gv = z3::ite(e, nv, gv);
                define(t, node, s.dst, e, nv);
                return;
            }
            bool write = o.kind != VisOp::Load;
            race_check(t, e, mk.op, o, write);
            if (o.kind == VisOp::Store) {
                auto nv = lookup(t, node, s.args.at(0));
                event(e, r, t, mk.op, nv, o.width);
                gv = z3::ite(e, nv, gv);
                define(t, node, s.dst, e, nv);
            } else {
                event(e, r, t, mk.op, gv, o.width);
                define(t, node, s.dst, e, gv);
            }
            touch(t, e, mk.op, o.var, write, o.atomic);
            return;
        }
        if (o.kind == VisOp::Unlock && o.cond_dyn && mk.tail) {  // wait release: remember the cv generation
            auto& tk = tkd[static_cast<std::size_t>(t)];
            tk = z3::ite(e, z3::select(CgenA, lookup(t, node, s.args.at(0))), tk);
            define(t, node, s.dst, e, lookup(t, node, s.args.at(0)));
            return;
        }
        if (o.dyn && (o.kind == VisOp::Lock || o.kind == VisOp::Unlock || o.kind == VisOp::MutexInit ||
                      o.kind == VisOp::CondWait || o.kind == VisOp::CondSignal)) {
            event(e, r, t, mk.op, std::nullopt, 0);
            auto a = lookup(t, node, s.args.at(0));
            switch (o.kind) {
                case VisOp::Lock:
                    ok = ok && z3::implies(e, z3::select(OwnA, a) == bv(0, 8));
                    OwnA = z3::ite(e, z3::store(OwnA, a, tid), OwnA);
                    break;
                case VisOp::Unlock:
                    violation(e && z3::select(OwnA, a) != tid, "unlock", "LOCK-DOUBLE-UNLOCK",
                              "unlock of a mutex that " + tname(p, t) + " does not hold" + at_line(o.line), o.line, t,
                              "unlock:*:" + std::to_string(o.line));
                    OwnA = z3::ite(e, z3::store(OwnA, a, bv(0, 8)), OwnA);
                    if (o.cond >= 0) {
                        auto& tk = ticket[static_cast<std::size_t>(t)][static_cast<std::size_t>(o.cond)];
                        tk = z3::ite(e, cgen[static_cast<std::size_t>(o.cond)], tk);
                    }
                    break;
                case VisOp::MutexInit: OwnA = z3::ite(e, z3::store(OwnA, a, bv(0, 8)), OwnA); break;
                case VisOp::CondWait:
                    ok = ok && z3::implies(e, z3::select(CgenA, a) != tkd[static_cast<std::size_t>(t)]);
                    break;
                case VisOp::CondSignal:
                    CgenA = z3::ite(e, z3::store(CgenA, a, z3::select(CgenA, a) + bv(1, 16)), CgenA);
                    break;
                default: break;
            }
            define(t, node, s.dst, e, a);
            touch(t, e, mk.op, -1, false, false);
            return;
        }
        event(e, r, t, mk.op, std::nullopt, 0);
        switch (o.kind) {
            case VisOp::Lock: {
                auto& ow = owner[static_cast<std::size_t>(o.mutex)];
                ok = ok && z3::implies(e, ow == bv(0, 8));
                ow = z3::ite(e, tid, ow);
                break;
            }
            case VisOp::TryLock: {
                auto& ow = owner[static_cast<std::size_t>(o.mutex)];
                auto fr = ow == bv(0, 8);
                define(t, node, s.dst, e, z3::ite(fr, bv(0, o.width), bv(o.busy, o.width)));
                ow = z3::ite(e && fr, tid, ow);
                break;
            }
            case VisOp::Unlock: {
                auto& ow = owner[static_cast<std::size_t>(o.mutex)];
                const auto& mn = p.mutexes[static_cast<std::size_t>(o.mutex)];
                violation(e && ow != tid, "unlock", "LOCK-DOUBLE-UNLOCK",
                          "unlock of mutex " + mn + " that " + tname(p, t) + " does not hold" + at_line(o.line), o.line,
                          t, "unlock:" + mn + ":" + std::to_string(o.line));
                ow = z3::ite(e, bv(0, 8), ow);
                if (o.cond >= 0) {
                    auto& tk = ticket[static_cast<std::size_t>(t)][static_cast<std::size_t>(o.cond)];
                    tk = z3::ite(e, cgen[static_cast<std::size_t>(o.cond)], tk);
                }
                break;
            }
            case VisOp::CondWait: {
                const auto ci = static_cast<std::size_t>(o.cond);
                ok = ok && z3::implies(e, cgen[ci] != ticket[static_cast<std::size_t>(t)][ci]);
                break;
            }
            case VisOp::CondSignal: {
                auto& gn = cgen[static_cast<std::size_t>(o.cond)];
                gn = z3::ite(e, gn + bv(1, 16), gn);
                break;
            }
            case VisOp::MutexInit: {
                auto& ow = owner[static_cast<std::size_t>(o.mutex)];
                ow = z3::ite(e, bv(0, 8), ow);
                break;
            }
            case VisOp::Create: {
                if (o.copies > 1 || create_slot.count({t, node})) {
                    // looped creation: head = handle address, tail = argument
                    int u = create_slot.at({t, node});
                    if (mk.tail) {
                        auto& ta = targ[static_cast<std::size_t>(u)];
                        ta = z3::ite(e, lookup(t, node, s.args.at(0)), ta);
                        define(t, node, s.dst, e, lookup(t, node, s.args.at(0)));
                        return;
                    }
                    auto& a = active[static_cast<std::size_t>(u)];
                    a = a || e;
                    auto h = lookup(t, node, s.args.at(0));
                    M = z3::ite(e, z3::store(M, h, c.bv_val(static_cast<uint64_t>(u), 64)), M);
                    define(t, node, s.dst, e, h);
                    touch(t, e, mk.op, -1, false, false);
                    return;
                }
                auto& a = active[static_cast<std::size_t>(o.thread)];
                a = a || e;
                auto& ta = targ[static_cast<std::size_t>(o.thread)];
                ta = z3::ite(e, lookup(t, node, s.args.at(0)), ta);
                break;
            }
            case VisOp::Arg: {
                define(t, node, s.dst, e, targ[static_cast<std::size_t>(t)]);
                touch(t, e, mk.op, -1, false, false);
                return;
            }
            case VisOp::Alloc: {
                auto& h = heap.at(heap_of.at({t, node}));
                h.live = z3::ite(e, c.bool_val(true), h.live);
                define(t, node, s.dst, e, c.bv_val(h.id << 32, 64));
                touch(t, e, mk.op, -1, false, false);
                return;
            }
            case VisOp::Free: {
                auto a = lookup(t, node, s.args.at(0));
                z3::expr base_live = c.bool_val(false), base_dead = c.bool_val(false);
                for (auto& h : heap) {
                    auto at = a == c.bv_val(h.id << 32, 64);
                    base_live = base_live || (at && h.live);
                    base_dead = base_dead || (at && !h.live);
                }
                auto null = a == c.bv_val(0, 64);
                violation(e && base_dead, "double-free", "MEM-DOUBLE-FREE",
                          "double free by " + tname(p, t) + at_line(o.line), o.line, t, "dfree:" + std::to_string(mk.op));
                violation(e && !null && !base_live && !base_dead, "invalid-free", "MEM-INVALID-FREE",
                          o.what + " of a pointer that is not the start of a heap allocation by " + tname(p, t) +
                              at_line(o.line),
                          o.line, t, "ifree:" + std::to_string(mk.op));
                for (auto& h : heap) h.live = z3::ite(e && a == c.bv_val(h.id << 32, 64), c.bool_val(false), h.live);
                break;
            }
            case VisOp::Join: {
                auto h = lookup(t, node, s.args.at(0));
                ok = ok && z3::implies(e, joinable(h));
                break;
            }
            default: break;
        }
        if (s.kind == Stmt::Assign && o.kind != VisOp::TryLock && !s.args.empty())
            define(t, node, s.dst, e, lookup(t, node, s.args.at(0)));
        touch(t, e, mk.op, -1, false, false);
    }

    void run_slot(int r, int t) {
        auto& gr = g[static_cast<std::size_t>(t)];
        const auto& fn = p.threads[static_cast<std::size_t>(t)].fn;
        const auto ts = std::to_string(t), rs = std::to_string(r);
        auto END = end_of(t);
        auto& pct = pc[static_cast<std::size_t>(t)];
        z3::expr runnable = active[static_cast<std::size_t>(t)] && !exited && pct != END;
        if (p.atomic_mutex >= 0) {
            // inside another thread's atomic section nobody else runs
            const auto& am = owner[static_cast<std::size_t>(p.atomic_mutex)];
            runnable = runnable && (am == bv(0, 8) || am == bv(static_cast<uint64_t>(t + 1), 8));
        }
        z3::expr cs = c.bv_const(("cs_r" + rs + "_T" + ts).c_str(), 16);
        hard.push_back(z3::implies(runnable, z3::ule(pct, cs) && z3::ule(cs, END)));
        hard.push_back(z3::implies(!runnable, cs == pct));
        std::size_t n_nodes = gr.nodes.size();
        std::vector<std::vector<std::pair<int, z3::expr>>> taken(n_nodes);  // per target: (pred, cond)
        std::vector<z3::expr> ex;
        ex.reserve(n_nodes);
        z3::expr ended = c.bool_val(false), stopped = c.bool_val(false);
        std::vector<std::pair<z3::expr, int>> stops;
        for (std::size_t n = 0; n < n_nodes; ++n) {
            ++seq;
            const auto& nd = gr.nodes[n];
            const auto& bl = gr.blocks[static_cast<std::size_t>(nd.block)];
            auto id = c.bv_val(static_cast<uint64_t>(n), 16);
            z3::expr rch = runnable && pct == id;
            for (auto& [pr, cond] : taken.at(n)) rch = rch || cond;
            z3::expr e = rch && z3::ult(id, cs);
            stops.emplace_back(rch && !z3::ult(id, cs), static_cast<int>(n));
            ex.push_back(e);
            int node = static_cast<int>(n);
            // phis: assigned on the incoming edge taken in this round
            if (!bl.phis.empty()) {
                std::vector<std::pair<int, z3::expr>> newv;
                for (auto& ph : bl.phis) {
                    auto key = inst_key(t, node, ph.dst);
                    z3::expr v = slot(t, key);
                    for (auto& [pr, cond] : taken.at(n)) {
                        int pb = gr.nodes[static_cast<std::size_t>(pr)].block;
                        const Arg* in = nullptr;
                        for (auto& [b, a] : ph.in)
                            if (b == pb) in = &a;
                        if (!in) throw Fail{std::string(laws::ERROR), "internal: phi without incoming for predecessor"};
                        v = z3::ite(cond, lookup(t, pr, *in), v);
                    }
                    newv.emplace_back(ph.dst, v);
                }
                for (auto& [dst, v] : newv) slot(t, inst_key(t, node, dst)) = v;
            }
            for (const auto& s : bl.stmts) {
                switch (s.kind) {
                    case Stmt::Assign: {
                        auto mk = marker_of(fn.vars[static_cast<std::size_t>(s.dst)].name);
                        if (mk.op >= 0) {
                            visible(r, t, node, s, mk, e);
                            break;
                        }
                        define(t, node, s.dst, e, op_expr(t, node, s));
                        break;
                    }
                    case Stmt::Check: {
                        // a path PIR cannot follow (exception/indirect-call soft check): not a violation
                        if (s.prop == "unmodelled" || s.prop == "throw-unmodelled")
                            throw Fail{std::string(laws::NEEDS_HARNESS), "UNENCODED: " + s.msg};
                        auto cond = is1(lookup(t, node, s.args.at(0)));
                        violation(e && cond, s.prop, s.cls, s.msg + " in " + tname(p, t) + at_line(s.line), s.line, t,
                                  s.prop + ":" + s.cls + ":" + std::to_string(s.line));
                        break;
                    }
                    case Stmt::Assume:
                        ok = ok && z3::implies(e, is1(lookup(t, node, s.args.at(0))));
                        break;
                    case Stmt::Alloc:
                        [[fallthrough]];
                    case Stmt::Free:
                        [[fallthrough]];
                    case Stmt::Load:
                        [[fallthrough]];
                    case Stmt::Store:
                        [[fallthrough]];
                    case Stmt::MemCpy:
                        [[fallthrough]];
                    case Stmt::MemSet:
                        [[fallthrough]];
                    case Stmt::StackSave:
                        [[fallthrough]];
                    case Stmt::StackRestore:
                        [[fallthrough]];
                    case Stmt::Revive:
                        break;  // visible ops are handled separately; SSA memory is not encoded here
                }
            }
            switch (bl.term.kind) {
                case Term::Ret: ended = ended || e; break;
                case Term::Stop:
                    ended = ended || e;
                    stopped = stopped || e;
                    break;
                case Term::Jmp:
                    [[fallthrough]];
                case Term::Br: {
                    for (auto& [to, kind] : nd.out) {
                        z3::expr cond = e;
                        if (kind != 0) {
                            auto cv = is1(lookup(t, node, bl.term.cond));
                            cond = e && (kind == 1 ? cv : !cv);
                        }
                        if (to < 0) {
                            ok = ok && !cond;  // unwinding cut: the path is assumed away (bounded)
                            continue;
                        }
                        taken.at(static_cast<std::size_t>(to)).emplace_back(node, cond);
                    }
                    break;
                }
            }
        }
        z3::expr npc = pct;
        for (auto it = stops.rbegin(); it != stops.rend(); ++it)
            npc = z3::ite(it->first, c.bv_val(static_cast<uint64_t>(it->second), 16), npc);
        npc = z3::ite(ended, END, npc);
        pct = npc;
        exited = exited || stopped || (t == 0 ? ended : c.bool_val(false));
    }

    void deadlock_check(int r) {
        ++seq;
        z3::expr all_blocked = c.bool_val(true), live = c.bool_val(false);
        for (std::size_t t = 0; t < p.threads.size(); ++t) {
            auto& gr = g[t];
            int ti = static_cast<int>(t);
            z3::expr blocked = c.bool_val(false);
            for (std::size_t n = 0; n < gr.nodes.size(); ++n) {
                int op = gr.nodes[n].head;
                if (op < 0) continue;
                const auto& o = p.ops[static_cast<std::size_t>(op)];
                auto at = pc[t] == c.bv_val(static_cast<uint64_t>(n), 16);
                if (o.dyn && (o.kind == VisOp::Lock || o.kind == VisOp::CondWait)) {
                    const auto& s = gr.blocks[static_cast<std::size_t>(gr.nodes[n].block)].stmts.at(0);
                    auto a = lookup(ti, static_cast<int>(n), s.args.at(0));
                    blocked = blocked || (at && (o.kind == VisOp::Lock ? z3::select(OwnA, a) != bv(0, 8)
                                                                       : z3::select(CgenA, a) == tkd[t]));
                } else if (o.kind == VisOp::Lock) {
                    blocked = blocked || (at && owner[static_cast<std::size_t>(o.mutex)] != bv(0, 8));
                } else if (o.kind == VisOp::CondWait) {
                    const auto ci = static_cast<std::size_t>(o.cond);
                    blocked = blocked || (at && cgen[ci] == ticket[t][ci]);
                } else if (o.kind == VisOp::Join) {
                    const auto& s = gr.blocks[static_cast<std::size_t>(gr.nodes[n].block)].stmts.at(0);
                    blocked = blocked || (at && !joinable(lookup(ti, static_cast<int>(n), s.args.at(0))));
                }
            }
            auto alive = active[t] && pc[t] != end_of(ti);
            live = live || alive;
            all_blocked = all_blocked && (!alive || blocked);
        }
        VRec v{ok && !exited && live && all_blocked,
               "deadlock",
               "CONC-DEADLOCK",
               "deadlock: every live thread is blocked",
               "deadlock",
               0,
               0,
               seq,
               -1,
               {},
               pc};
        viols.push_back(std::move(v));
        (void)r;
    }

    void encode() {
        std::size_t N = p.threads.size();
        for (auto& th : p.threads) g.push_back(build_graph(th.fn, opt));
        vals.resize(N);
        // heap slots: one per (thread, node) running an allocation site;
        // their number is the allocation bound (reported in extra["heap_slots"]).
        for (std::size_t t = 0; t < N; ++t)
            for (std::size_t nd = 0; nd < g[t].nodes.size(); ++nd) {
                int h = g[t].nodes[nd].head;
                if (h < 0 || p.ops[static_cast<std::size_t>(h)].kind != VisOp::Alloc) continue;
                const auto& o = p.ops[static_cast<std::size_t>(h)];
                heap_of[{static_cast<int>(t), static_cast<int>(nd)}] = heap.size();
                heap.push_back({p.objects.size() + 1 + heap.size(), o.size, c.bool_val(false),
                                o.what + "#" + std::to_string(heap.size()) + at_line(o.line)});
            }
        // a creation site must run at most once (no loops of threads yet):
        // once per graph, and in one thread only. A thread may create
        // threads (nested creation) when the created index is above its own,
        // so the static set of slots is well founded.
        std::map<int, std::size_t> create_in;
        for (std::size_t t = 0; t < N; ++t)
            for (auto& nd : g[t].nodes) {
                if (nd.head < 0) continue;
                const auto& o = p.ops[static_cast<std::size_t>(nd.head)];
                if (o.kind != VisOp::Create) continue;
                auto [it, fresh] = create_in.emplace(nd.head, t);
                if ((!fresh && it->second != t) || o.thread <= static_cast<int>(t))
                    throw Fail{std::string(laws::NEEDS_HARNESS),
                               "UNENCODED: " + o.what + " reached by several threads, or a thread that (indirectly) "
                               "creates itself" + at_line(o.line)};
            }
        // looped creation sites: the k-th unrolled execution creates slot
        // thread + k; more executions than slots is a bound hit (reported)
        for (std::size_t t = 0; t < N; ++t) {
            std::map<int, int> used;
            for (std::size_t nd = 0; nd < g[t].nodes.size(); ++nd) {
                int h = g[t].nodes[nd].head;
                if (h < 0) continue;
                const auto& o = p.ops[static_cast<std::size_t>(h)];
                if (o.kind != VisOp::Create || o.copies <= 1) continue;
                int k = used[h]++;
                if (k >= o.copies)
                    throw Fail{std::string(laws::NEEDS_HARNESS),
                               "UNENCODED: " + o.what + " runs more than " + std::to_string(o.copies) +
                                   " times (thread slots per looped creation site = unwind)" + at_line(o.line)};
                create_slot[{static_cast<int>(t), static_cast<int>(nd)}] = o.thread + k;
            }
        }
        for (std::size_t t = 0; t < N; ++t)
            for (auto& nd : g[t].nodes) {
                if (nd.head < 0) continue;
                const auto& o = p.ops[static_cast<std::size_t>(nd.head)];
                if (o.kind != VisOp::Create || o.copies > 1) continue;
                int cnt = 0;
                for (auto& m : g[t].nodes)
                    if (m.head == nd.head) ++cnt;
                if (cnt > 1 || !nd.ctx.empty())
                    throw Fail{std::string(laws::NEEDS_HARNESS),
                               "UNENCODED: " + o.what + " inside a loop or a function called more than once"
                               " (dynamic thread sets need the PIR memory model)" + at_line(o.line)};
            }
        for (auto& v : p.vars) G.push_back(bv(v.init, v.width));
        for (std::size_t k = 0; k < p.objects.size(); ++k)
            for (auto& in : p.objects[k].init)
                M = z3::store(M, c.bv_val(((static_cast<uint64_t>(k) + 1) << 32) | in.off, 64), c.bv_val(in.bits, 64));
        for (std::size_t m = 0; m < p.mutexes.size(); ++m) owner.push_back(bv(0, 8));
        for (std::size_t k = 0; k < p.conds.size(); ++k) cgen.push_back(bv(0, 16));
        ticket.assign(N, std::vector<z3::expr>(p.conds.size(), bv(0, 16)));
        for (std::size_t t = 0; t < N; ++t) {
            targ.push_back(c.bv_val(0, 64));
            sb_pend.push_back(c.bool_val(false));
            sb_var.push_back(c.bv_val(0, 16));
            sb_val.push_back(c.bv_val(0, 64));
            tkd.push_back(c.bv_val(0, 16));
            pc.push_back(c.bv_val(0, 16));
            active.push_back(c.bool_val(t == 0));
        }
        for (int r = 0; r < opt.rounds; ++r) {
            for (std::size_t t = 0; t < N; ++t) run_slot(r, static_cast<int>(t));
            deadlock_check(r);
        }
        // Lazy-CSeq schedules the main thread once more after the last round,
        // so the code after its joins runs without spending a whole round.
        run_slot(opt.rounds, 0);
        deadlock_check(opt.rounds);
    }

    std::string value_text(const z3::model& m, const z3::expr& v, unsigned w) {
        auto x = m.eval(v, true);
        uint64_t u = 0;
        if (!x.is_numeral_u64(u)) return "?";
        return std::to_string(as_signed(u, w));
    }

    Violation render(const z3::model& m, const VRec& v) {
        Violation out;
        out.kind = v.kind;
        out.cls = v.cls;
        out.msg = v.msg;
        out.line = v.line;
        out.thread = v.thread;
        if (v.other_op) {
            auto x = m.eval(*v.other_op, true);
            uint64_t u = 0;
            if (x.is_numeral_u64(u) && u > 0 && u <= p.ops.size()) {
                const auto& o = p.ops[static_cast<std::size_t>(u - 1)];
                out.msg += " conflicts with the immediately preceding " + o.what + at_line(o.line) +
                           " by another thread (unordered by any lock, join or create)";
            }
        }
        if (!v.pcs.empty()) {
            std::string who;
            for (std::size_t t = 0; t < v.pcs.size(); ++t) {
                uint64_t u = 0;
                if (!m.eval(v.pcs[t], true).is_numeral_u64(u)) continue;
                if (u >= g[t].nodes.size()) continue;  // finished or never started
                if (!m.eval(active[t], true).is_true()) continue;
                int op = g[t].nodes[static_cast<std::size_t>(u)].head;
                if (op < 0) continue;
                const auto& o = p.ops[static_cast<std::size_t>(op)];
                who += (who.empty() ? "" : "; ") + tname(p, static_cast<int>(t)) + " waits in " + op_text(p, o) +
                       at_line(o.line);
                if (!out.line) out.line = o.line;
            }
            if (!who.empty()) out.msg += " (" + who + ")";
        }
        for (auto& ev : events) {
            if (ev.seq > v.seq) continue;
            if (!m.eval(ev.exec, true).is_true()) continue;
            Event step;
            step.round = ev.round;
            step.thread = ev.thread;
            step.op = ev.op;
            const auto& o = p.ops[static_cast<std::size_t>(ev.op)];
            step.line = o.line;
            step.text = "[r" + std::to_string(ev.round) + " " + tname(p, ev.thread) + "] " + op_text(p, o);
            if (ev.val)
                step.text += (o.kind == VisOp::Store ? " := " : " -> ") + value_text(m, *ev.val, ev.width);
            step.text += at_line(o.line);
            out.trace.push_back(std::move(step));
        }
        std::string sched;
        int cur = -1, cur_r = -1;
        for (auto& e : out.trace) {
            const auto& o = p.ops[static_cast<std::size_t>(e.op)];
            if (e.thread != cur || e.round != cur_r) {
                sched += (sched.empty() ? "" : " | ") + std::string("r") + std::to_string(e.round) + " T" +
                         std::to_string(e.thread) + ":";
                cur = e.thread;
                cur_r = e.round;
            } else {
                sched += ",";
            }
            sched += " " + op_text(p, o) + (o.line ? "@" + std::to_string(o.line) : "");
        }
        out.schedule = sched;
        return out;
    }
};

}  // namespace
#endif

Result check(const Program& prog, const Options& opt) {
    Result res;
    int N = static_cast<int>(prog.threads.size());
    res.extra["rounds"] = std::to_string(opt.rounds);
    res.extra["unwind"] = std::to_string(opt.unwind);
    res.extra["threads"] = std::to_string(N);
    res.extra["method"] = opt.tso ? "lazy sequentialisation (Lazy-CSeq), x86-TSO (1-entry store buffers, scalar globals)"
                                  : "lazy sequentialisation (Lazy-CSeq), sequentially consistent";
    res.extra["context_switch_bound"] = std::to_string(opt.rounds * N);  // K*N slots + main's final slot
    if (auto why = escaping_throw(prog); !why.empty()) {
        res.status = std::string(laws::NEEDS_HARNESS);
        res.message = std::move(why);
        return res;
    }
#ifndef PRISM_HAS_Z3
    res.status = std::string(laws::NOTRUN);
    res.message = "z3 not built";
    return res;
#else
    try {
        Enc enc(prog, opt);
        enc.encode();
        res.extra["heap_slots"] = std::to_string(enc.heap.size());
        for (auto& o : prog.ops)
            if (o.kind == VisOp::Create && o.copies > 1)
                res.extra["loop_thread_slots"] = std::to_string(o.copies) + " per looped creation site";
        z3::solver s(enc.c);
        z3::params prm(enc.c);
        prm.set("timeout", static_cast<unsigned>(std::max(1.0, opt.timeout_s) * 1000));
        s.set(prm);
        for (auto& h : enc.hard) s.add(h);
        std::set<std::string> seen;
        for (int round = 0; round < opt.max_findings; ++round) {
            z3::expr any = enc.c.bool_val(false);
            bool some = false;
            for (auto& v : enc.viols)
                if (!seen.count(v.key)) {
                    any = any || v.cond;
                    some = true;
                }
            if (!some) break;
            s.push();
            s.add(any);
            auto r = s.check();
            if (r == z3::unknown) {
                s.pop();
                if (res.violations.empty()) {
                    res.status = std::string(laws::UNKNOWN);
                    res.message = "solver unknown/timeout on the sequentialised program (" +
                                  s.reason_unknown() + ")";
                    return res;
                }
                res.extra["incomplete"] = "later queries timed out; more violations may exist";
                break;
            }
            if (r == z3::unsat) {
                s.pop();
                break;
            }
            auto m = s.get_model();
            const VRec* best = nullptr;
            for (auto& v : enc.viols) {
                if (seen.count(v.key)) continue;
                if (!m.eval(v.cond, true).is_true()) continue;
                if (!best || v.seq < best->seq) best = &v;
            }
            s.pop();
            if (!best) break;  // cannot happen: the model satisfies the disjunction
            seen.insert(best->key);
            res.violations.push_back(enc.render(m, *best));
        }
        if (!res.violations.empty()) {
            res.status = std::string(laws::FAILED);
            res.message = res.violations.at(0).msg;
        } else {
            res.status = std::string(laws::BOUNDED);
            res.message = "no data race, deadlock, assertion failure or UB within " + std::to_string(opt.rounds) +
                          " round(s) of " + std::to_string(N) + " thread(s) (loop unwind " +
                          std::to_string(opt.unwind) +
                          "); a context-switch bound is not a proof";
        }
        bool cut = false;
        for (auto& gr : enc.g) cut = cut || gr.has_cut;
        res.extra["unwind_cut"] = cut ? "loop paths past the unwind bound are cut (assumed away)" : "no loop cut";
    } catch (const Fail& f) {
        res.status = f.status;
        res.message = f.reason;
    } catch (const z3::exception& ex) {
        res.status = std::string(laws::ERROR);
        res.message = std::string("z3: ") + ex.msg();
    }
    return res;
#endif
}

}  // namespace prism::conc
