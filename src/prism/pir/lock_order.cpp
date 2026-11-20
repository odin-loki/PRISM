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
#include <optional>
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

    // The class of an operand: a global's name (also through a getelementptr constant expression whose indices are
    // all zero: the same address), or what `pm` says for a local. A class "&C" is not a lock: it is the address of a
    // stack slot that holds the lock pointer C (a guard() variable, see local_classes).
    static std::string cls_of(const ir::Operand& o, const std::map<std::string, std::string>& pm) {
        if (o.v.kind == ir::Value::Global) return o.v.name;
        if (o.v.kind == ir::Value::ConstExpr && o.v.ce_op == "getelementptr" && !o.v.elems.empty() &&
            o.v.elems[0].v.kind == ir::Value::Global) {
            for (std::size_t k = 1; k < o.v.elems.size(); ++k) {
                const auto& v = o.v.elems[k].v;
                if (!(v.kind == ir::Value::Zero || (v.kind == ir::Value::Int && v.bits == 0))) return "";
            }
            return o.v.elems[0].v.name;
        }
        if (o.v.kind == ir::Value::Local) {
            auto it = pm.find(o.v.name);
            if (it != pm.end()) return it->second;
        }
        return "";
    }
    // a lock class (never a slot address "&C" or a parameter placeholder "#k")
    static std::string lock_cls(const ir::Operand& o, const std::map<std::string, std::string>& pm) {
        auto c = cls_of(o, pm);
        return c.starts_with("&") || c.starts_with("#") ? std::string() : c;
    }

    static bool zero_gep(const ir::Inst& in) {
        if (in.op != "getelementptr" || in.ops.size() < 2) return false;
        for (std::size_t k = 1; k < in.ops.size(); ++k) {
            const auto& v = in.ops[k].v;
            if (!(v.kind == ir::Value::Zero || (v.kind == ir::Value::Int && v.bits == 0))) return false;
        }
        return true;
    }

    // Classes of the locals of f, flow-insensitively, from its parameters' classes (in pm). With --pir-lock-scopes
    // (`scopes`):
    //   - a getelementptr with all-zero indices has its base's class (&lock->rlock is the lock);
    //   - a call of a unit function that returns one of its parameters (spinlock_check, class_mutex_constructor,
    //     see returned_param) has that argument's class;
    //   - an alloca to which only values of one class C are stored is the slot "&C", and a load from it is C
    //     (the guard() / scoped_guard variable whose address the cleanup, class_mutex_destructor, gets);
    //   - a load through a parameter of class "&C" (the destructor's argument) is C.
    // Exact: each rule only names the address a value already has; a slot with two classes gets none.
    bool scopes = false;
    void local_classes(const ir::Function& f, std::map<std::string, std::string>& pm, int depth, bool params_only = false) {
        if (!scopes) return;
        std::set<std::string> allocas;
        for (auto& b : f.blocks)
            for (auto& in : b.insts)
                if (in.op == "alloca" && !in.result.empty()) allocas.insert(in.result);
        // alloca -> the one class stored to it ("" = a store of no class, or two classes). Rebuilt on every pass from
        // the classes known so far. The instrumentation's initial store (@__prism.uninit.*, stage.cpp instrument) and
        // undef / poison are not values the program stores.
        std::map<std::string, std::string> slot;
        std::set<std::string> sentinel;
        for (auto& b : f.blocks)
            for (auto& in : b.insts)
                if (in.op == "call" && !in.result.empty() && in.callee.starts_with("__prism.uninit")) sentinel.insert(in.result);
        auto keep = [&](const std::string& c) { return params_only ? c : (c.starts_with("#") ? std::string() : c); };
        for (bool changed = true; changed;) {
            changed = false;
            auto set = [&](const std::string& r, const std::string& c) {
                if (r.empty() || c.empty() || c.starts_with("&") || pm.count(r)) return;
                pm[r] = c;
                changed = true;
            };
            slot.clear();
            for (auto& b : f.blocks)
                for (auto& in : b.insts) {
                    if (in.op != "store" || in.ops.size() < 2 || in.ops[1].v.kind != ir::Value::Local ||
                        !allocas.count(in.ops[1].v.name))
                        continue;
                    const auto& v = in.ops[0].v;
                    if (v.kind == ir::Value::Undef || v.kind == ir::Value::Poison ||
                        (v.kind == ir::Value::Local && sentinel.count(v.name)))
                        continue;
                    auto c = keep(cls_of(in.ops[0], pm));
                    if (c.starts_with("&")) c.clear();
                    auto [it, fresh] = slot.emplace(in.ops[1].v.name, c);
                    if (!fresh && it->second != c) it->second.clear();
                }
            for (auto& b : f.blocks)
                for (auto& in : b.insts) {
                    if (in.op == "load" && !in.ops.empty() && in.ops[0].v.kind == ir::Value::Local) {
                        if (auto sl = slot.find(in.ops[0].v.name); sl != slot.end()) {
                            set(in.result, sl->second);
                        } else if (auto c = cls_of(in.ops[0], pm); c.starts_with("&")) {
                            set(in.result, c.substr(1));
                        }
                    } else if (zero_gep(in)) {
                        auto c = cls_of(in.ops[0], pm);
                        if (!c.starts_with("&")) set(in.result, c);
                    } else if (in.op == "call" && !in.result.empty() && !in.callee.empty()) {
                        auto k = returned_param(in.callee, depth);
                        if (k && *k < in.ops.size()) {
                            auto c = cls_of(in.ops[*k], pm);
                            if (!c.starts_with("&")) set(in.result, c);
                        }
                    }
                }
        }
        // a slot that holds one lock class, as an operand (its address passed to the cleanup function): "&C"
        for (auto& [a, c] : slot)
            if (!c.empty() && !c.starts_with("#") && !pm.count(a)) pm[a] = "&" + c;
    }

    // k when every `ret` of the unit function g returns its parameter k (or that parameter at offset zero)
    std::map<std::string, std::optional<std::size_t>> ret_memo;
    std::optional<std::size_t> returned_param(const std::string& g, int depth) {
        if (auto it = ret_memo.find(g); it != ret_memo.end()) return it->second;
        ret_memo[g] = std::nullopt;  // while it is computed (recursion): no answer
        auto fi = fns.find(g);
        if (fi == fns.end() || fi->second->is_declaration || fi->second->is_model || depth >= 3) return std::nullopt;
        const auto& h = *fi->second;
        if (h.ret.kind != ir::Type::Ptr) return std::nullopt;
        std::map<std::string, std::string> pm;
        for (std::size_t i = 0; i < h.params.size(); ++i) pm[h.params[i].name] = "#" + std::to_string(i);
        local_classes(h, pm, depth + 1, true);
        std::optional<std::string> r;
        for (auto& b : h.blocks)
            for (auto& in : b.insts) {
                if (in.op != "ret") continue;
                if (in.ops.size() != 1) return std::nullopt;
                const auto c = cls_of(in.ops[0], pm);
                if (!c.starts_with("#") || (r && *r != c)) return std::nullopt;
                r = c;
            }
        if (!r) return std::nullopt;
        ret_memo[g] = static_cast<std::size_t>(std::stoul(r->substr(1)));
        return ret_memo[g];
    }

    // --pir-lock-scopes: an i1 branch condition that is a constant, or an icmp of two integer constants (after
    // mem2reg, `if (__sleep_us)` of readl_poll_timeout(..., 0, ...) is `icmp ne i64 0, 0`): its value. Exact.
    static std::optional<bool> const_cond(const ir::Operand& o, const std::map<std::string, const ir::Inst*>& defs,
                                          const std::map<std::string, std::string>* pm = nullptr) {
        if (o.v.kind == ir::Value::Int) return (o.v.bits & 1) != 0;
        if (o.v.kind == ir::Value::Zero) return false;
        if (o.v.kind != ir::Value::Local) return std::nullopt;
        auto d = defs.find(o.v.name);
        if (d == defs.end()) return std::nullopt;
        const ir::Inst& in = *d->second;
        if (in.op == "icmp" && in.ops.size() == 2) {
            auto val = [](const ir::Operand& x) -> std::optional<uint64_t> {
                if (x.v.kind == ir::Value::Int) return x.v.bits;
                if (x.v.kind == ir::Value::Zero || x.v.kind == ir::Value::Null) return uint64_t{0};
                return std::nullopt;
            };
            // a pointer whose lock class is a global is that global's address: never NULL
            auto is_null = [](const ir::Operand& x) {
                return x.ty.kind == ir::Type::Ptr && (x.v.kind == ir::Value::Null || x.v.kind == ir::Value::Zero);
            };
            if (pm && (in.pred == "eq" || in.pred == "ne")) {
                for (int s = 0; s < 2; ++s) {
                    const auto& x = in.ops[static_cast<std::size_t>(s)];
                    const auto& y = in.ops[static_cast<std::size_t>(1 - s)];
                    if (is_null(y) && x.ty.kind == ir::Type::Ptr && !lock_cls(x, *pm).empty()) return in.pred == "ne";
                }
            }
            auto a = val(in.ops[0]), b = val(in.ops[1]);
            if (!a || !b) return std::nullopt;
            const unsigned w = in.ops[0].ty.kind == ir::Type::Int && in.ops[0].ty.bits ? in.ops[0].ty.bits : 64;
            const uint64_t m = w >= 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1;
            const uint64_t x = *a & m, y = *b & m;
            auto sx = [&](uint64_t v) {
                if (w < 64 && (v >> (w - 1)) & 1) v |= ~m;
                return static_cast<int64_t>(v);
            };
            const auto& p = in.pred;
            if (p == "eq") return x == y;
            if (p == "ne") return x != y;
            if (p == "ult") return x < y;
            if (p == "ule") return x <= y;
            if (p == "ugt") return x > y;
            if (p == "uge") return x >= y;
            if (p == "slt") return sx(x) < sx(y);
            if (p == "sle") return sx(x) <= sx(y);
            if (p == "sgt") return sx(x) > sx(y);
            if (p == "sge") return sx(x) >= sx(y);
        }
        return std::nullopt;
    }

    // the blocks of f reachable from its entry, a branch on a constant condition taking only its live edge
    std::set<std::string> live_blocks(const ir::Function& f) const {
        std::set<std::string> live;
        if (f.blocks.empty()) return live;
        std::map<std::string, const ir::Inst*> defs;
        std::map<std::string, const ir::Block*> by;
        for (auto& b : f.blocks) {
            by[b.name] = &b;
            for (auto& in : b.insts)
                if (!in.result.empty()) defs[in.result] = &in;
        }
        std::vector<std::string> work{f.blocks.front().name};
        while (!work.empty()) {
            auto n = work.back();
            work.pop_back();
            if (!live.insert(n).second) continue;
            auto bi = by.find(n);
            if (bi == by.end()) continue;
            for (auto& in : bi->second->insts) {
                if (in.op == "br") {
                    if (scopes && in.targets.size() == 2 && !in.ops.empty())
                        if (auto c = const_cond(in.ops[0], defs)) {
                            work.push_back(in.targets[*c ? 0 : 1]);
                            continue;
                        }
                    for (auto& t : in.targets) work.push_back(t);
                } else if (in.op == "switch") {
                    for (auto& t : in.targets) work.push_back(t);
                    for (auto& c : in.cases) work.push_back(c.second);
                } else if (in.op == "invoke") {
                    for (auto& t : in.targets) work.push_back(t);
                }
            }
        }
        return live;
    }

    static std::string table_class(const LockContract& c, const ir::Inst& in,
                                   const std::map<std::string, std::string>& pm) {
        std::string cl;
        if (c.lock.starts_with("global:")) cl = c.lock.substr(7);
        else if (c.lock.starts_with("arg:")) {
            std::size_t n = std::stoul(c.lock.substr(4));
            if (n < in.ops.size()) cl = lock_cls(in.ops[n], pm);
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
        local_classes(f, pm, depth);
        std::map<std::string, const ir::Inst*> defs;
        if (scopes)
            for (auto& b : f.blocks)
                for (auto& in : b.insts)
                    if (!in.result.empty()) defs[in.result] = &in;
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
                    std::optional<bool> k;
                    if (scopes && in.targets.size() == 2 && !in.ops.empty()) k = const_cond(in.ops[0], defs, &pm);
                    for (std::size_t ti = 0; ti < in.targets.size(); ++ti)
                        if (!k || ti == (*k ? 0u : 1u))
                            for (auto& h : cur) push(in.targets[ti], h);
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
            std::string cl = in.ops.empty() ? std::string() : lock_cls(in.ops[0], pm);
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
            std::string cl = lock_cls(in.ops[0], pm);
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
        a.local_classes(f, pm, depth);
        // --pir-lock-scopes: a block behind a branch on a constant (readl_poll_timeout's `if (__sleep_us)` with
        // sleep_us 0) is dead code, not reached from the handler
        const auto live = a.scopes ? a.live_blocks(f) : std::set<std::string>{};
        for (const auto& b : f.blocks) {
            if (a.scopes && !live.count(b.name)) continue;
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

LockOrderReport lock_order_report(const ir::Module& m, const std::vector<LockContract>& contracts, bool scopes) {
    Analyzer a;
    a.mod = &m;
    for (auto& f : m.functions) a.fns.emplace(f.name, &f);
    for (auto& c : contracts) a.table[c.function].push_back(&c);
    a.scopes = scopes;
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

namespace {

// Lock effects of a library-model function (is_model) called with constant arguments `cargs` (nullopt: not a
// constant): bit 0 acquire, bit 1 release (the ghost intrinsics __prism_lock_acquire / __prism_lock_release),
// bit 2 a spin acquire, bit 3 a spin release, bit 4 a lock intrinsic whose lock is not the model's parameter 0 or
// whose spin flag is not a constant; bits 5-7: __prism_rcu_add with a positive, negative, non-constant delta; bits
// 8-10: the same for __prism_atomic_add (preempt / bh count).
int model_lock_fx(const ir::Module& m, const ir::Function& g, const std::vector<std::optional<uint64_t>>& cargs,
                  const std::set<std::string>& p0, int depth) {
    if (depth > 8) return 16;
    std::map<std::string, std::optional<uint64_t>> cv;
    for (std::size_t i = 0; i < g.params.size(); ++i) cv[g.params[i].name] = i < cargs.size() ? cargs[i] : std::nullopt;
    auto cval = [&](const ir::Operand& o) -> std::optional<uint64_t> {
        if (o.v.kind == ir::Value::Int) return o.v.bits;
        if (o.v.kind == ir::Value::Zero) return uint64_t{0};
        if (o.v.kind == ir::Value::Local)
            if (auto it = cv.find(o.v.name); it != cv.end()) return it->second;
        return std::nullopt;
    };
    std::map<std::string, const ir::Inst*> defs;
    for (auto& b : g.blocks)
        for (auto& in : b.insts)
            if (!in.result.empty()) defs[in.result] = &in;
    auto sval = [&](const ir::Operand& o) -> std::optional<int64_t> {  // a signed 32-bit delta: constant, or 0 - constant
        auto as32 = [](uint64_t v) { return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(v))); };
        if (auto c = cval(o)) return as32(*c);
        if (o.v.kind != ir::Value::Local) return std::nullopt;
        auto d = defs.find(o.v.name);
        if (d == defs.end() || d->second->op != "sub" || d->second->ops.size() != 2) return std::nullopt;
        auto z = cval(d->second->ops[0]), x = cval(d->second->ops[1]);
        if (!z || *z != 0 || !x) return std::nullopt;
        return -as32(*x);
    };
    int fx = 0;
    for (auto& b : g.blocks)
        for (auto& in : b.insts) {
            if ((in.op != "call" && in.op != "invoke") || in.callee.empty()) continue;
            if (in.callee == "__prism_rcu_add" || in.callee == "__prism_atomic_add") {
                const int sh = in.callee == "__prism_rcu_add" ? 5 : 8;
                auto d = in.ops.empty() ? std::nullopt : sval(in.ops[0]);
                fx |= (!d ? 4 : *d > 0 ? 1 : *d < 0 ? 2 : 0) << sh;
                continue;
            }
            const bool acq = in.callee == "__prism_lock_acquire", rel = in.callee == "__prism_lock_release";
            if (acq || rel) {
                fx |= acq ? 1 : 2;
                const bool from_p0 = !in.ops.empty() && in.ops[0].v.kind == ir::Value::Local && p0.count(in.ops[0].v.name);
                auto sp = in.ops.size() >= 2 ? cval(in.ops[1]) : std::nullopt;
                if (!from_p0 || !sp) fx |= 16;
                else if (*sp != 0) fx |= acq ? 4 : 8;
                continue;
            }
            const auto* h = m.find(in.callee);
            if (!h || h->blocks.empty() || h == &g) continue;
            std::vector<std::optional<uint64_t>> ca;
            std::set<std::string> hp0;
            for (std::size_t i = 0; i < in.ops.size(); ++i) {
                ca.push_back(cval(in.ops[i]));
                if (i < h->params.size() && in.ops[i].v.kind == ir::Value::Local && p0.count(in.ops[i].v.name))
                    hp0.insert(h->params[i].name);
            }
            fx |= model_lock_fx(m, *h, ca, hp0, depth + 1);
        }
    return fx;
}

struct SiteWalk {
    const ir::Module& m;
    Analyzer a;
    LockSites out;
    std::set<std::pair<std::string, std::vector<std::string>>> seen;
    explicit SiteWalk(const ir::Module& mm) : m(mm) {
        for (auto& f : m.functions) a.fns.emplace(f.name, &f);
        a.scopes = true;
    }
    void walk(const ir::Function& f, const std::vector<std::string>& pcls, int depth) {
        if (!seen.insert({f.name, pcls}).second) return;
        if (depth > 24) {
            out.unknown_acquire = out.unknown_release = true;
            out.rcu |= 4;
            out.preempt |= 4;
            return;
        }
        std::map<std::string, std::string> pm;
        for (std::size_t i = 0; i < f.params.size(); ++i)
            pm[f.params[i].name] = i < pcls.size() ? pcls[i] : std::string();
        a.local_classes(f, pm, 0);
        for (auto& b : f.blocks)
            for (auto& in : b.insts) {
                if (in.op != "call" && in.op != "invoke") continue;
                if (in.callee.empty()) {  // an indirect call: its targets are not walked, so nothing is release- or acquire-only
                    if (in.callee_op) {
                    out.unknown_acquire = out.unknown_release = true;
                    out.rcu |= 4;
                    out.preempt |= 4;
                }
                    continue;
                }
                if (in.callee == "__prism_lock_acquire" || in.callee == "__prism_lock_release" ||
                    in.callee == "__prism_lock_entry" || in.callee == "__prism_lock_exit" ||
                    in.callee == "__prism_rcu_add" || in.callee == "__prism_atomic_add") {
                    // an intrinsic in analysed code: its lock is not classified here
                    out.unknown_acquire = out.unknown_release = true;
                    out.rcu |= 4;
                    out.preempt |= 4;
                    continue;
                }
                const auto* g = m.find(in.callee);
                if (!g || g->blocks.empty()) continue;
                if (g->is_model) {
                    std::vector<std::optional<uint64_t>> ca;
                    for (auto& o : in.ops)
                        ca.push_back(o.v.kind == ir::Value::Int ? std::optional<uint64_t>(o.v.bits)
                                     : o.v.kind == ir::Value::Zero ? std::optional<uint64_t>(0)
                                                                    : std::nullopt);
                    std::set<std::string> p0;
                    if (!g->params.empty()) p0.insert(g->params[0].name);
                    const int fx = model_lock_fx(m, *g, ca, p0, 0);
                    out.rcu |= (fx >> 5) & 7;
                    out.preempt |= (fx >> 8) & 7;
                    if (!(fx & 3)) continue;
                    const std::string cl = in.ops.empty() ? std::string() : Analyzer::lock_cls(in.ops[0], pm);
                    const bool known = !cl.empty() && !(fx & 16);
                    if (fx & 1) {
                        if (known) out.by_class[cl] |= 1 | ((fx & 4) ? 4 : 0);
                        else out.unknown_acquire = true;
                    }
                    if (fx & 2) {
                        if (known) out.by_class[cl] |= 2 | ((fx & 8) ? 8 : 0);
                        else out.unknown_release = true;
                    }
                    continue;
                }
                std::vector<std::string> pc;
                for (auto& o : in.ops) pc.push_back(Analyzer::cls_of(o, pm));
                walk(*g, pc, depth + 1);
            }
    }
};

}  // namespace

LockSites lock_sites(const ir::Module& m, const ir::Function& root) {
    SiteWalk w(m);
    w.walk(root, std::vector<std::string>(root.params.size()), 0);
    return w.out;
}

}  // namespace prism::pir
