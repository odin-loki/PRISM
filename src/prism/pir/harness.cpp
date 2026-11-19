// Planner of the automatic pointer harness (harness.hpp, docs/PIR.md
// "Automatic pointer harness"). Pure IR analysis: no solver, no memory model.

#include "harness.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace prism::pir::pirmem {

namespace {

struct Root {
    const ir::Function* fn = nullptr;
    std::string name;
};

struct Use {
    std::optional<ir::Type> struct_ty;  // first struct-typed getelementptr on the pointer itself
    std::optional<ir::Type> scalar_ty;  // scalar type loaded/stored/indexed through the pointer itself
    bool indexed = false;               // a non-constant first index (array of the type)
    uint64_t max_end = 0;               // highest byte (offset + size) accessed at a constant offset
    bool negative = false;              // an access before the start of the object (container_of)
    int64_t min_neg = 0;                // lowest constant offset accessed or derived (negative: container_of)
    std::optional<ir::Type> neg_struct; // struct type of a getelementptr based at a negative offset (the container)
    int64_t neg_base = 0;               // that base's offset from the parameter
    std::map<uint64_t, std::vector<Root>> ptr_fields;  // byte offset -> pointer values loaded from there
    // byte offset -> (struct spelling, field index) of a loaded pointer that is the callee of an indirect call
    std::map<uint64_t, std::pair<std::string, int>> fp_fields;
    // integer values loaded from the object at a constant offset: (function, register) -> (byte offset, bits)
    // (--pir-length-pairing: a count field bounding the index of a pointer field)
    std::map<std::pair<const ir::Function*, std::string>, std::pair<uint64_t, unsigned>> int_loads;
};

// ---------------------------------------------------------------------------
// --pir-length-pairing: (pointer, count) evidence in the IR (harness.hpp LengthPair)
// ---------------------------------------------------------------------------

struct PairEvidence {
    std::string count;
    ir::Type elem;
    uint64_t esz = 0;
    bool is_signed = false;
    std::string how;
};

// count key of a register of the function being searched ("" = not a count)
using CountOf = std::function<std::string(const std::string&)>;

struct FnFacts {
    std::map<std::string, const ir::Inst*> defs;
    std::map<std::string, int> block_of_label;
    std::map<std::string, int> block_of_value;
    std::vector<std::vector<int>> preds;
    std::vector<std::vector<char>> dom;  // dom[b][a]: block a dominates block b
    bool ok = false;
};

FnFacts facts_of(const ir::Function& g) {
    FnFacts f;
    const int n = static_cast<int>(g.blocks.size());
    if (n == 0 || n > 3000) return f;  // a very large function: no evidence (the analysis is quadratic)
    for (int i = 0; i < n; ++i) {
        f.block_of_label[g.blocks[static_cast<std::size_t>(i)].name] = i;
        for (auto& in : g.blocks[static_cast<std::size_t>(i)].insts)
            if (!in.result.empty()) {
                f.defs[in.result] = &in;
                f.block_of_value[in.result] = i;
            }
    }
    f.preds.assign(static_cast<std::size_t>(n), {});
    for (int i = 0; i < n; ++i) {
        const auto& insts = g.blocks[static_cast<std::size_t>(i)].insts;
        if (insts.empty()) continue;
        const auto& t = insts.back();
        std::vector<std::string> succ = t.targets;
        for (auto& c : t.cases) succ.push_back(c.second);
        std::set<int> seen;
        for (auto& s : succ)
            if (auto it = f.block_of_label.find(s); it != f.block_of_label.end() && seen.insert(it->second).second)
                f.preds[static_cast<std::size_t>(it->second)].push_back(i);
    }
    // iterative dominators (blocks are in layout order; the entry is block 0)
    f.dom.assign(static_cast<std::size_t>(n), std::vector<char>(static_cast<std::size_t>(n), 1));
    f.dom[0].assign(static_cast<std::size_t>(n), 0);
    f.dom[0][0] = 1;
    for (int i = 1; i < n; ++i)
        if (f.preds[static_cast<std::size_t>(i)].empty()) {  // unreachable: dominated by nothing but itself
            f.dom[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(n), 0);
            f.dom[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = 1;
        }
    for (bool changed = true; changed;) {
        changed = false;
        for (int b = 1; b < n; ++b) {
            auto& pr = f.preds[static_cast<std::size_t>(b)];
            if (pr.empty()) continue;
            std::vector<char> d(static_cast<std::size_t>(n), 1);
            for (int p : pr)
                for (int a = 0; a < n; ++a) d[static_cast<std::size_t>(a)] &= f.dom[static_cast<std::size_t>(p)][static_cast<std::size_t>(a)];
            d[static_cast<std::size_t>(b)] = 1;
            if (d != f.dom[static_cast<std::size_t>(b)]) {
                f.dom[static_cast<std::size_t>(b)] = std::move(d);
                changed = true;
            }
        }
    }
    f.ok = true;
    return f;
}

// the register under integer casts (sext / zext / trunc / freeze)
std::string strip_casts(const FnFacts& f, const ir::Operand& o) {
    if (o.v.kind != ir::Value::Local) return {};
    std::string n = o.v.name;
    for (int k = 0; k < 8; ++k) {
        auto it = f.defs.find(n);
        if (it == f.defs.end()) break;
        const auto& d = *it->second;
        if ((d.op == "sext" || d.op == "zext" || d.op == "trunc" || d.op == "freeze") && !d.ops.empty() &&
            d.ops[0].v.kind == ir::Value::Local)
            n = d.ops[0].v.name;
        else
            break;
    }
    return n;
}

bool is_const(const ir::Operand& o, int64_t v) {
    if (o.v.kind == ir::Value::Zero) return v == 0;
    if (o.v.kind != ir::Value::Int) return false;
    unsigned w = o.ty.bits ? o.ty.bits : 64;
    uint64_t b = o.v.bits, want = static_cast<uint64_t>(v);
    if (w < 64) {
        const uint64_t m = (uint64_t{1} << w) - 1;
        b &= m;
        want &= m;
    }
    return b == want;
}

// `x pred y` with operands swapped: the same relation
std::string swap_pred(const std::string& p) {
    static const std::map<std::string, std::string> s{{"ult", "ugt"}, {"ugt", "ult"}, {"ule", "uge"}, {"uge", "ule"},
                                                      {"slt", "sgt"}, {"sgt", "slt"}, {"sle", "sge"}, {"sge", "sle"},
                                                      {"eq", "eq"},   {"ne", "ne"}};
    auto it = s.find(p);
    return it == s.end() ? std::string() : it->second;
}

// The block reached only by the edge on which `a pred b` keeps a below b (a < b, a <= b, a != b true; a >= b, a > b,
// a == b false), when that successor has the branch block as its only predecessor; -1 otherwise.
int inbound_successor(const FnFacts& f, int branch_block, const ir::Inst& br, const std::string& pred_a_b) {
    if (br.op != "br" || br.targets.size() != 2) return -1;
    int which = -1;
    if (pred_a_b == "ult" || pred_a_b == "slt" || pred_a_b == "ule" || pred_a_b == "sle" || pred_a_b == "ne") which = 0;
    else if (pred_a_b == "uge" || pred_a_b == "sge" || pred_a_b == "ugt" || pred_a_b == "sgt" || pred_a_b == "eq") which = 1;
    if (which < 0) return -1;
    auto it = f.block_of_label.find(br.targets[static_cast<std::size_t>(which)]);
    if (it == f.block_of_label.end()) return -1;
    const auto& pr = f.preds[static_cast<std::size_t>(it->second)];
    if (pr.size() != 1 || pr[0] != branch_block) return -1;
    return it->second;
}

bool dominates(const FnFacts& f, int a, int b) { return f.dom[static_cast<std::size_t>(b)][static_cast<std::size_t>(a)] != 0; }

// Evidence that the pointer (registers `ptrs` of g) is accessed only within `count` elements in some place.
void pair_evidence(const ir::Module& m, const Layout& lay, const ir::Function& g, const std::set<std::string>& ptrs,
                   const CountOf& count_of, int depth, std::vector<PairEvidence>& out) {
    FnFacts f = facts_of(g);
    if (!f.ok) return;
    const int nb = static_cast<int>(g.blocks.size());
    auto term = [&](int b) -> const ir::Inst* {
        const auto& is = g.blocks[static_cast<std::size_t>(b)].insts;
        return is.empty() ? nullptr : &is.back();
    };
    // (1) indexed access: getelementptr T, P, idx, ... under `idx < count`
    for (int bi = 0; bi < nb; ++bi)
        for (const auto& in : g.blocks[static_cast<std::size_t>(bi)].insts) {
            if (in.op != "getelementptr" || in.ops.size() < 2 || in.ops[0].v.kind != ir::Value::Local ||
                !ptrs.count(in.ops[0].v.name) || in.ops[1].v.kind != ir::Value::Local)
                continue;
            const std::string idx = strip_casts(f, in.ops[1]);
            for (int bj = 0; bj < nb; ++bj) {
                const ir::Inst* br = term(bj);
                if (!br || br->op != "br" || br->ops.empty() || br->ops[0].v.kind != ir::Value::Local) continue;
                auto c = f.defs.find(br->ops[0].v.name);
                if (c == f.defs.end() || c->second->op != "icmp" || c->second->ops.size() != 2) continue;
                const auto& cmp = *c->second;
                std::string pred, count;
                if (strip_casts(f, cmp.ops[0]) == idx && !(count = count_of(strip_casts(f, cmp.ops[1]))).empty())
                    pred = cmp.pred;
                else if (strip_casts(f, cmp.ops[1]) == idx && !(count = count_of(strip_casts(f, cmp.ops[0]))).empty())
                    pred = swap_pred(cmp.pred);
                else
                    continue;
                const int s = inbound_successor(f, bj, *br, pred);
                if (s < 0 || !dominates(f, s, bi)) continue;
                PairEvidence e;
                e.count = count;
                e.elem = in.ety;
                try {
                    e.esz = lay.alloc_size(in.ety);
                } catch (const Unenc&) {
                    continue;
                }
                e.is_signed = pred[0] == 's';
                e.how = "index %" + idx + " " + pred + " the count, block " + g.blocks[static_cast<std::size_t>(s)].name +
                        " (" + g.name + ")";
                out.push_back(std::move(e));
                break;
            }
        }
    // (2) advanced pointer: phi P' = [P, ...], [gep T, P', 1] in a loop header whose exit test counts `count` down to 0
    // (`while (n--)`) or an index up from 0 to `count`
    for (int h = 0; h < nb; ++h) {
        const ir::Inst* br = term(h);
        if (!br || br->op != "br" || br->ops.empty() || br->ops[0].v.kind != ir::Value::Local) continue;
        auto c = f.defs.find(br->ops[0].v.name);
        if (c == f.defs.end() || c->second->op != "icmp" || c->second->ops.size() != 2) continue;
        const auto& hb = g.blocks[static_cast<std::size_t>(h)];
        // the counter phi of this header and its relation to the count
        auto counter = [&](const ir::Operand& o, std::string& count, bool& down) -> bool {
            if (o.v.kind != ir::Value::Local) return false;
            auto d = f.defs.find(o.v.name);
            if (d == f.defs.end() || d->second->op != "phi" || f.block_of_value[o.v.name] != h ||
                d->second->incoming.size() != 2)
                return false;
            const auto& ph = *d->second;
            for (int k = 0; k < 2; ++k) {
                const auto& init = ph.incoming[static_cast<std::size_t>(k)].first;
                const auto& step = ph.incoming[static_cast<std::size_t>(1 - k)].first;
                if (step.v.kind != ir::Value::Local) continue;
                auto sd = f.defs.find(step.v.name);
                if (sd == f.defs.end() || sd->second->ops.size() != 2 || sd->second->ops[0].v.kind != ir::Value::Local ||
                    sd->second->ops[0].v.name != o.v.name)
                    continue;
                const bool dec = (sd->second->op == "add" && is_const(sd->second->ops[1], -1)) ||
                                 (sd->second->op == "sub" && is_const(sd->second->ops[1], 1));
                const bool inc = sd->second->op == "add" && is_const(sd->second->ops[1], 1);
                if (dec && init.v.kind == ir::Value::Local && !(count = count_of(strip_casts(f, init))).empty()) {
                    down = true;
                    return true;
                }
                if (inc && is_const(init, 0)) {
                    down = false;
                    return true;
                }
            }
            return false;
        };
        const auto& cmp = *c->second;
        std::string count, pred;
        bool down = false;
        for (int side = 0; side < 2 && pred.empty(); ++side) {
            const auto& a = cmp.ops[static_cast<std::size_t>(side)];
            const auto& b = cmp.ops[static_cast<std::size_t>(1 - side)];
            const std::string p = side == 0 ? cmp.pred : swap_pred(cmp.pred);
            std::string k;
            bool dn = false;
            if (!counter(a, k, dn)) continue;
            if (dn && is_const(b, 0) && (p == "ne" || p == "ugt" || p == "sgt")) {
                // n > 0 / n != 0: the in-bound edge is the one where n is still positive
                count = k;
                pred = p == "ne" ? "ne" : p;
                down = true;
            } else if (!dn && b.v.kind == ir::Value::Local && !(k = count_of(strip_casts(f, b))).empty() &&
                       (p == "ult" || p == "slt" || p == "ne")) {
                count = k;
                pred = p;
            }
        }
        if (pred.empty()) continue;
        // for the in-bound edge of `n > 0` take the `<` successor rule on (0 < n): the true edge
        const int s = inbound_successor(f, h, *br, down ? "ult" : pred);
        if (s < 0) continue;
        for (const auto& ph : hb.insts) {
            if (ph.op != "phi" || ph.result.empty() || ph.incoming.size() != 2) continue;
            const ir::Type* ety = nullptr;
            bool from_p = false;
            for (int k = 0; k < 2; ++k) {
                const auto& init = ph.incoming[static_cast<std::size_t>(k)].first;
                const auto& step = ph.incoming[static_cast<std::size_t>(1 - k)].first;
                if (init.v.kind != ir::Value::Local || !ptrs.count(init.v.name) || step.v.kind != ir::Value::Local) continue;
                auto sd = f.defs.find(step.v.name);
                if (sd == f.defs.end() || sd->second->op != "getelementptr" || sd->second->ops.size() != 2 ||
                    sd->second->ops[0].v.kind != ir::Value::Local || sd->second->ops[0].v.name != ph.result ||
                    !is_const(sd->second->ops[1], 1))
                    continue;
                ety = &sd->second->ety;
                from_p = true;
            }
            if (!from_p) continue;
            // an access through the advanced pointer in the loop body
            bool used = false;
            for (int bi = 0; bi < nb && !used; ++bi) {
                if (!dominates(f, s, bi)) continue;
                for (const auto& in : g.blocks[static_cast<std::size_t>(bi)].insts) {
                    const ir::Operand* addr = in.op == "load" && !in.ops.empty() ? &in.ops[0]
                                              : in.op == "store" && in.ops.size() >= 2 ? &in.ops[1]
                                                                                       : nullptr;
                    if (addr && addr->v.kind == ir::Value::Local && addr->v.name == ph.result) used = true;
                }
            }
            if (!used) continue;
            PairEvidence e;
            e.count = count;
            e.elem = *ety;
            try {
                e.esz = lay.alloc_size(*ety);
            } catch (const Unenc&) {
                continue;
            }
            e.is_signed = pred[0] == 's';
            e.how = "pointer %" + ph.result + " advanced one element per iteration of the loop at " + hb.name + " (" +
                    (down ? "the count runs down to 0" : "an index runs up to the count") + ", " + g.name + ")";
            out.push_back(std::move(e));
        }
    }
    // (3) the pointer and its count passed together to a module function
    if (depth >= 3) return;
    for (const auto& bl : g.blocks)
        for (const auto& in : bl.insts) {
            if (in.op != "call" || in.callee.empty() || in.callee_op) continue;
            const auto* callee = m.find(in.callee);
            if (!callee || callee->blocks.empty() || callee == &g || callee->is_model) continue;
            std::set<std::string> cp;
            std::map<std::string, std::string> cc;
            for (std::size_t i = 0; i < in.ops.size() && i < callee->params.size(); ++i) {
                const auto& a = in.ops[i];
                if (a.v.kind != ir::Value::Local) continue;
                if (callee->params[i].ty.kind == ir::Type::Ptr && ptrs.count(a.v.name)) cp.insert(callee->params[i].name);
                if (callee->params[i].ty.kind == ir::Type::Int)
                    if (auto k = count_of(strip_casts(f, a)); !k.empty()) cc[callee->params[i].name] = k;
            }
            if (cp.empty() || cc.empty()) continue;
            CountOf sub = [cc](const std::string& r) {
                auto it = cc.find(r);
                return it == cc.end() ? std::string() : it->second;
            };
            pair_evidence(m, lay, *callee, cp, sub, depth + 1, out);
        }
}

// One count and one element size across all the evidence, else nothing.
std::optional<LengthPair> agree(const std::vector<PairEvidence>& ev) {
    if (ev.empty()) return std::nullopt;
    LengthPair lp;
    lp.count = ev.front().count;
    lp.elem = ev.front().elem;
    lp.elem_bytes = ev.front().esz;
    lp.is_signed = true;
    lp.evidence = ev.front().how;
    for (const auto& e : ev) {
        if (e.count != lp.count || e.esz != lp.elem_bytes || e.esz == 0) return std::nullopt;
        lp.is_signed = lp.is_signed && e.is_signed;
    }
    if (ev.size() > 1) lp.evidence += " and " + std::to_string(ev.size() - 1) + " more";
    return lp;
}

int64_t signed_of(const ir::Operand& o) {
    uint64_t b = o.v.bits;
    unsigned w = o.ty.bits ? o.ty.bits : 64;
    if (w < 64) {
        const uint64_t m = (uint64_t{1} << w) - 1;
        b &= m;
        if (b >> (w - 1)) b |= ~m;
    }
    return static_cast<int64_t>(b);
}

// Byte offset of a getelementptr whose indices are all constants.
std::optional<int64_t> const_gep_offset(const Layout& lay, const ir::Inst& in) {
    if (in.ops.size() < 2) return std::nullopt;
    for (std::size_t k = 1; k < in.ops.size(); ++k)
        if (in.ops[k].v.kind != ir::Value::Int && in.ops[k].v.kind != ir::Value::Zero) return std::nullopt;
    auto idx = [](const ir::Operand& o) { return o.v.kind == ir::Value::Zero ? int64_t{0} : signed_of(o); };
    int64_t off = idx(in.ops[1]) * static_cast<int64_t>(lay.alloc_size(in.ety));
    ir::Type cur = in.ety;
    for (std::size_t k = 2; k < in.ops.size(); ++k) {
        const auto& r = lay.resolve(cur);
        const int64_t i = idx(in.ops[k]);
        if (r.kind == ir::Type::Struct) {
            off += static_cast<int64_t>(lay.field_offset(r, static_cast<unsigned>(i)));
            cur = lay.field_type(r, static_cast<unsigned>(i));
        } else if (r.kind == ir::Type::Array && !r.elems.empty()) {
            off += i * static_cast<int64_t>(lay.alloc_size(r.elems.at(0)));
            cur = r.elems.at(0);
        } else {
            return std::nullopt;
        }
    }
    return off;
}

bool is_local(const ir::Operand& o, const std::map<std::string, int64_t>& alias, int64_t* off) {
    if (o.v.kind != ir::Value::Local) return false;
    auto it = alias.find(o.v.name);
    if (it == alias.end()) return false;
    *off = it->second;
    return true;
}

std::optional<int64_t> returned_offset(const ir::Module& m, const Layout& lay, const ir::Function& g, std::size_t pi,
                                       int depth);

// Pointers derived from the roots in `alias` at constant offsets (fixpoint: blocks are not in dominance order).
// With `calls` (--pir-container-of), the result of a call is derived too when the callee returns its pointer argument
// plus one constant offset on every path (a container_of helper such as `to_foo(hw)`; see returned_offset).
void derive_aliases(const ir::Module& m, const Layout& lay, const ir::Function& g, std::map<std::string, int64_t>& alias,
                    int depth, bool calls) {
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& bl : g.blocks)
            for (auto& in : bl.insts) {
                if (in.result.empty() || alias.count(in.result)) continue;
                int64_t a = 0;
                if (in.op == "getelementptr") {
                    if (in.ops.empty() || !is_local(in.ops[0], alias, &a)) continue;
                    try {
                        if (auto d = const_gep_offset(lay, in)) {
                            alias[in.result] = a + *d;
                            changed = true;
                        }
                    } catch (const Unenc&) {
                    }
                } else if (calls && in.op == "call" && !in.callee.empty() && !in.callee_op && depth < 3) {
                    const auto* callee = m.find(in.callee);
                    if (!callee || callee->blocks.empty() || callee == &g) continue;
                    for (std::size_t i = 0; i < in.ops.size() && i < callee->params.size(); ++i) {
                        if (!is_local(in.ops[i], alias, &a)) continue;
                        if (auto r = returned_offset(m, lay, *callee, i, depth + 1)) {
                            alias[in.result] = a + *r;
                            changed = true;
                            break;
                        }
                    }
                }
            }
    }
}

