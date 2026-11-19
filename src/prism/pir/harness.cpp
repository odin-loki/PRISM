// Planner of the automatic pointer harness (harness.hpp, docs/PIR.md
// "Automatic pointer harness"). Pure IR analysis: no solver, no memory model.

#include "harness.hpp"

#include <algorithm>
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
};

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
            auto child = build(rs, depth + 1, fname);
            if (!child || (!child->type_known && !opt.keep_untyped)) {
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

std::optional<HarnessPlan> plan_harness_roots(const ir::Module& m, const Layout& lay,
                                              const std::vector<std::pair<const ir::Function*, std::string>>& roots,
                                              const std::string& name, const HarnessOptions& opt, std::string* why) {
    if (roots.empty()) {
        if (why) *why = "no use of the object";
        return std::nullopt;
    }
    Builder b{m, lay, opt, {}, {}};
    std::vector<Root> rs;
    for (auto& [f, v] : roots) rs.push_back(Root{f, v});
    auto root = b.build(rs, 0, name);
    if (!root) {
        if (why) *why = b.fail.empty() ? "no harness object" : b.fail;
        return std::nullopt;
    }
    b.plan.root = root;
    unbound_lines(*root, opt, b.plan.assumptions);
    return std::move(b.plan);
}

}  // namespace prism::pir::pirmem
