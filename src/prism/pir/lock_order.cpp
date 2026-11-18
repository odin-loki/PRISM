// src/prism/pir/lock_order.cpp (docs/KERNEL_CONCURRENCY.md sections 14 and 15)
//
// Lock-order and interrupt-context passes.
//
// 1. Lock-order inversions (lockdep-style ABBA). Explores every path of each
//    defined function with an ordered held-lock state, inlining defined callees
//    (depth <= 3, memoised) and recording, per acquire, the pair (held, acquired)
//    with the first function that performed it. A class pair seen in both orders
//    is reported once. Lock classes are module global names or struct-field
//    classes (see below); parameters map to the caller's operand class. Bounds:
//    200000 (block, state) steps per call to lock_order_report, then exploration
//    stops silently.
// 2. Lock-order cycles of length 3 to 6: the recorded pairs form a class graph
//    (edge held -> acquired); simple cycles of length >= 3 are enumerated, each
//    once, starting at its smallest class. Bounds: 100 cycles, 100000 DFS
//    expansions, then the search stops silently.
// 3. Interrupt context: handlers registered through request_irq and friends
//    (the hardirq part only: primary handler / filter) are explored with their
//    callees to depth 3. Sleeping primitives reached there are reported
//    (HardirqSleep); spinlock classes taken there and taken without irq
//    disabling in a function outside every hardirq context are reported
//    (LockCtxMismatch).
//
// Struct-field classes: a lock that lives in a struct field (the address is a
// getelementptr result, e.g. &adapter->stats64_lock) gets the lockdep-style
// class "STRUCT.#INDEX": the struct type that directly contains the lock member
// and the member's index. All instances of that struct share the class. A
// contract can name such a class directly with "field:CLASS".
//
// Same-class nesting is not an inversion (lockdep nesting rule); it is ignored,
// there is no explicit nested annotation yet.
//
// Limits: there are no field names (the index is used; no debug info is
// parsed). Parameters' pointee types are not inferred (opaque pointers), so a
// lock reached only through a parameter keeps the caller's class.

#include "prism/pir.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace prism::pir {

namespace {

using Held = std::vector<std::string>;

// Lockdep-style class of a getelementptr that addresses a struct field:
// "<containing struct>.#<field index>" for the last struct descended into.
// The IR carries no field names in this pipeline, so the index is used.
std::string field_class(const ir::Module& m, const ir::Inst& g) {
    ir::Type cur = g.ety;
    std::string containing;
    long field = -1;
    for (std::size_t k = 2; k < g.ops.size(); ++k) {
        const ir::Type* body = nullptr;
        std::string name;
        // a named struct (%struct.S) is a Struct with that text and no elements: its body is in
        // m.types under the name without '%'
        if (!cur.text.empty() && cur.text[0] == '%') {
            auto it = m.types.find(cur.text.substr(1));
            if (it == m.types.end()) it = m.types.find(cur.text);
            if (it != m.types.end() && it->second.kind == ir::Type::Struct) {
                body = &it->second;
                name = cur.text;
            }
        } else if (cur.kind == ir::Type::Struct) {
            body = &cur;  // literal struct: no name, so no class (checked below)
            name = cur.text;
        }
        if (body) {
            const auto& v = g.ops[k].v;
            if (v.kind != ir::Value::Int) return "";
            if (v.bits >= body->elems.size()) return "";
            containing = name;
            field = static_cast<long>(v.bits);
            ir::Type next = body->elems[static_cast<std::size_t>(v.bits)];
            cur = std::move(next);
        } else if (cur.kind == ir::Type::Array) {
            if (cur.elems.empty()) return "";
            ir::Type next = cur.elems[0];
            cur = std::move(next);
        } else {
            return "";
        }
    }
    if (field < 0) return "";
    if (containing.starts_with("%")) containing.erase(0, 1);
    if (containing.starts_with("struct.")) containing.erase(0, 7);
    else if (containing.starts_with("union.")) containing.erase(0, 6);
    if (containing.empty() || containing[0] == '{') return "";  // anonymous (literal) struct: no name
    return containing + ".#" + std::to_string(field);
}

struct Analyzer {
    std::map<std::string, const ir::Function*> fns;
    std::map<std::string, std::vector<const LockContract*>> table;
    std::set<std::string> acquires, releases;
    std::map<std::pair<std::string, std::string>, std::string> pairs;
    std::map<std::tuple<std::string, Held, std::vector<std::string>, int>, std::set<Held>> memo;
    std::map<const ir::Function*, std::map<std::string, std::string>> gcls;
    const ir::Module* mod = nullptr;
    long steps = 0;
    static constexpr long kMaxSteps = 200000;
    // coverage: every acquire / release call site the exploration reached; value = resolved to a class in at least
    // one exploration (a parameter-derived lock resolves only under a caller that passes a class)
    std::map<const ir::Inst*, bool> acq_sites, rel_sites;
    std::map<const ir::Inst*, std::string> site_class;  // first class a site resolved to