// The constant offset of g's returned pointer from its pointer parameter pi, when every `ret` of g returns a value
// derived from that parameter at one and the same constant offset; nullopt otherwise (the caller then learns nothing).
std::optional<int64_t> returned_offset(const ir::Module& m, const Layout& lay, const ir::Function& g, std::size_t pi,
                                       int depth) {
    if (pi >= g.params.size() || g.blocks.empty() || g.ret.kind != ir::Type::Ptr ||
        g.params[pi].ty.kind != ir::Type::Ptr)
        return std::nullopt;
    std::map<std::string, int64_t> alias{{g.params[pi].name, 0}};
    derive_aliases(m, lay, g, alias, depth, true);
    std::optional<int64_t> off;
    for (auto& bl : g.blocks)
        for (auto& in : bl.insts) {
            if (in.op != "ret") continue;
            int64_t a = 0;
            if (in.ops.size() != 1 || !is_local(in.ops[0], alias, &a)) return std::nullopt;
            if (off && *off != a) return std::nullopt;
            off = a;
        }
    return off;
}

// The byte offset k when every `ret` of g returns the pointer loaded from its pointer parameter pi at offset k (an
// accessor such as `dev_get_drvdata(dev)` or `netdev_priv`-free getters `return p->field;`), directly or through
// another such accessor; nullopt otherwise. The caller's call result is then that pointer field of its object.
std::optional<int64_t> returned_field(const ir::Module& m, const Layout& lay, const ir::Function& g, std::size_t pi,
                                      int depth) {
    if (pi >= g.params.size() || g.blocks.empty() || g.ret.kind != ir::Type::Ptr ||
        g.params[pi].ty.kind != ir::Type::Ptr || depth >= 3)
        return std::nullopt;
    std::map<std::string, int64_t> alias{{g.params[pi].name, 0}};
    derive_aliases(m, lay, g, alias, depth, true);
    std::map<std::string, const ir::Inst*> defs;
    for (auto& bl : g.blocks)
        for (auto& in : bl.insts)
            if (!in.result.empty()) defs[in.result] = &in;
    std::optional<int64_t> off;
    for (auto& bl : g.blocks)
        for (auto& in : bl.insts) {
            if (in.op != "ret") continue;
            if (in.ops.size() != 1 || in.ops[0].v.kind != ir::Value::Local) return std::nullopt;
            auto d = defs.find(in.ops[0].v.name);
            if (d == defs.end()) return std::nullopt;
            const ir::Inst& def = *d->second;
            std::optional<int64_t> k;
            int64_t a = 0;
            if (def.op == "load" && def.ty.kind == ir::Type::Ptr && !def.ops.empty() && is_local(def.ops[0], alias, &a) &&
                a >= 0) {
                k = a;
            } else if (def.op == "call" && !def.callee.empty() && !def.callee_op) {
                const auto* c2 = m.find(def.callee);
                if (c2 && !c2->blocks.empty() && c2 != &g)
                    for (std::size_t i = 0; i < def.ops.size() && i < c2->params.size() && !k; ++i)
                        if (is_local(def.ops[i], alias, &a) && a >= 0)
                            if (auto r = returned_field(m, lay, *c2, i, depth + 1)) k = a + *r;
            }
            if (!k || (off && *off != *k)) return std::nullopt;
            off = k;
        }
    return off;
}

