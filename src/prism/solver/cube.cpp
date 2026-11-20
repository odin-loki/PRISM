// Cube-and-conquer for hard queries (docs/SOLVERS.md "Cube-and-conquer").
//
// The formula is bit-blasted to CNF (the same Z3 tactic chain as the
// CaDiCaL/Kissat portfolio members), then solved by a dynamic partition tree:
//
//  - a node is a cube, a set of literals over CNF variables, solved as
//    assumptions by an incremental CaDiCaL (linked as a library, one solver
//    per thread, learned clauses kept across cubes) under a conflict budget;
//  - a cube that runs out of budget is split on one variable x into the two
//    children cube+x and cube-x (x from CaDiCaL's lookahead() under the cube,
//    else the highest open input bit), with 1.5 x the budget; while fewer
//    cubes are open than threads, nodes are split before they are solved;
//  - an UNSAT cube's failed assumptions are an UNSAT core: every open cube
//    containing a core is closed without solving it (core-guided pruning,
//    after Shapiro, Lerner and Bjorner 2026); an empty core closes the root.
//
// Soundness rules (the same as the portfolio's, docs/SOLVERS.md):
//  - UNSAT only when the tree is finished, every leaf is closed by a solver
//    UNSAT or by a core that is a subset of it, AND cubes_cover() shows the
//    leaf cubes cover every assignment (the clauses "not cube" are
//    unsatisfiable, checked by Z3, independently of the tree bookkeeping);
//  - SAT only through an assignment that satisfies the CNF and whose model
//    evaluates the original formula to true in Z3 (validate_model);
//  - a leaf still open at the deadline (or after a stop) leaves the answer
//    UNKNOWN / TIMEOUT, never UNSAT.
// The GPU (PRISM_CUBE_GPU=1, built with PRISM_CUDA) only runs ProbSAT walkers
// on the whole CNF and on open cubes: it can find a satisfying assignment (a
// counterexample candidate, validated as above), never close a cube.

#include "prism/solver.hpp"
#include "prism/membudget.hpp"
#include "internal.hpp"
#include "query.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef PRISM_HAS_CADICAL_LIB
#include <cadical.hpp>
#endif