    void note_site(const ir::Inst& in, bool is_acquire, const std::string& cl) {
        auto& m = is_acquire ? acq_sites : rel_sites;
        bool& r = m[&in];
        if (!cl.empty()) {
            r = true;
            site_class.emplace(&in, cl);
        }
    }

    static std::string cls_of(const ir::Operand& o, const std::map<std::string, std::string>& pm) {
        if (o.v.kind == ir::Value::Global) return o.v.name;
        if (o.v.kind == ir::Value::Local) {
            auto it = pm.find(o.v.name);
            if (it != pm.end()) return it->second;
        }
        return "";
    }

    static std::string table_class(const LockContract& c, const ir::Inst& in,
                                   const std::map<std::string, std::string>& pm) {
        std::string cl;
        if (c.lock.starts_with("global:")) cl = c.lock.substr(7);
        else if (c.lock.starts_with("arg:")) {
            std::size_t n = std::stoul(c.lock.substr(4));
            if (n < in.ops.size()) cl = cls_of(in.ops[n], pm);
        } else if (c.lock.starts_with("field:")) cl = c.lock.substr(6);
        return cl;
    }

    const std::map<std::string, std::string>& gep_map(const ir::Function& f) {
        auto it = gcls.find(&f);
        if (it != gcls.end()) return it->second;
        std::map<std::string, std::string> g;
        if (mod) {
            for (const auto& b : f.blocks)
                for (const auto& in : b.insts) {
                    if (in.op != "getelementptr" || in.result.empty()) continue;
                    std::string c = field_class(*mod, in);
                    if (!c.empty()) g[in.result] = std::move(c);
                }
        }
        return gcls.emplace(&f, std::move(g)).first->second;
    }

    std::map<std::string, std::string> pm_for(const ir::Function& f, const std::vector<std::string>& pcls) {
        std::map<std::string, std::string> pm = gep_map(f);
        for (std::size_t i = 0; i < f.params.size(); ++i)
            pm[f.params[i].name] = i < pcls.size() ? pcls[i] : std::string();
        return pm;
    }

    void acquire(const std::string& c, Held& h, const std::string& witness) {
        if (c.empty() || std::find(h.begin(), h.end(), c) != h.end()) return;
        for (auto& x : h) pairs.emplace(std::make_pair(x, c), witness);
        h.push_back(c);
    }
    static void release(const std::string& c, Held& h) {
        if (c.empty()) return;
        auto it = std::find(h.begin(), h.end(), c);
        if (it != h.end()) h.erase(it);
    }

    std::set<Held> analyse(const ir::Function& f, const Held& entry, const std::vector<std::string>& pcls, int depth) {
        auto key = std::make_tuple(f.name, entry, pcls, depth);
        auto mi = memo.find(key);
        if (mi != memo.end()) return mi->second;
        std::set<Held> exits;
        if (f.blocks.empty()) return exits;
        std::map<std::string, std::string> pm = pm_for(f, pcls);
        std::map<std::string, std::size_t> bidx;
        for (std::size_t i = 0; i < f.blocks.size(); ++i) bidx.emplace(f.blocks[i].name, i);
        std::set<std::pair<std::size_t, Held>> seen;
        std::vector<std::pair<std::size_t, Held>> work;
        work.emplace_back(0, entry);
        seen.insert(work.back());
        auto push = [&](const std::string& t, const Held& h) {
            auto it = bidx.find(t);
            if (it == bidx.end()) return;
            std::pair<std::size_t, Held> p{it->second, h};
            if (seen.insert(p).second) work.push_back(std::move(p));
        };
        while (!work.empty()) {
            if (steps >= kMaxSteps) break;
            ++steps;
            auto [bi, st] = std::move(work.back());
            work.pop_back();
            std::set<Held> cur{st};
            for (const auto& in : f.blocks[bi].insts) {
                if (in.op == "call" || in.op == "invoke") {
                    std::set<Held> next;
                    for (const auto& h0 : cur) step_call(f, in, pm, h0, depth, next);
                    cur = std::move(next);
                    if (in.op == "invoke") {
                        if (!in.targets.empty())
                            for (auto& h : cur) push(in.targets[0], h);
                        cur.clear();
                    }
                } else if (in.op == "br") {
                    for (auto& t : in.targets)
                        for (auto& h : cur) push(t, h);
                    cur.clear();
                } else if (in.op == "switch") {
                    for (auto& h : cur) {
                        if (!in.targets.empty()) push(in.targets[0], h);
                        for (auto& c : in.cases) push(c.second, h);
                    }
                    cur.clear();
                } else if (in.op == "ret") {
                    for (auto& h : cur) exits.insert(h);
                    cur.clear();
                }
                if (cur.empty()) break;
            }
        }
        memo[key] = exits;
        return exits;
    }