void scan_one(const ir::Module& m, const Layout& lay, const ir::Function& g, const std::string& rootname,
              int64_t base, int depth, bool container, Use& u,
              std::set<std::pair<const ir::Function*, std::string>>& visited) {
    if (!visited.insert({&g, rootname + "@" + std::to_string(base)}).second) return;
    std::map<std::string, int64_t> alias{{rootname, base}};
    derive_aliases(m, lay, g, alias, depth, container);
    // defining instruction of each register, and the registers called through (function-pointer loads)
    std::map<std::string, const ir::Inst*> defs;
    std::set<std::string> called;
    for (auto& bl : g.blocks)
        for (auto& in : bl.insts) {
            if (!in.result.empty()) defs[in.result] = &in;
            if (in.callee_op && in.callee_op->v.kind == ir::Value::Local) called.insert(in.callee_op->v.name);
        }
    for (auto& bl : g.blocks)
        for (auto& in : bl.insts) {
            int64_t a = 0;
            try {
                if (in.op == "getelementptr" && !in.ops.empty() && in.ops[0].v.kind == ir::Value::Local &&
                    in.ops[0].v.name == rootname && base == 0) {
                    const auto& r = lay.resolve(in.ety);
                    if (r.kind == ir::Type::Struct) {
                        if (!u.struct_ty) u.struct_ty = in.ety;
                    } else if (!u.scalar_ty) {
                        u.scalar_ty = in.ety;
                    }
                    if (in.ops.size() >= 2 && in.ops[1].v.kind != ir::Value::Int && in.ops[1].v.kind != ir::Value::Zero)
                        u.indexed = true;
                }
                const ir::Type* acc = nullptr;
                const ir::Operand* addr = nullptr;
                if (in.op == "load" && !in.ops.empty()) {
                    acc = &in.ty;
                    addr = &in.ops[0];
                } else if (in.op == "store" && in.ops.size() >= 2) {
                    acc = &in.ops[0].ty;
                    addr = &in.ops[1];
                }
                if (acc && addr && is_local(*addr, alias, &a)) {
                    if (a < 0) {
                        u.negative = true;
                        u.min_neg = std::min(u.min_neg, a);
                        continue;
                    }
                    const uint64_t sz = lay.store_size(*acc);
                    u.max_end = std::max<uint64_t>(u.max_end, static_cast<uint64_t>(a) + sz);
                    if (base == 0 && addr->v.name == rootname && acc->kind != ir::Type::Ptr && !u.scalar_ty &&
                        (acc->kind == ir::Type::Int || acc->kind == ir::Type::Float))
                        u.scalar_ty = *acc;
                    if (in.op == "load" && acc->kind == ir::Type::Int && !in.result.empty() && acc->bits && acc->bits <= 64)
                        u.int_loads[{&g, in.result}] = {static_cast<uint64_t>(a), acc->bits};
                    if (in.op == "load" && acc->kind == ir::Type::Ptr && !in.result.empty()) {
                        u.ptr_fields[static_cast<uint64_t>(a)].push_back({&g, in.result});
                        if (called.count(in.result) && addr->v.kind == ir::Value::Local) {
                            // load (gep %struct.T, base, 0, K) used as a callee: field K of T
                            if (auto d = defs.find(addr->v.name); d != defs.end() && d->second->op == "getelementptr" &&
                                d->second->ops.size() == 3 && d->second->ops[1].v.kind == ir::Value::Int &&
                                d->second->ops[1].v.bits == 0 && d->second->ops[2].v.kind == ir::Value::Int)
                                u.fp_fields[static_cast<uint64_t>(a)] = {d->second->ety.text,
                                                                         static_cast<int>(d->second->ops[2].v.bits)};
                        }
                    }
                    continue;
                }
                if (in.op == "getelementptr" && in.ops.size() >= 1 && is_local(in.ops[0], alias, &a) && a < 0) {
                    u.negative = true;
                    u.min_neg = std::min(u.min_neg, a);
                    if (!u.neg_struct && lay.resolve(in.ety).kind == ir::Type::Struct) {
                        u.neg_struct = in.ety;
                        u.neg_base = a;
                    }
                }
                if (in.op == "call" && !in.callee.empty() && depth < 3) {
                    const auto* callee = m.find(in.callee);
                    if (!callee || callee->blocks.empty() || callee == &g) continue;
                    // call operands: the arguments (the callee is not among `ops`)
                    for (std::size_t i = 0; i < in.ops.size() && i < callee->params.size(); ++i)
                        if (is_local(in.ops[i], alias, &a) && (a >= 0 || container) && callee->params[i].ty.kind == ir::Type::Ptr)
                            scan_one(m, lay, *callee, callee->params[i].name, a, depth + 1, container, u, visited);
                    // --pir-container-of: an accessor's result is the pointer field it loads (returned_field); its uses
                    // here are uses of that nested object, as for a load of the field in this function
                    if (container && !in.result.empty() && !in.callee_op)
                        for (std::size_t i = 0; i < in.ops.size() && i < callee->params.size(); ++i)
                            if (is_local(in.ops[i], alias, &a) && a >= 0)
                                if (auto k = returned_field(m, lay, *callee, i, depth + 1)) {
                                    const auto fo = static_cast<uint64_t>(a + *k);
                                    u.ptr_fields[fo].push_back({&g, in.result});
                                    u.max_end = std::max<uint64_t>(u.max_end, fo + lay.store_size(ir::parse_type("ptr")));
                                    break;
                                }
                }
            } catch (const Unenc&) {
            }
        }
}