namespace prism::solver {

namespace {

double env_double(const char* name, double dflt) {
    const char* e = std::getenv(name);
    if (!e || !*e) return dflt;
    char* end = nullptr;
    const double v = std::strtod(e, &end);
    return end != e ? v : dflt;
}

}  // namespace

// Gate builds only (CMake PRISM_CUBE_BUILD_DEFAULT): what an unset PRISM_CUBE
// means. 0 (every normal build): off. 1: the cube member on, joining at once
// (PRISM_CUBE_AFTER 0), 4 threads. 2: every query by the cube engine alone.
// The soundness gates run in a clean environment, so these builds are how
// they exercise the engine.
#ifndef PRISM_CUBE_BUILD_DEFAULT
#define PRISM_CUBE_BUILD_DEFAULT 0
#endif

CubeOptions resolve_cube_options(const CubeOptions& o) {
    CubeOptions r = o;
    constexpr int build_default = PRISM_CUBE_BUILD_DEFAULT;
    const char* env_on = std::getenv("PRISM_CUBE");
    const bool env_set = env_on && *env_on;
    if (r.enabled < 0) {
        r.enabled = env_set ? ((std::string(env_on) != "0" && std::string(env_on) != "off") ? 1 : 0) : (build_default > 0 ? 1 : 0);
        if (!env_set && build_default == 2) r.alone = true;
    }
    if (r.after_s < 0) r.after_s = std::max(0.0, env_double("PRISM_CUBE_AFTER", build_default > 0 && !env_set ? 0.0 : 2.0));
    if (r.threads == 0)
        r.threads = unsigned(std::clamp(env_double("PRISM_CUBE_THREADS", build_default > 0 && !env_set ? 4.0 : 8.0), 1.0, 256.0));
    if (r.conflicts <= 0) r.conflicts = static_cast<long long>(std::max(10.0, env_double("PRISM_CUBE_CONFLICTS", 5000.0)));
    if (r.split.empty()) {
        const char* e = std::getenv("PRISM_CUBE_SPLIT");
        r.split = e && std::string(e) == "inputs" ? "inputs" : "lookahead";
    }
    if (r.gpu < 0) {
        const char* e = std::getenv("PRISM_CUBE_GPU");
        r.gpu = (e && *e && std::string(e) != "0" && std::string(e) != "off") ? 1 : 0;
    }
    if (r.max_leaves == 0) r.max_leaves = 200000;
    // soundness gates: every query answered by the cube engine alone
    if (const char* e = std::getenv("PRISM_CUBE_ALONE"); e && *e && std::string(e) != "0") r.alone = true;
    if (const char* e = std::getenv("PRISM_CUBE_DEBUG_DROP"); e && *e) r.debug_drop_leaf = std::atoll(e);
    return r;
}

bool cube_engine_available(std::string* what) {
#ifdef PRISM_HAS_CADICAL_LIB
    if (what) *what = std::string("cadical-lib ") + CaDiCaL::Solver::version();
    return true;
#else
    if (what) *what = "not built: CaDiCaL library not found at build time (PRISM_CADICAL_DIR; NOTRUN)";
    return false;
#endif
}

bool cubes_cover(const std::vector<std::vector<int>>& cubes, std::string* why) {
    for (const auto& c : cubes)
        if (c.empty()) return true;  // the empty cube is the whole space
    if (cubes.empty()) {
        if (why) *why = "no cubes";
        return false;
    }
#ifdef PRISM_HAS_Z3
    try {
        z3::context c;
        z3::solver s(c);
        z3::params p(c);
        p.set("timeout", 120000u);
        s.set(p);
        std::vector<z3::expr> vars;
        auto var = [&](int v) -> z3::expr {
            const std::size_t k = std::size_t(std::abs(v));
            while (vars.size() <= k) vars.push_back(c.bool_const(("c" + std::to_string(vars.size())).c_str()));
            return vars[k];
        };
        for (const auto& cube : cubes) {
            z3::expr_vector neg(c);
            for (int l : cube) {
                if (l == 0) {
                    if (why) *why = "literal 0 in a cube";
                    return false;
                }
                neg.push_back(l > 0 ? !var(l) : var(l));
            }
            s.add(z3::mk_or(neg));
        }
        const auto r = s.check();
        if (r == z3::unsat) return true;
        if (why) {
            if (r == z3::sat) {
                // name one uncovered assignment (its first few literals)
                std::string ex;
                z3::model m = s.get_model();
                int shown = 0;
                for (std::size_t k = 1; k < vars.size() && shown < 12; ++k) {
                    z3::expr val = m.eval(vars[k], false);
                    if (val.is_true() || val.is_false()) {
                        ex += (shown ? " " : "") + std::string(val.is_true() ? "" : "-") + std::to_string(k);
                        ++shown;
                    }
                }
                *why = "the cubes do not cover the space (uncovered: " + ex + " ...)";
            } else {
                *why = "cover check unknown: " + s.reason_unknown();
            }
        }
        return false;
    } catch (const z3::exception& e) {
        if (why) *why = std::string("cover check failed: ") + e.msg();
        return false;
    }
#else
    if (why) *why = "cover check needs Z3";
    return false;
#endif
}

#ifdef PRISM_HAS_Z3

namespace {

using Kind = SolveResult::Kind;
using detail::now_s;

struct Node {
    int parent = -1;
    int depth = 0;
    std::vector<int> lits;  // the cube (assumptions)
    long long budget = 0;   // conflicts
    enum St { Queued, Running, Split, ClosedSolver, ClosedCore } st = Queued;
    int core = -1;          // closing core (index into Tree::cores)
    int kids[2] = {-1, -1};
    int split_lit = 0;
    bool eager_done = false;  // already offered for an eager split
};

struct Tree {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Node> nodes;
    std::deque<int> queue;
    std::vector<std::vector<int>> cores;  // sorted
    unsigned running = 0;
    bool finished = false;
    bool root_unsat = false;              // an empty core: the formula alone is unsat
    bool sat = false;
    std::vector<std::int8_t> assignment;  // the satisfying assignment (sat)
    std::string sat_by;
    std::vector<std::string> notes;
    std::size_t leaves = 1;
    CubeStats st;
};

bool subset_sorted(const std::vector<int>& small, const std::vector<int>& big_sorted) {
    return std::includes(big_sorted.begin(), big_sorted.end(), small.begin(), small.end());
}

std::vector<int> sorted(std::vector<int> v) {
    std::sort(v.begin(), v.end());
    return v;
}

// index of a core contained in the cube, -1 if none (lock held)
int covering_core(const Tree& t, const std::vector<int>& lits) {
    const auto s = sorted(lits);
    for (std::size_t k = 0; k < t.cores.size(); ++k)
        if (subset_sorted(t.cores[k], s)) return int(k);
    return -1;
}

// Input bits, most significant first: the fallback split order.
std::vector<int> input_split_order(const Cnf& cnf) {
    std::vector<int> order;
    std::size_t maxw = 0;
    for (const auto& s : cnf.symbols) maxw = std::max(maxw, s.vars.size());
    for (std::size_t i = 0; i < maxw; ++i)
        for (const auto& s : cnf.symbols)
            if (s.vars.size() > i) {
                const int v = s.vars[s.vars.size() - 1 - i];
                if (v > 0) order.push_back(v);
            }
    return order;
}

#ifdef PRISM_HAS_CADICAL_LIB
struct Term : CaDiCaL::Terminator {
    const std::atomic<bool>* a = nullptr;
    const std::atomic<bool>* b = nullptr;
    double deadline = 0;
    bool terminate() override { return (a && a->load()) || (b && b->load()) || now_s() > deadline; }
};
#endif

}  // namespace

CubeCnfAnswer cube_solve_cnf(const Cnf& cnf, const CubeOptions& co_in, std::uint64_t seed, double timeout_s,
                             const std::atomic<bool>* stop, CubeStats& stats) {
    const double t0 = now_s();
    const double deadline = t0 + std::max(0.01, timeout_s);
    const CubeOptions co = resolve_cube_options(co_in);
    CubeCnfAnswer res;
    stats.ran = true;
    std::vector<std::string> notes;
    auto finish = [&](CubeCnfAnswer& r) -> CubeCnfAnswer {
        stats.wall_s = now_s() - t0;
        std::string n;
        for (const auto& x : notes) n += (n.empty() ? "" : "; ") + x;
        r.note = n;
        return r;
    };
    if (!cube_engine_available(&stats.engine)) {
        notes.push_back("cube: " + stats.engine);
        return finish(res);
    }
#ifdef PRISM_HAS_CADICAL_LIB
    stats.cnf_vars = cnf.num_vars;
    stats.cnf_clauses = cnf.clauses.size();
    for (const auto& cl : cnf.clauses)
        if (cl.empty()) {
            res.kind = CubeCnfAnswer::Unsat;  // the empty cube, closed by the empty clause
            res.by = "cnf-empty-clause";
            stats.leaves = 1;
            stats.cover_checked = true;
            notes.push_back("cube: the CNF has an empty clause");
            return finish(res);
        }
    if ((stop && stop->load()) || now_s() > deadline) {
        notes.push_back("cube: stopped before solving");
        return finish(res);
    }

    // ---------------------------------------------------------------- threads (memory)
    std::size_t lits_total = 0;
    for (const auto& cl : cnf.clauses) lits_total += cl.size();
    // CaDiCaL holds a clause in about 24 bytes + 4 per literal, watches and
    // per-variable state on top; this estimate is deliberately generous.
    const std::uint64_t per_solver = 16ull * lits_total + 96ull * cnf.clauses.size() + 400ull * std::uint64_t(cnf.num_vars) + (8ull << 20);
    unsigned threads = co.threads;
    if (const auto budget = prism::membudget::budget_mb(); budget > 0) {
        const auto used = prism::membudget::tree_rss_mb();
        const std::uint64_t room = budget > used ? (budget - used) * 7 / 10 : 0;
        const auto fit = unsigned(std::max<std::uint64_t>(1, (room << 20) / std::max<std::uint64_t>(1, per_solver)));
        if (fit < threads) {
            notes.push_back("cube: " + std::to_string(fit) + " of " + std::to_string(threads) +
                            " threads (memory budget " + std::to_string(budget) + " MB, about " +
                            std::to_string(per_solver >> 20) + " MB per solver)");
            threads = fit;
        }
    }
    stats.threads = threads;

    Tree t;
    t.nodes.push_back(Node{});
    t.nodes[0].budget = co.conflicts;
    t.queue.push_back(0);
    std::atomic<bool> halt{false};
    const auto split_order = input_split_order(cnf);
    const bool use_lookahead = co.split != "inputs";
    const unsigned want_open = std::max(2u, 2 * threads);
    std::atomic<double> load_max{0}, split_sum{0}, conquer_sum{0};
    auto add_atomic = [](std::atomic<double>& a, double d) {
        double cur = a.load();
        while (!a.compare_exchange_weak(cur, cur + d)) {}
    };

    auto pick_split = [&](CaDiCaL::Solver& s, const std::vector<int>& lits) -> int {
        std::unordered_set<int> in;
        for (int l : lits) in.insert(std::abs(l));
        if (use_lookahead) {
            for (int l : lits) s.assume(l);
            const int x = s.lookahead();
            s.reset_assumptions();
            if (x != 0 && std::abs(x) <= cnf.num_vars && !in.count(std::abs(x))) return x;
        }
        // a variable that occurs in no clause would split nothing
        const int known = std::min(cnf.num_vars, s.vars());
        for (int v : split_order)
            if (v <= known && !in.count(v)) return v;
        for (int v = 1; v <= known; ++v)
            if (!in.count(v)) return v;
        return 0;
    };

    // Make the two children of node id (lock held).
    auto split_node = [&](int id, int x, long long budget) {
        for (int k = 0; k < 2; ++k) {
            Node kid;
            kid.parent = id;
            kid.depth = t.nodes[id].depth + 1;
            kid.lits = t.nodes[id].lits;
            kid.lits.push_back(k == 0 ? x : -x);
            kid.budget = budget;
            const int kid_id = int(t.nodes.size());
            t.nodes[id].kids[k] = kid_id;
            t.nodes.push_back(std::move(kid));
            t.queue.push_back(kid_id);
        }
        t.nodes[id].st = Node::Split;
        t.nodes[id].split_lit = x;
        ++t.leaves;
        ++stats.splits;
        stats.max_depth = std::max<std::size_t>(stats.max_depth, std::size_t(t.nodes[id].depth + 1));
    };

    auto worker = [&](unsigned wid) {
        CaDiCaL::Solver s;
        Term term;
        term.a = &halt;
        term.b = stop;
        term.deadline = deadline;
        s.connect_terminator(&term);
        s.set("quiet", 1);
        {
            const double l0 = now_s();
            std::size_t k = 0;
            for (const auto& cl : cnf.clauses) {
                for (int l : cl) s.add(l);
                s.add(0);
                // a 1M-clause CNF takes seconds per solver: stop on time
                if ((++k & 0xffff) == 0 && term.terminate()) {
                    std::lock_guard<std::mutex> lk(t.mu);
                    t.finished = true;
                    halt.store(true);
                    t.cv.notify_all();
                    return;
                }
            }
            const double d = now_s() - l0;
            double cur = load_max.load();
            while (d > cur && !load_max.compare_exchange_weak(cur, d)) {}
        }
        (void)wid;
        for (;;) {
            int id = -1;
            std::vector<int> lits;
            long long budget = 0;
            bool eager = false;
            {
                std::unique_lock<std::mutex> lk(t.mu);
                for (;;) {
                    if (t.finished) return;
                    if ((stop && stop->load()) || now_s() > deadline) {
                        t.finished = true;
                        halt.store(true);
                        t.cv.notify_all();
                        return;
                    }
                    if (!t.queue.empty()) break;
                    if (t.running == 0) {
                        t.finished = true;  // every leaf closed
                        t.cv.notify_all();
                        return;
                    }
                    t.cv.wait_for(lk, std::chrono::milliseconds(20));
                }
                id = t.queue.front();
                t.queue.pop_front();
                Node& n = t.nodes[id];
                if (const int k = covering_core(t, n.lits); k >= 0) {
                    n.st = Node::ClosedCore;
                    n.core = k;
                    ++stats.closed_core;
                    continue;
                }
                eager = !n.eager_done && t.queue.size() + t.running + 1 < want_open && t.leaves < co.max_leaves;
                n.eager_done = true;
                n.st = Node::Running;
                ++t.running;
                lits = n.lits;
                budget = n.budget;
            }
            if (eager) {
                const double s0 = now_s();
                const int x = pick_split(s, lits);
                add_atomic(split_sum, now_s() - s0);
                std::lock_guard<std::mutex> lk(t.mu);
                --t.running;
                if (x != 0 && !halt.load()) split_node(id, x, budget);
                else {
                    t.nodes[id].st = Node::Queued;
                    t.queue.push_front(id);
                }
                t.cv.notify_all();
                continue;
            }
            for (int l : lits) s.assume(l);
            s.limit("conflicts", int64_t(std::min<long long>(budget, (1ll << 40))));
            const double c0 = now_s();
            const int r = s.solve();
            add_atomic(conquer_sum, now_s() - c0);
            if (r == 20) {
                std::vector<int> core;
                for (int l : lits)
                    if (s.failed(l)) core.push_back(l);
                std::lock_guard<std::mutex> lk(t.mu);
                ++stats.solver_calls;
                --t.running;
                Node& n = t.nodes[id];
                n.st = Node::ClosedSolver;
                ++stats.closed_solver;
                core = sorted(std::move(core));
                // the core is a subset of the cube by construction (checked
                // again before the answer)
                n.core = int(t.cores.size());
                t.cores.push_back(core);
                ++stats.cores;
                if (core.empty()) {
                    t.root_unsat = true;
                    t.finished = true;
                    halt.store(true);
                }
                t.cv.notify_all();
                continue;
            }
            if (r == 10) {
                std::vector<std::int8_t> a(std::size_t(cnf.num_vars) + 1, 0);
                const int known = s.vars();  // variables CaDiCaL has seen (others occur in no clause)
                for (int v = 1; v <= cnf.num_vars && v <= known; ++v) a[std::size_t(v)] = s.val(v) > 0 ? 1 : 0;
                const bool ok = assignment_satisfies(cnf, a);
                std::lock_guard<std::mutex> lk(t.mu);
                ++stats.solver_calls;
                --t.running;
                if (ok && !t.sat) {
                    t.sat = true;
                    t.sat_by = "cadical";
                    t.assignment = std::move(a);
                    t.finished = true;
                    halt.store(true);
                } else if (!ok) {
                    // left open: the answer stays unknown
                    t.notes.push_back("cube: a CaDiCaL assignment does not satisfy the CNF (cube left open)");
                    t.nodes[id].st = Node::Queued;
                    t.finished = true;
                    halt.store(true);
                }
                t.cv.notify_all();
                continue;
            }
            // r == 0: budget spent, or stopped
            if (halt.load() || (stop && stop->load()) || now_s() > deadline) {
                std::lock_guard<std::mutex> lk(t.mu);
                ++stats.solver_calls;
                --t.running;
                t.nodes[id].st = Node::Queued;  // open
                t.queue.push_back(id);
                t.cv.notify_all();
                continue;
            }
            const double s0 = now_s();
            const int x = t.leaves < co.max_leaves ? pick_split(s, lits) : 0;
            add_atomic(split_sum, now_s() - s0);
            std::lock_guard<std::mutex> lk(t.mu);
            ++stats.solver_calls;
            --t.running;
            const long long nb = static_cast<long long>(std::min(1e12, double(budget) * 1.5));
            if (x != 0 && t.leaves < co.max_leaves) {
                split_node(id, x, nb);
            } else {
                t.nodes[id].budget = nb;  // no split left: more budget
                t.nodes[id].st = Node::Queued;
                t.queue.push_back(id);
            }
            t.cv.notify_all();
        }
    };

    // ---------------------------------------------------------------- GPU walkers (SAT only)
    std::thread gpu_thread;
    stats.gpu = co.gpu ? "" : "off";
#ifdef PRISM_HAS_CUDA
    if (co.gpu) {
        gpu_thread = std::thread([&] {
            std::uint64_t gseed = seed * 7919 + 1;
            bool whole = true;
            while (!halt.load() && !(stop && stop->load()) && now_s() < deadline) {
                std::vector<int> fix;
                if (!whole) {
                    std::lock_guard<std::mutex> lk(t.mu);
                    if (!t.queue.empty()) fix = t.nodes[t.queue.back()].lits;
                }
                whole = !whole;
                // The CNF restricted to the cube: satisfied clauses dropped,
                // false literals removed (a clause emptied: skip the cube).
                Cnf sub;
                const Cnf* use = &cnf;
                if (!fix.empty()) {
                    std::vector<std::int8_t> val(std::size_t(cnf.num_vars) + 1, -1);
                    for (int l : fix) val[std::size_t(std::abs(l))] = l > 0 ? 1 : 0;
                    sub.num_vars = cnf.num_vars;
                    bool empty = false;
                    for (const auto& cl : cnf.clauses) {
                        std::vector<int> nc;
                        bool sat = false;
                        for (int l : cl) {
                            const auto vv = val[std::size_t(std::abs(l))];
                            if (vv < 0) nc.push_back(l);
                            else if ((l > 0) == (vv == 1)) { sat = true; break; }
                        }
                        if (sat) continue;
                        if (nc.empty()) { empty = true; break; }
                        sub.clauses.push_back(std::move(nc));
                    }
                    if (empty) continue;
                    use = &sub;
                }
                std::vector<std::int8_t> a;
                CudaProbsatInfo info;
                const double g0 = now_s();
                const bool found = cuda_probsat_ex(*use, gseed++, 200000, 2048, a, &info);
                {
                    std::lock_guard<std::mutex> lk(t.mu);
                    stats.gpu_s += now_s() - g0;
                    ++stats.gpu_calls;
                    stats.gpu = "cuda " + info.device + ", up to " + std::to_string(info.walkers) + " walkers";
                }
                if (!info.error.empty()) {

                    if (fix.empty()) {
                        std::lock_guard<std::mutex> lk(t.mu);
                        stats.gpu = "cuda: " + info.error;
                        break;  // the whole CNF cannot run on the GPU
                    }
                    continue;
                }
                if (found) {
                    for (int l : fix) a[std::size_t(std::abs(l))] = l > 0 ? 1 : 0;
                    if (assignment_satisfies(cnf, a)) {
                        std::lock_guard<std::mutex> lk(t.mu);
                        if (!t.sat) {
                            t.sat = true;
                            t.sat_by = fix.empty() ? "gpu-probsat" : "gpu-probsat(cube)";
                            t.assignment = std::move(a);
                            t.finished = true;
                            halt.store(true);
                            t.cv.notify_all();
                        }
                        break;
                    }
                }
            }
        });
    }
#else
    if (co.gpu) stats.gpu = "not built (PRISM_CUDA off or nvcc missing; NOTRUN)";
#endif

    std::vector<std::thread> pool;
    for (unsigned w = 0; w < threads; ++w) pool.emplace_back(worker, w);
    for (auto& th : pool) th.join();
    halt.store(true);
    if (gpu_thread.joinable()) gpu_thread.join();
    stats.load_s = load_max.load();
    stats.split_s = split_sum.load();
    stats.conquer_s = conquer_sum.load();
    for (auto& n : t.notes) notes.push_back(n);

    // ---------------------------------------------------------------- answer
    std::size_t open = 0;
    std::vector<std::vector<int>> leaves;
    bool core_ok = true;
    for (const auto& n : t.nodes) {
        if (n.st == Node::Split) continue;
        leaves.push_back(n.lits);
        if (n.st != Node::ClosedSolver && n.st != Node::ClosedCore) ++open;
        else if (n.core < 0 || std::size_t(n.core) >= t.cores.size() ||
                 !subset_sorted(t.cores[std::size_t(n.core)], sorted(n.lits)))
            core_ok = false;
    }
    stats.leaves = leaves.size();
    stats.open = open;
    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "cube: %s, %u threads, CNF %d vars %zu clauses; %zu leaves (%zu closed by solver, %zu by core, %zu open), "
                  "%zu splits, depth %zu, %zu solver calls; blast %.2fs load %.2fs split %.2fs conquer %.2fs (thread sum)",
                  stats.engine.c_str(), threads, cnf.num_vars, cnf.clauses.size(), stats.leaves, stats.closed_solver,
                  stats.closed_core, open, stats.splits, stats.max_depth, stats.solver_calls, stats.blast_s, stats.load_s,
                  stats.split_s, stats.conquer_s);
    notes.push_back(buf);
    if (co.gpu) {
        std::snprintf(buf, sizeof buf, "cube gpu: %s, %zu calls, %.2fs", stats.gpu.c_str(), stats.gpu_calls, stats.gpu_s);
        notes.push_back(buf);
    }

