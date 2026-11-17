// src/prism/pir/lock_order.cpp (docs/KERNEL_CONCURRENCY.md section 14)
//
// Lock-order inversion pass (lockdep-style ABBA). Explores every path of each
// defined function with an ordered held-lock state, inlining defined callees
// (depth <= 3, memoised) and recording, per acquire, the pair (held, acquired)
// with the first function that performed it. A class pair seen in both orders
// is reported once. Lock classes are module global names; parameters map to
// the caller's operand class. Bounds: 200000 (block, state) steps per call to
// lock_order_inversions, then exploration stops silently. Cycles of length 3
// or more are NOT reported (out of scope).

#include "prism/pir.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace prism::pir {

namespace {

using Held = std::vector<std::string>;

struct Analyzer {
    std::map<std::string, const ir::Function*> fns;
    std::map<std::string, std::vector<const LockContract*>> table;
    std::set<std::string> acquires, releases;
    std::map<std::pair<std::string, std::string>, std::string> pairs;
    std::map<std::tuple<std::string, Held, std::vector<std::string>, int>, std::set<Held>> memo;
    long steps = 0;
    static constexpr long kMaxSteps = 200000;

    static std::string cls_of(const ir::Operand& o, const std::map<std::string, std::string>& pm) {
        if (o.v.kind == ir::Value::Global) return o.v.name;
        if (o.v.kind == ir::Value::Local) {
            auto it = pm.find(o.v.name);
            if (it != pm.end()) return it->second;
        }
        return "";
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
        std::map<std::string, std::string> pm;
        for (std::size_t i = 0; i < f.params.size(); ++i)
            pm[f.params[i].name] = i < pcls.size() ? pcls[i] : std::string();
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
                std::string cl;
                if (c->lock.starts_with("global:")) cl = c->lock.substr(7);
                else if (c->lock.starts_with("arg:")) {
                    std::size_t n = std::stoul(c->lock.substr(4));
                    if (n < in.ops.size()) cl = cls_of(in.ops[n], pm);
                }
                event = true;
                if (c->action == "acquires") acquire(cl, h, f.name);
                else release(cl, h);
            }
        } else if (acquires.count(cal) || releases.count(cal)) {
            event = true;
            std::string cl = in.ops.empty() ? std::string() : cls_of(in.ops[0], pm);
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
};

}  // namespace

std::vector<LockOrderInversion> lock_order_inversions(const ir::Module& m, const std::vector<LockContract>& contracts) {
    Analyzer a;
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
    std::vector<LockOrderInversion> out;
    for (auto& [k, w] : a.pairs) {
        if (!(k.first < k.second)) continue;
        auto r = a.pairs.find({k.second, k.first});
        if (r == a.pairs.end()) continue;
        LockOrderInversion i;
        i.first = k.first;
        i.second = k.second;
        i.function = r->second;
        i.other = w;
        out.push_back(std::move(i));
    }
    return out;
}

}  // namespace prism::pir