struct Builder {
    const ir::Module& m;
    const Layout& lay;
    const HarnessOptions& opt;
    HarnessPlan plan;
    std::string fail;

    // --pir-length-pairing: the pointer field loaded into `rs` is indexed under a bound by an integer field of the same
    // object `n` (the int loads `u` saw): a flat object of that many elements, the count read at entry
    std::shared_ptr<HarnessNode> paired_field(const Use& u, const HarnessNode& n, const std::vector<Root>& rs,
                                              const std::string& fname) {
        std::vector<PairEvidence> ev;
        for (const auto& r : rs) {
            const ir::Function* fn = r.fn;
            CountOf count_of = [&u, fn](const std::string& reg) {
                auto it = u.int_loads.find({fn, reg});
                return it == u.int_loads.end() ? std::string() : "@" + std::to_string(it->second.first);
            };
            pair_evidence(m, lay, *fn, {r.name}, count_of, 0, ev);
        }
        auto lp = agree(ev);
        if (!lp) return nullptr;
        // the count field: same offset in every load, inside the first element of the containing object
        lp->count_off = std::stoull(lp->count.substr(1));
        std::set<unsigned> widths;
        for (const auto& [k, v] : u.int_loads)
            if (v.first == lp->count_off) widths.insert(v.second);
        if (widths.size() != 1) return nullptr;  // the field read at two widths: not one count
        lp->count_bits = *widths.begin();
        if ( n.member_off + lp->count_off + (lp->count_bits + 7) / 8 > n.bytes) return nullptr;
        auto c = std::make_shared<HarnessNode>();
        c->name = fname;
        c->type = lp->elem;
        c->type_known = true;
        c->elems = 0;
        c->bytes = 0;
        c->basis = "length pairing";
        const std::string cnt = n.name + "->@" + std::to_string(lp->count_off);
        plan.assumptions.push_back(
            "length pairing: " + fname + " points to at least " + cnt + " elements (inferred from usage): an object of " +
            (lp->is_signed ? "max(" + cnt + ", 0)" : cnt + " (unsigned)") + " x " + std::to_string(lp->elem_bytes) + " bytes (" +
            lp->elem.text + "), the count being the i" + std::to_string(lp->count_bits) + " field at byte " +
            std::to_string(lp->count_off) + " of " + n.name + " as it is at entry; size below 2^47 bytes; evidence: " +
            lp->evidence + "; 16-byte aligned, arbitrary contents, pointer fields of its elements arbitrary (auto harness)");
        c->pair = std::move(lp);
        return c;
    }