    void step_call(const ir::Function& f, const ir::Inst& in, const std::map<std::string, std::string>& pm,
                   const Held& h0, int depth, std::set<Held>& next) {
        const std::string& cal = in.callee;
        if (cal.empty()) { next.insert(h0); return; }
        Held h = h0;
        auto tb = table.find(cal);
        bool event = false;
        if (tb != table.end()) {
            for (auto* c : tb->second) {
                if (c->action != "acquires" && c->action != "releases") continue;
                std::string cl = table_class(*c, in, pm);
                event = true;
                note_site(in, c->action == "acquires", cl);
                if (c->action == "acquires") acquire(cl, h, f.name);
                else release(cl, h);
            }
        } else if (acquires.count(cal) || releases.count(cal)) {
            event = true;
            std::string cl = in.ops.empty() ? std::string() : cls_of(in.ops[0], pm);
            note_site(in, acquires.count(cal) != 0, cl);
            if (acquires.count(cal)) acquire(cl, h, f.name);
            else release(cl, h);
        }
        if (event) { next.insert(h); return; }
        auto fi = fns.find(cal);
        if (fi == fns.end() || fi->second->is_declaration || fi->second->is_model || depth >= 3) {
            next.insert(h0);
            return;
        }
        std::vector<std::string> pc;
        for (auto& o : in.ops) pc.push_back(cls_of(o, pm));
        auto ex = analyse(*fi->second, h0, pc, depth + 1);
        for (auto& e : ex) next.insert(e);
    }

    // ---- interrupt context helpers ----

    static std::string contract_class(const LockContract& c, const ir::Inst& in,
                                      const std::map<std::string, std::string>& pm) {
        return table_class(c, in, pm);
    }

    // Spin acquires of one call. process == false: every spin acquire (hardirq side);
    // process == true: only those that leave interrupts enabled.
    std::vector<std::string> spin_classes(const ir::Inst& in, const std::map<std::string, std::string>& pm,
                                          bool process) const {
        std::vector<std::string> out;
        const std::string& cal = in.callee;
        auto tb = table.find(cal);
        if (tb != table.end()) {
            for (auto* c : tb->second) {
                if (!c->spin || c->action != "acquires") continue;
                if (process && cal.find("irq") != std::string::npos) continue;
                std::string cl = contract_class(*c, in, pm);
                if (!cl.empty()) out.push_back(std::move(cl));
            }
            return out;
        }
        bool ok;
        if (process) {
            static const std::set<std::string> unsafe{"spin_lock", "_raw_spin_lock", "spin_lock_bh",
                                                      "_raw_spin_lock_bh"};
            ok = unsafe.count(cal) != 0;
        } else {
            ok = acquires.count(cal) && (cal.find("spin") != std::string::npos || cal == "mtx_lock_spin");
        }
        if (ok && !in.ops.empty()) {
            std::string cl = cls_of(in.ops[0], pm);
            if (!cl.empty()) out.push_back(std::move(cl));
        }
        return out;
    }