    if (t.sat) {
        // satisfies the CNF (checked when it was found); the caller validates
        // the model against the formula
        res.kind = CubeCnfAnswer::Sat;
        res.assignment = std::move(t.assignment);
        res.by = t.sat_by;
        return finish(res);
    }
    if (t.root_unsat) {
        // an empty core: the CNF alone is unsat; the partition is {empty cube}
        res.kind = CubeCnfAnswer::Unsat;
        res.by = "cadical, empty core";
        stats.cover_checked = cubes_cover({std::vector<int>{}});
        return finish(res);
    }
    if (open > 0 || !t.finished) {
        notes.push_back("cube: " + std::to_string(open) + " of " + std::to_string(leaves.size()) +
                        " cubes open: no answer (never UNSAT with an open cube)");
        return finish(res);
    }
    if (!core_ok) {
        notes.push_back("cube: a leaf's closing core is not a subset of it: no answer");
        return finish(res);
    }
    // Tests: a dropped cube must leave the partition incomplete.
    if (co.debug_drop_leaf >= 0 && std::size_t(co.debug_drop_leaf) < leaves.size()) {
        leaves.erase(leaves.begin() + std::ptrdiff_t(co.debug_drop_leaf));
        notes.push_back("cube: debug: leaf " + std::to_string(co.debug_drop_leaf) + " dropped");
    }
    const double v0 = now_s();
    std::string why;
    const bool covers = cubes_cover(leaves, &why);
    stats.cover_s = now_s() - v0;
    if (!covers) {
        notes.push_back("cube: every leaf closed but " + why + ": no answer");
        return finish(res);
    }
    stats.cover_checked = true;
    res.kind = CubeCnfAnswer::Unsat;
    res.by = "cadical";
    std::snprintf(buf, sizeof buf, "cube: %zu leaf cubes cover the space (checked by Z3 in %.2fs); every leaf UNSAT",
                  leaves.size(), stats.cover_s);
    notes.push_back(buf);
    return finish(res);
#else
    (void)cnf;
    (void)seed;
    (void)timeout_s;
    (void)stop;
    return finish(res);
#endif  // PRISM_HAS_CADICAL_LIB
}