    // `roots`: the SSA values (of any function) that hold the pointer to this object
    std::shared_ptr<HarnessNode> build(const std::vector<Root>& roots, int depth, const std::string& name) {
        Use u;
        std::set<std::pair<const ir::Function*, std::string>> visited;
        for (auto& r : roots) scan_one(m, lay, *r.fn, r.name, 0, 0, opt.container_of, u, visited);
        auto n = std::make_shared<HarnessNode>();
        n->name = name;
        uint64_t member_off = 0;
        std::optional<ir::Type> container;
        if (u.negative) {
            // container_of: the parameter points to a member of a larger object. The container's type is the
            // struct a getelementptr is based on at the negative offset; the object must cover every offset used.
            if (opt.container_of && u.neg_struct && u.min_neg >= u.neg_base) {
                try {
                    const uint64_t csz = lay.alloc_size(*u.neg_struct);
                    member_off = static_cast<uint64_t>(-u.neg_base);
                    if (member_off + u.max_end <= csz && csz <= max_obj_size()) container = u.neg_struct;
                } catch (const Unenc&) {
                }
            }
            if (!container) {
                if (depth == 0)
                    fail = opt.container_of
                               ? "access before the start of the pointed-to object (container_of style negative offset) "
                                 "without a containing struct type that covers every offset used"
                               : "access before the start of the pointed-to object (container_of style negative offset)";
                return nullptr;
            }
        }
        try {
            if (container) {
                n->type = *container;
                n->type_known = true;
                n->elems = 1;
                n->member_off = member_off;
                n->basis = "container_of: the pointer points to byte " + std::to_string(member_off) + " of a " +
                           container->text + " (containing object; type from the getelementptr at the negative offset)";
            } else if (u.struct_ty) {
                n->type = *u.struct_ty;
                n->type_known = true;
                n->elems = u.indexed ? opt.elems : 1;
                n->basis = std::string("pointee type ") + u.struct_ty->text + " from the function's field accesses" +
                           (u.indexed ? ", indexed: default " + std::to_string(opt.elems) + " elements" : ", one object");
            } else if (u.scalar_ty) {
                n->type = *u.scalar_ty;
                n->type_known = true;
                n->elems = opt.elems;
                n->basis = std::string("pointee type ") + u.scalar_ty->text + " from the function's accesses, default " +
                           std::to_string(opt.elems) + " elements (no size annotation)";
            } else {
                n->type = ir::parse_type("i8");
                n->elems = opt.unknown_bytes;
                n->basis = "pointee type not determined (not accessed by the function), default " +
                           std::to_string(opt.unknown_bytes) + " bytes";
            }
            const uint64_t esz = std::max<uint64_t>(1, lay.alloc_size(n->type));
            if (esz && n->elems > max_obj_size() / esz) {
                if (depth == 0) fail = "pointee object larger than the pointer model allows";
                return nullptr;
            }
            if (!container) n->elems = std::max<uint64_t>(n->elems, (u.max_end + esz - 1) / esz);
            n->bytes = n->elems * esz;
        } catch (const Unenc& e) {
            if (depth == 0) fail = e.reason;
            return nullptr;
        }
        ++plan.objects;
        plan.depth_reached = std::max(plan.depth_reached, depth);
        plan.assumptions.push_back(name + (container ? " points to byte " + std::to_string(member_off) + " of a valid object of "
                                                     : std::string(" points to the start of a valid object of ")) +
                                   std::to_string(n->bytes) +
                                   " bytes (" + std::to_string(n->elems) + " x " + n->type.text + "; " + n->basis +
                                   "), 16-byte aligned, fields initialised with arbitrary values (auto harness)");
        const uint64_t psz = lay.store_size(ir::parse_type("ptr"));
        for (auto& [off, rs] : u.ptr_fields) {
            if (n->member_off + off + psz > n->bytes) continue;
            const std::string fname = name + "->@" + std::to_string(off);
            // a function-pointer field the ops tables know (--pir-ops-auto): bound to one of its targets
            if (auto fp = u.fp_fields.find(off); fp != u.fp_fields.end() && opt.ops && opt.ops_module) {
                const OpsField* of = nullptr;
                for (const auto& f : *opt.ops)
                    if (f.struct_type == fp->second.first && f.index == fp->second.second) {
                        of = &f;
                        break;
                    }
                bool all_known = of != nullptr && !of->targets.empty();
                if (of)
                    for (const auto& t : of->targets)
                        all_known = all_known && (opt.ops_module->find(t) != nullptr ||
                                                  std::find(opt.ops_module->declarations.begin(), opt.ops_module->declarations.end(), t) !=
                                                      opt.ops_module->declarations.end());
                if (all_known) {
                    HarnessFpField hf;
                    hf.offset = off;
                    hf.struct_type = of->struct_type;
                    hf.index = of->index;
                    hf.targets = of->targets;
                    hf.role = of->role;
                    std::string tl;
                    for (const auto& t : of->targets) tl += (tl.empty() ? "" : ", ") + t;
                    plan.assumptions.push_back(fname + " (function-pointer field " + of->struct_type + "." + std::to_string(of->index) +
                                               ") holds one of {" + tl + "} (ops table, " + of->role +
                                               ": the framework is assumed to have initialised it to a target the table names; "
                                               "a field set outside this unit may hold another function)");
                    n->fp_fields.push_back(std::move(hf));
                    continue;
                }
            }
            if (depth >= opt.depth) {
                n->unbound_fields.push_back(off);
                ++n->cut_depth;
                continue;
            }
            if (plan.objects >= opt.max_objects) {
                n->unbound_fields.push_back(off);
                ++n->cut_cap;
                continue;
            }
            if (opt.length_pairing)
                if (auto child = paired_field(u, *n, rs, fname)) {
                    ++plan.objects;
                    plan.depth_reached = std::max(plan.depth_reached, depth + 1);
                    n->nested.emplace_back(off, std::move(child));
                    continue;
                }
            auto child = build(rs, depth + 1, fname);
            if (!child || !child->type_known) {
                if (child) {  // no type evidence: its object was counted and listed, take it back
                    --plan.objects;
                    plan.assumptions.pop_back();
                }
                n->unbound_fields.push_back(off);
                ++n->cut_notype;
                continue;
            }
            n->nested.emplace_back(off, std::move(child));
        }
        return n;
    }
};