    bool is_event(const std::string& cal) const {
        auto tb = table.find(cal);
        if (tb != table.end()) {
            for (auto* c : tb->second)
                if (c->action == "acquires" || c->action == "releases") return true;
            return false;
        }
        return acquires.count(cal) || releases.count(cal);
    }
};

bool sleeps_in_hardirq(const ir::Inst& in) {
    static const std::set<std::string> always{"mutex_lock", "mutex_lock_nested", "mutex_lock_interruptible",
                                              "mutex_lock_killable", "msleep", "msleep_interruptible",
                                              "usleep_range", "schedule", "wait_for_completion",
                                              "synchronize_rcu", "__might_sleep", "might_sleep"};
    const std::string& c = in.callee;
    if (always.count(c)) return true;
    std::size_t gi;
    if (c == "kmalloc" || c == "kzalloc") gi = 1;
    else if (c == "krealloc" || c == "kcalloc" || c == "kmalloc_array") gi = 2;
    else return false;
    if (gi >= in.ops.size()) return false;
    const auto& v = in.ops[gi].v;
    return v.kind == ir::Value::Int && (v.bits & 0x400) != 0;
}

struct Hardirq {
    Analyzer& a;
    std::set<std::string> reached;
    std::map<std::string, std::string> classes;  // class -> first handler
    std::vector<HardirqSleep> sleeps;
    std::set<std::tuple<std::string, std::string, std::string>> sleep_seen;
    std::map<std::string, int> visited;
    std::string handler;
    long steps = 0;
    static constexpr long kMax = 200000;

    explicit Hardirq(Analyzer& an) : a(an) {}

    void run(const ir::Function& h) {
        visited.clear();
        handler = h.name;
        walk(h, std::vector<std::string>(h.params.size()), 0);
    }

    void walk(const ir::Function& f, const std::vector<std::string>& pcls, int depth) {
        if (steps >= kMax) return;
        ++steps;
        auto vi = visited.find(f.name);
        if (vi != visited.end() && vi->second <= depth) return;
        visited[f.name] = depth;
        reached.insert(f.name);
        std::map<std::string, std::string> pm = a.pm_for(f, pcls);
        for (const auto& b : f.blocks) {
            for (const auto& in : b.insts) {
                if (in.op != "call" && in.op != "invoke") continue;
                if (in.callee.empty()) continue;
                if (sleeps_in_hardirq(in)) {
                    if (sleep_seen.emplace(f.name, in.callee, handler).second)
                        sleeps.push_back(HardirqSleep{f.name, handler, in.callee});
                    continue;
                }
                for (auto& cl : a.spin_classes(in, pm, false)) classes.emplace(cl, handler);
                if (a.is_event(in.callee)) continue;
                auto fi = a.fns.find(in.callee);
                if (fi == a.fns.end() || fi->second->is_declaration || fi->second->is_model || depth >= 3) continue;
                std::vector<std::string> pc;
                for (auto& o : in.ops) pc.push_back(Analyzer::cls_of(o, pm));
                walk(*fi->second, pc, depth + 1);
            }
        }
    }
};

void find_cycles(const std::map<std::string, std::set<std::string>>& adj,
                 const std::map<std::pair<std::string, std::string>, std::string>& pairs,
                 std::vector<LockOrderCycle>& out) {
    constexpr std::size_t kMaxLen = 6, kMaxCycles = 100;
    constexpr long kMaxExp = 100000;
    long expansions = 0;
    std::vector<std::string> path;
    bool stop = false;
    std::string start;
    auto dfs = [&](auto&& self, const std::string& v) -> void {
        if (stop) return;
        if (++expansions > kMaxExp) { stop = true; return; }
        auto it = adj.find(v);
        if (it == adj.end()) return;
        for (const auto& w : it->second) {
            if (stop) return;
            if (w == start) {
                if (path.size() >= 3) {
                    LockOrderCycle c;
                    c.classes = path;
                    for (std::size_t i = 0; i < path.size(); ++i) {
                        auto p = pairs.find({path[i], path[(i + 1) % path.size()]});
                        c.functions.push_back(p == pairs.end() ? std::string() : p->second);
                    }
                    out.push_back(std::move(c));
                    if (out.size() >= kMaxCycles) { stop = true; return; }
                }
            } else if (w > start && path.size() < kMaxLen &&
                       std::find(path.begin(), path.end(), w) == path.end()) {
                path.push_back(w);
                self(self, w);
                path.pop_back();
            }
        }
    };
    for (const auto& [s, _] : adj) {
        if (stop) break;
        start = s;
        path.assign(1, s);
        dfs(dfs, s);
    }
}

}  // namespace