SolveResult cube_and_conquer(z3::context& c, const z3::expr& formula, const SolveOptions& opt, double timeout_s,
                             const std::atomic<bool>* stop) {
    const double t0 = now_s();
    const double deadline = t0 + std::max(0.01, timeout_s);
    SolveResult res;
    res.kind = Kind::Unknown;
    auto finish = [&](std::string note) -> SolveResult {
        res.cube.wall_s = now_s() - t0;
        res.note = std::move(note);
        res.wall_s = res.cube.wall_s;
        return res;
    };
    res.cube.ran = true;
    if (!cube_engine_available(&res.cube.engine)) return finish("cube: " + res.cube.engine);
    // ---------------------------------------------------------------- CNF
    std::optional<Cnf> cnf_o;
    std::string why;
    const double b0 = now_s();
    try {
        z3::context bc;
        z3::expr bf(bc, Z3_translate(c, formula, bc));
        // the tactics poll Z3's cancel flag: interrupt them at the deadline
        std::atomic<bool> blast_done{false};
        std::thread timer([&] {
            while (!blast_done.load()) {
                if ((stop && stop->load()) || now_s() > deadline) {
                    Z3_interrupt(bc);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        try {
            prism::membudget::Phase phase("cube: bit-blast");
            cnf_o = detail::bitblast_fresh(bc, bf, &why);
        } catch (const z3::exception& e) {
            why = std::string("bit-blast: ") + e.msg();
        }
        blast_done.store(true);
        timer.join();
    } catch (const z3::exception& e) {
        why = std::string("bit-blast: ") + e.msg();
    }
    res.cube.blast_s = now_s() - b0;
    if (!cnf_o) {
        res.kind = now_s() > deadline ? Kind::Timeout : Kind::Unknown;
        return finish("cube: no CNF (" + why + ")");
    }
    for (const auto& cl : cnf_o->clauses)
        if (cl.empty()) {
            // the bit-blaster decided the formula: one leaf, the empty cube,
            // closed by the empty clause
            res.kind = Kind::Unsat;
            res.winner = "cube(cnf-empty-clause)";
            res.cube.leaves = 1;
            res.cube.cover_checked = true;
            return finish("cube: the CNF has an empty clause");
        }
    const double blast_s = res.cube.blast_s;
    const auto co = resolve_cube_options(opt.cube);
    auto a = cube_solve_cnf(*cnf_o, co, opt.seed, deadline - now_s(), stop, res.cube);
    res.cube.blast_s = blast_s;
    if (a.kind == CubeCnfAnswer::Sat) {
        auto model = model_from_assignment(*cnf_o, a.assignment);
        if (validate_model(c, formula, model, &why)) {
            res.kind = Kind::Sat;
            res.model = std::move(model);
            res.winner = "cube(" + a.by + ")";
            return finish(a.note);
        }
        return finish(a.note + "; cube: model from " + a.by + " REJECTED by Z3 (" + why + "); no answer");
    }
    if (a.kind == CubeCnfAnswer::Unsat) {
        res.kind = Kind::Unsat;
        res.winner = "cube(" + a.by + ")";
        return finish(a.note);
    }
    res.kind = (stop && stop->load()) ? Kind::Unknown : Kind::Timeout;
    return finish(a.note);
}

#endif  // PRISM_HAS_Z3

}  // namespace prism::solver