void count_cuts(const HarnessNode& n, int& d, int& c, int& t) {
    d += n.cut_depth;
    c += n.cut_cap;
    t += n.cut_notype;
    for (auto& [o, ch] : n.nested) count_cuts(*ch, d, c, t);
}

void unbound_lines(const HarnessNode& n, const HarnessOptions& opt, std::vector<std::string>& out) {
    if (!n.unbound_fields.empty()) {
        std::string offs;
        for (auto o : n.unbound_fields) offs += (offs.empty() ? "" : ",") + std::to_string(o);
        out.push_back("pointer fields of " + n.name + " at byte offsets " + offs +
                      " that the function loads are unconstrained arbitrary values (nested pointer depth bound " +
                      std::to_string(opt.depth) + ", object cap " + std::to_string(opt.max_objects) +
                      ", or no pointee type evidence); a dereference of one is reported as a violation of the "
                      "harness (auto harness)");
    }
    for (auto& [o, c] : n.nested) unbound_lines(*c, opt, out);
}

}  // namespace

std::optional<HarnessPlan> plan_pointer_harness(const ir::Module& m, const Layout& lay, const ir::Function& f,
                                                const ir::Param& p, const HarnessOptions& opt, std::string* why) {
    Builder b{m, lay, opt, {}, {}};
    auto root = b.build({Root{&f, p.name}}, 0, p.name);
    if (!root) {
        if (why) *why = b.fail.empty() ? "no harness object" : b.fail;
        return std::nullopt;
    }
    b.plan.root = root;
    b.plan.assumptions.push_back("auto harness for " + p.name + ": nested pointer depth bound " +
                                 std::to_string(opt.depth) + " (pointer fields past it are unconstrained)");
    unbound_lines(*root, opt, b.plan.assumptions);
    // the bound that was applied and what it cut, for the record (--jsonl harness.depth_*)
    int cd = 0, cc = 0, cn = 0;
    count_cuts(*root, cd, cc, cn);
    for (auto& a : b.plan.assumptions)
        if (a.starts_with("auto harness for " + p.name + ": nested pointer depth bound"))
            a += "; deepest object at level " + std::to_string(b.plan.depth_reached) + " of " + std::to_string(opt.depth) +
                 "; pointer fields left arbitrary: " + std::to_string(cd) + " past the depth bound, " + std::to_string(cc) +
                 " over the object cap of " + std::to_string(opt.max_objects) + ", " + std::to_string(cn) + " without pointee type evidence";
    return std::move(b.plan);
}

std::optional<LengthPair> infer_param_length(const ir::Module& m, const Layout& lay, const ir::Function& f, const ir::Param& p) {
    if (p.ty.kind != ir::Type::Ptr || p.name.empty()) return std::nullopt;
    std::set<std::string> ints;
    for (const auto& q : f.params)
        if (q.ty.kind == ir::Type::Int && !q.name.empty()) ints.insert(q.name);
    if (ints.empty()) return std::nullopt;
    CountOf count_of = [&ints](const std::string& reg) { return ints.count(reg) ? reg : std::string(); };
    std::vector<PairEvidence> ev;
    pair_evidence(m, lay, f, {p.name}, count_of, 0, ev);
    return agree(ev);
}

}  // namespace prism::pir::pirmem