// Static classification of the lock operand of every acquire / release call in the unit (what the class could
// come from), next to what the exploration actually resolved (Analyzer::acq_sites).
void collect_coverage(const ir::Module& m, Analyzer& a, LockCoverage& cv) {
    for (auto& f : m.functions) {
        if (f.is_declaration || f.is_model) continue;
        ++cv.functions;
        std::map<std::string, const ir::Inst*> def;
        std::set<std::string> params;
        for (auto& p : f.params) params.insert(p.name);
        const auto& gm = a.gep_map(f);
        for (auto& b : f.blocks)
            for (auto& in : b.insts) {
                if (!in.result.empty()) def[in.result] = &in;
                if (in.op == "getelementptr") {
                    ++cv.geps;
                    if (gm.count(in.result)) ++cv.field_geps;
                }
            }
        for (auto& b : f.blocks)
            for (auto& in : b.insts) {
                if (in.op != "call" && in.op != "invoke") continue;
                if (in.callee.empty()) continue;
                if (!a.is_event(in.callee)) {
                    // a lock-like call that the pass does not model (rwlock, semaphore, rtnl_lock, ...): not analysed
                    auto fi = a.fns.find(in.callee);
                    bool defined = fi != a.fns.end() && !fi->second->is_declaration && !fi->second->is_model;
                    const std::string& n = in.callee;
                    if (!defined && (n.find("lock") != std::string::npos || n.find("mutex") != std::string::npos ||
                                     n.starts_with("down") || n.starts_with("up_") || n.find("sem_") != std::string::npos))
                        ++cv.unmodelled_lock_calls[n];
                    continue;
                }
                bool is_acq;
                const ir::Operand* lock = nullptr;
                std::string kind;
                auto tb = a.table.find(in.callee);
                if (tb != a.table.end()) {
                    const LockContract* c = nullptr;
                    for (auto* x : tb->second)
                        if (x->action == "acquires" || x->action == "releases") { c = x; break; }
                    if (!c) continue;
                    is_acq = c->action == "acquires";
                    if (c->lock.starts_with("global:")) kind = "global";
                    else if (c->lock.starts_with("field:")) kind = "field";
                    else if (c->lock.starts_with("arg:")) {
                        std::size_t n = std::stoul(c->lock.substr(4));
                        if (n < in.ops.size()) lock = &in.ops[n];
                        else kind = "unresolved:no-operand";
                    }
                    ++cv.table_sites;
                } else {
                    is_acq = a.acquires.count(in.callee) != 0;
                    if (in.ops.empty()) kind = "unresolved:no-operand";
                    else lock = &in.ops[0];
                }
                if (lock) {
                    const auto& v = lock->v;
                    if (v.kind == ir::Value::Global) kind = "global";
                    else if (v.kind == ir::Value::Local) {
                        if (gm.count(v.name)) kind = "field";
                        else if (params.count(v.name)) kind = "param";
                        else {
                            auto d = def.find(v.name);
                            if (d == def.end()) kind = "unresolved:undefined";
                            else if (d->second->op == "getelementptr") kind = "unresolved:gep-no-class";
                            else if (d->second->op == "load") kind = "unresolved:loaded-pointer";
                            else if (d->second->op == "phi" || d->second->op == "select") kind = "unresolved:phi-select";
                            else if (d->second->op == "alloca") kind = "unresolved:stack-object";
                            else kind = "unresolved:" + d->second->op;
                        }
                    } else kind = "unresolved:constant";
                }
                auto& sites = is_acq ? a.acq_sites : a.rel_sites;
                auto sit = sites.find(&in);
                bool reached = sit != sites.end();
                bool resolved = reached && sit->second;
                if (is_acq) {
                    ++cv.acquire_sites;
                    ++cv.static_kind[kind];
                    if (reached) ++cv.acquire_reached;
                    if (resolved) ++cv.acquire_resolved;
                } else {
                    ++cv.release_sites;
                    if (reached) ++cv.release_reached;
                    if (resolved) ++cv.release_resolved;
                }
                if (resolved) {
                    auto cit = a.site_class.find(&in);
                    if (cit != a.site_class.end()) {
                        bool fld = cit->second.find(".#") != std::string::npos;
                        (fld ? cv.field_classes : cv.global_classes).insert(cit->second);
                    }
                }
            }
    }
    cv.pairs = static_cast<long>(a.pairs.size());
    cv.steps = a.steps;
    cv.step_limit_hit = a.steps >= Analyzer::kMaxSteps;
}

LockOrderReport lock_order_report(const ir::Module& m, const std::vector<LockContract>& contracts) {
    Analyzer a;
    a.mod = &m;
    for (auto& f : m.functions) a.fns.emplace(f.name, &f);
    for (auto& c : contracts) a.table[c.function].push_back(&c);
    for (auto n : {"mutex_lock", "mutex_lock_nested", "mutex_lock_interruptible", "mutex_lock_killable",
                   "_raw_spin_lock", "_raw_spin_lock_irqsave", "_raw_spin_lock_bh", "spin_lock",
                   "spin_lock_irqsave", "spin_lock_bh", "mtx_lock", "mtx_lock_spin", "sx_xlock"})
        a.acquires.insert(n);
    for (auto n : {"mutex_unlock", "_raw_spin_unlock", "_raw_spin_unlock_irqrestore", "_raw_spin_unlock_bh",
                   "spin_unlock", "spin_unlock_irqrestore", "spin_unlock_bh", "mtx_unlock", "mtx_unlock_spin",
                   "sx_xunlock"})
        a.releases.insert(n);
    for (auto& f : m.functions) {
        if (f.is_declaration || f.is_model) continue;
        a.analyse(f, Held{}, std::vector<std::string>(f.params.size()), 0);
    }
    LockOrderReport rep;
    collect_coverage(m, a, rep.coverage);
    for (auto& [k, w] : a.pairs) {
        if (!(k.first < k.second)) continue;
        auto r = a.pairs.find({k.second, k.first});
        if (r == a.pairs.end()) continue;
        LockOrderInversion i;
        i.first = k.first;
        i.second = k.second;
        i.function = r->second;
        i.other = w;
        rep.inversions.push_back(std::move(i));
    }

    // (A) cycles of length >= 3
    {
        std::map<std::string, std::set<std::string>> adj;
        for (auto& [k, w] : a.pairs)
            if (k.first != k.second) adj[k.first].insert(k.second);
        find_cycles(adj, a.pairs, rep.cycles);
    }

    // (B) interrupt context
    static const std::map<std::string, std::size_t> reg{{"request_irq", 1},
                                                        {"request_threaded_irq", 1},
                                                        {"devm_request_irq", 2},
                                                        {"devm_request_threaded_irq", 2},
                                                        {"bus_setup_intr", 3}};
    std::vector<const ir::Function*> handlers;
    std::set<std::string> hseen;
    for (auto& f : m.functions) {
        if (f.is_declaration || f.is_model) continue;
        for (auto& b : f.blocks)
            for (auto& in : b.insts) {
                if (in.op != "call" && in.op != "invoke") continue;
                auto ri = reg.find(in.callee);
                if (ri == reg.end() || ri->second >= in.ops.size()) continue;
                const auto& v = in.ops[ri->second].v;
                if (v.kind != ir::Value::Global) continue;
                auto fi = a.fns.find(v.name);
                if (fi == a.fns.end() || fi->second->is_declaration || fi->second->is_model) continue;
                if (hseen.insert(v.name).second) handlers.push_back(fi->second);
            }
    }
    if (handlers.empty()) return rep;
    Hardirq hi(a);
    for (auto* h : handlers) hi.run(*h);
    rep.hardirq_sleeps = hi.sleeps;
    if (hi.classes.empty()) return rep;
    std::set<std::string> emitted;
    for (auto& f : m.functions) {
        if (f.is_declaration || f.is_model) continue;
        if (hi.reached.count(f.name)) continue;
        const std::map<std::string, std::string> fn_pm = a.pm_for(f, {});
        for (auto& b : f.blocks)
            for (auto& in : b.insts) {
                if (in.op != "call" && in.op != "invoke") continue;
                if (in.callee.empty()) continue;
                for (auto& cl : a.spin_classes(in, fn_pm, true)) {
                    auto hc = hi.classes.find(cl);
                    if (hc == hi.classes.end() || !emitted.insert(cl).second) continue;
                    rep.ctx_mismatches.push_back(LockCtxMismatch{f.name, hc->second, cl});
                }
            }
    }
    return rep;
}

std::vector<LockOrderInversion> lock_order_inversions(const ir::Module& m, const std::vector<LockContract>& contracts) {
    return lock_order_report(m, contracts).inversions;
}

}  // namespace prism::pir
