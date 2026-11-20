// Planner of the automatic pointer harness (harness.hpp, docs/PIR.md
// "Automatic pointer harness"). Pure IR analysis: no solver, no memory model.

#include "harness.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

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
    std::map<int64_t, ir::Type> pos_struct;  // offset -> struct type of the first getelementptr based there (a > 0)
    uint64_t max_struct0 = 0;  // largest struct type a getelementptr uses at offset 0 (tcp_sk(sk): struct tcp_sock over a struct sock)
    std::string max_struct0_ty, max_struct0_at;  // that struct and where (function, line)
    std::string freed_at;  // --pir-heap-freed: the object itself is passed to a free function (function, line)
    // --pir-index-field-bounds: (index field offset, width) -> (array element count, array offset, where)
    std::map<std::pair<int64_t, unsigned>, std::tuple<uint64_t, int64_t, std::string>> index_fields;
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

// " line N" of an instruction (its !dbg location), or ""
std::string where_line(const ir::Module& m, const ir::Inst& in) {
    auto it = m.locs.find(in.dbg);
    return it == m.locs.end() || it->second.line <= 0 ? std::string() : " line " + std::to_string(it->second.line);
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
                if (in.op == "getelementptr" && in.ops.size() >= 1 && is_local(in.ops[0], alias, &a) && a > 0 &&
                    !u.pos_struct.count(a) && lay.resolve(in.ety).kind == ir::Type::Struct)
                    u.pos_struct[a] = in.ety;  // --pir-priv-areas: a struct based past the start (a private area)
                if (in.op == "getelementptr" && in.ops.size() >= 1 && is_local(in.ops[0], alias, &a) && a == 0 &&
                    lay.resolve(in.ety).kind == ir::Type::Struct)  // the largest struct the object is used as
                    if (const uint64_t sz = lay.alloc_size(in.ety); sz > u.max_struct0) {
                        u.max_struct0 = sz;
                        u.max_struct0_ty = in.ety.text;
                        u.max_struct0_at = g.name + where_line(m, in);
                    }
                if (in.op == "getelementptr" && in.ops.size() >= 1 && is_local(in.ops[0], alias, &a) && a < 0) {
                    u.negative = true;
                    u.min_neg = std::min(u.min_neg, a);
                    if (!u.neg_struct && lay.resolve(in.ety).kind == ir::Type::Struct) {
                        u.neg_struct = in.ety;
                        u.neg_base = a;
                    }
                }
                // --pir-index-field-bounds: getelementptr (base at a constant offset of this object), constant indices
                // down to an in-object array [N x T], then a variable index that is (a cast of) an integer loaded from
                // a constant offset of the same object: that field indexes the array
                if (in.op == "getelementptr" && in.ops.size() >= 3 && is_local(in.ops[0], alias, &a) && a >= 0 &&
                    in.ops.back().v.kind == ir::Value::Local) {
                    bool ok = in.ops[1].v.kind == ir::Value::Zero ||
                              (in.ops[1].v.kind == ir::Value::Int && in.ops[1].v.bits == 0);
                    int64_t off = a;
                    ir::Type cur = in.ety;
                    for (std::size_t k = 2; ok && k + 1 < in.ops.size(); ++k) {
                        const auto& o = in.ops[k];
                        if (o.v.kind != ir::Value::Int && o.v.kind != ir::Value::Zero) { ok = false; break; }
                        const int64_t i = o.v.kind == ir::Value::Zero ? 0 : signed_of(o);
                        const auto& r = lay.resolve(cur);
                        if (r.kind == ir::Type::Struct && i >= 0) {
                            off += static_cast<int64_t>(lay.field_offset(r, static_cast<unsigned>(i)));
                            cur = lay.field_type(r, static_cast<unsigned>(i));
                        } else if (r.kind == ir::Type::Array && !r.elems.empty()) {
                            off += i * static_cast<int64_t>(lay.alloc_size(r.elems.at(0)));
                            cur = r.elems.at(0);
                        } else {
                            ok = false;
                        }
                    }
                    const auto& arr = ok ? lay.resolve(cur) : cur;
                    if (ok && arr.kind == ir::Type::Array && !arr.elems.empty()) {
                        const uint64_t esz = lay.alloc_size(arr.elems.at(0));
                        const uint64_t cnt = esz ? lay.alloc_size(arr) / esz : 0;
                        // the index: sext / zext / trunc of a load
                        std::string v = in.ops.back().v.name;
                        for (int g2 = 0; g2 < 4; ++g2) {
                            auto d = defs.find(v);
                            if (d == defs.end()) break;
                            const auto& di = *d->second;
                            if ((di.op == "sext" || di.op == "zext" || di.op == "trunc") && !di.ops.empty() &&
                                di.ops[0].v.kind == ir::Value::Local) {
                                v = di.ops[0].v.name;
                                continue;
                            }
                            int64_t fo = 0;
                            if (di.op == "load" && di.ty.kind == ir::Type::Int && di.ty.bits >= 8 && di.ty.bits <= 64 &&
                                di.ty.bits % 8 == 0 && !di.ops.empty() && is_local(di.ops[0], alias, &fo) && fo >= 0 && cnt > 0)
                                u.index_fields.emplace(std::make_pair(fo, di.ty.bits / 8),
                                                       std::make_tuple(cnt, off, in.ety.text + " array of " + std::to_string(cnt) +
                                                                                    " at byte " + std::to_string(off)));
                            break;
                        }
                    }
                }
                // --pir-heap-freed: the object itself (offset 0) is passed to a free function
                if (in.op == "call" && !in.callee.empty() && u.freed_at.empty()) {
                    static const std::map<std::string, std::size_t> kFree{
                        {"__prism_free", 0}, {"kfree", 0},      {"kvfree", 0},        {"vfree", 0},
                        {"kfree_sensitive", 0}, {"kvfree_sensitive", 0}, {"kzfree", 0}, {"free", 0},
                        {"devm_kfree", 1},   {"kmem_cache_free", 1}, {"kmem_free", 0}, {"zfs_kmem_free", 0}};
                    if (auto it = kFree.find(in.callee); it != kFree.end() && it->second < in.ops.size() &&
                        is_local(in.ops[it->second], alias, &a) && a == 0 &&
                        (in.callee != "__prism_free" ||
                         (in.ops.size() >= 2 && in.ops[1].v.kind == ir::Value::Int &&
                          in.ops[1].v.bits == static_cast<uint64_t>(MemKind::Heap))))
                        u.freed_at = in.callee + " in " + g.name + where_line(m, in);
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
                            // a may be negative (the container of a container_of parameter: to_platform_device(dev)
                            // then platform_get_drvdata(pdev)); the field itself must be at or after the parameter
                            if (is_local(in.ops[i], alias, &a))
                                if (auto k = returned_field(m, lay, *callee, i, depth + 1); k && a + *k >= 0) {
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

// --pir-priv-areas: the size of a private area whose struct is `psz` bytes. The allocation is used when the unit makes
// one with a constant private size at least psz (alloc_netdev_mqs(sizeof_priv, ...), devm_iio_device_alloc(dev,
// sizeof_priv), ...) and all such allocations agree; otherwise the area struct's own size. `src` says which.
uint64_t priv_area_size(const ir::Module& m, uint64_t psz, std::string* src) {
    static const std::map<std::string, std::size_t> kAlloc{
        {"alloc_netdev_mqs", 0},      {"alloc_etherdev_mqs", 0},      {"devm_alloc_etherdev_mqs", 1},
        {"alloc_candev_mqs", 0},      {"devm_iio_device_alloc", 1},   {"iio_device_alloc", 1},
        {"ieee80211_alloc_hw_nm", 0}, {"wiphy_new_nm", 1},            {"scsi_host_alloc", 1},
        {"__spi_alloc_controller", 1}, {"__devm_spi_alloc_controller", 1}, {"mdiobus_alloc_size", 0},
        {"devm_mdiobus_alloc_size", 1}, {"alloc_hdlcdev", 0},         {"devm_alloc_etherdev", 1}};
    std::set<uint64_t> sizes;
    std::string callee;
    for (const auto& g : m.functions)
        for (const auto& bl : g.blocks)
            for (const auto& in : bl.insts) {
                if (in.op != "call" || in.callee.empty()) continue;
                auto it = kAlloc.find(in.callee);
                if (it == kAlloc.end() || it->second >= in.ops.size()) continue;
                const auto& o = in.ops[it->second];
                if (o.v.kind != ir::Value::Int) continue;
                if (o.v.bits >= psz && o.v.bits < (uint64_t{1} << 30)) {
                    sizes.insert(o.v.bits);
                    callee = in.callee;
                }
            }
    if (sizes.size() == 1) {
        *src = "from " + callee + "(sizeof_priv = " + std::to_string(*sizes.begin()) + ") in this unit";
        return *sizes.begin();
    }
    *src = sizes.empty() ? "of the private area's struct (no allocation with a constant private size in this unit)"
                         : "of the private area's struct (the unit's allocations disagree)";
    return psz;
}

struct Builder {
    const ir::Module& m;
    const Layout& lay;
    const HarnessOptions& opt;
    HarnessPlan plan;
    std::string fail;

    // `roots`: the SSA values (of any function) that hold the pointer to this object; `decl`: its declared pointee
    // (--pir-declared-types), when the debug information has one
    std::shared_ptr<HarnessNode> build(const std::vector<Root>& roots, int depth, const std::string& name,
                                       const std::optional<DeclaredPointee>& decl = std::nullopt) {
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
            uint64_t min_bytes = 0;  // a declared or stated size the element count does not express
            // --pir-param-preconditions "type S" (the parameter's own object): the stated type replaces the evidence
            if (depth == 0 && opt.root_type && !container &&
                !(n->type_known && n->type.text == opt.root_type->text && n->elems == 1)) {
                n->type = *opt.root_type;
                n->type_known = true;
                n->elems = 1;
                n->basis = "type " + opt.root_type->text + " stated by a parameter precondition (--pir-param-preconditions)";
                plan.root_override_used = true;
            }
            const uint64_t esz = std::max<uint64_t>(1, lay.alloc_size(n->type));
            if (esz && n->elems > max_obj_size() / esz) {
                if (depth == 0) fail = "pointee object larger than the pointer model allows";
                return nullptr;
            }
            const uint64_t type_bytes = n->elems * esz;  // what the type evidence and the defaults give
            // --pir-declared-types: a declared pointee of known size (struct, union or array) IS the object's size. A
            // constant-offset access past it is a violation, reported as such, not a reason to grow the object. Not
            // used when an evidence struct is larger than the declaration (a cast to a bigger type: the object is never
            // made smaller than the evidence type), for an indexed struct pointee (the count is not declared), or for a
            // `void *` / `char *` declaration (no composite pointee). Each use is a named "declared-type:" line.
            const bool none = !u.struct_ty && !u.scalar_ty;
            const bool use_decl = opt.declared_types && decl && decl->bytes && decl->composite && !container &&
                                  !(depth == 0 && opt.root_type) && !(u.struct_ty && u.indexed) &&
                                  u.max_struct0 <= decl->bytes;
            // --pir-priv-areas: a struct based at the end of the object (within 128 bytes of alignment) is a private
            // area (netdev_priv, shost_priv, ...): the object reaches its end. The struct end is the declared size or
            // the evidence struct's; without either there is no end to find a tail after.
            uint64_t priv_end = 0;
            std::string priv_line;
            if (opt.priv_areas && !container) {
                const uint64_t base_end = use_decl ? decl->bytes
                                          : (u.struct_ty && !u.indexed) ? lay.alloc_size(*u.struct_ty) : 0;
                if (base_end)
                    for (const auto& [t, pty] : u.pos_struct) {
                        if (t < static_cast<int64_t>(base_end) || static_cast<uint64_t>(t) >= base_end + 128) continue;
                        const uint64_t psz = lay.alloc_size(pty);
                        std::string src;
                        const uint64_t area = priv_area_size(m, psz, &src);
                        if (!area || area >= max_obj_size() - static_cast<uint64_t>(t)) break;
                        priv_end = static_cast<uint64_t>(t) + area;
                        priv_line = "priv-area: " + name + " has a private area at byte " + std::to_string(t) + " of " +
                                    std::to_string(area) + " bytes (" + pty.text + " is based there, past the " +
                                    std::to_string(base_end) + "-byte struct; size " + src +
                                    ") (--pir-priv-areas; not checked here)";
                        break;
                    }
            }
            if (use_decl) {
                const std::string was = none ? "no type evidence in the IR"
                                             : "the IR evidence and defaults gave " + std::to_string(type_bytes) + " bytes (" +
                                                   std::to_string(n->elems) + " x " + n->type.text + ")";
                if (!decl->ir_struct.empty()) {
                    n->type = ir::Type{};
                    n->type.kind = ir::Type::Struct;
                    n->type.text = decl->ir_struct;
                    n->elems = 1;
                } else if (!u.struct_ty) {
                    n->type = ir::parse_type("i8");
                    n->elems = decl->bytes;
                }  // else: an evidence struct no larger than the declaration keeps its type, the object is extended
                n->type_known = true;
                n->bytes = decl->bytes;
                n->basis += "; declared pointee type " + decl->spelling + " of " + std::to_string(decl->bytes) +
                            " bytes (--pir-declared-types)";
                plan.assumptions.push_back("declared-type: " + name + " points to " + decl->spelling + " (" +
                                           std::to_string(decl->bytes) +
                                           " bytes, from the declaration in the unit's debug information; " + was +
                                           ") (--pir-declared-types; not checked here)");
                ++plan.declared_used;
                if (priv_end > n->bytes) {
                    n->bytes = priv_end;
                    plan.assumptions.push_back(priv_line);
                }
            } else {
                const uint64_t typed = std::max({type_bytes, priv_end, (!container && !u.indexed) ? u.max_struct0 : uint64_t{0}});
                // a struct plus its private area is a fully typed object: an access past the area is a violation
                if (!container && !priv_end) n->elems = std::max<uint64_t>(n->elems, (u.max_end + esz - 1) / esz);
                // the largest struct the object is used as at offset 0 is type evidence too (not access-sized)
                const uint64_t ev = (opt.declared_types && !container && !u.indexed) ? u.max_struct0 : 0;
                n->bytes = std::max({n->elems * esz, priv_end, ev});
                if (ev > type_bytes && ev > priv_end)
                    plan.assumptions.push_back("container-type-from-cast: " + name + " is treated as " + u.max_struct0_ty +
                                               " (" + std::to_string(ev) + " bytes; a getelementptr uses it at offset 0 in " +
                                               u.max_struct0_at + "; larger than its declared or first-used type) (--pir-declared-types; not checked here)");
                if (priv_end > type_bytes) plan.assumptions.push_back(priv_line);
                // sized from the very accesses being checked: says little, so it is tagged for separate counting/review
                if (!container && n->bytes > typed)
                    plan.assumptions.push_back("harness-size-from-access: " + name +
                                               " sized to reach the highest constant offset accessed (" +
                                               std::to_string(n->bytes) + " bytes; the type evidence and defaults gave " +
                                               std::to_string(typed) + ") (auto harness)");
            }
            if (depth == 0 && opt.root_min_bytes > n->bytes && !container) {
                min_bytes = std::max(min_bytes, opt.root_min_bytes);
                n->basis += "; at least " + std::to_string(opt.root_min_bytes) +
                            " bytes stated by a parameter precondition (--pir-param-preconditions)";
                plan.root_override_used = true;
            }
            if (min_bytes > n->bytes) {
                if (min_bytes >= max_obj_size()) {
                    if (depth == 0) fail = "pointee object larger than the pointer model allows";
                    return nullptr;
                }
                n->bytes = min_bytes;
            }
        } catch (const Unenc& e) {
            if (depth == 0) fail = e.reason;
            return nullptr;
        }
        // --pir-heap-freed: the function passes this object to a free function: it is a heap allocation (named), so the
        // free is checked against a live heap object (a second free, a free of an interior or non-heap pointer: caught)
        if (opt.heap_freed && !u.freed_at.empty() && !container) {
            n->type_known = true;  // bound even without type evidence: an object of the size chosen above
            n->heap = true;
            plan.assumptions.push_back("heap-object: " + name + " is heap-allocated (freed by " + u.freed_at +
                                       "): its harness object is a live heap allocation of " + std::to_string(n->bytes) +
                                       " bytes (--pir-heap-freed; not checked here)");
        }
        ++plan.objects;
        plan.depth_reached = std::max(plan.depth_reached, depth);
        plan.assumptions.push_back(name + (container ? " points to byte " + std::to_string(member_off) + " of a valid object of "
                                                     : std::string(" points to the start of a valid object of ")) +
                                   std::to_string(n->bytes) +
                                   " bytes (" + std::to_string(n->elems) + " x " + n->type.text + "; " + n->basis +
                                   "), 16-byte aligned, fields initialised with arbitrary values (auto harness)");
        if (opt.index_field_bounds)
            for (const auto& [fw, info] : u.index_fields) {
                const auto& [cnt, aoff, where] = info;
                const uint64_t fo = n->member_off + static_cast<uint64_t>(fw.first);
                if (fo + fw.second > n->bytes || cnt == 0 || cnt - 1 > (uint64_t{1} << (fw.second * 8 - 1)) - 1) continue;
                HarnessNode::IndexBound ib;
                ib.offset = fo;
                ib.width = fw.second;
                ib.count = cnt;
                ib.array_off = n->member_off + static_cast<uint64_t>(aoff);
                ib.where = where;
                n->index_bounds.push_back(ib);
                plan.assumptions.push_back("index-field-bound: " + name + "->@" + std::to_string(fo) + " (" +
                                           std::to_string(fw.second) + " bytes) is in [0, " + std::to_string(cnt - 1) +
                                           "]: it indexes the " + where +
                                           " (--pir-index-field-bounds; UNVALIDATED, not discharged; not checked here)");
            }
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
            std::optional<DeclaredPointee> cdecl;
            if (opt.declared_types && decl) cdecl = declared_member_pointee(m, lay, decl->ref, off);
            const std::size_t before = plan.assumptions.size();
            auto child = build(rs, depth + 1, fname, cdecl);
            if (!child || !child->type_known) {
                if (child) --plan.objects;  // no type evidence: its object was counted and listed, take it back
                plan.assumptions.resize(before);  // its object line and any size / declared-type line
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
    std::optional<DeclaredPointee> decl;
    if (opt.declared_types) decl = declared_param_pointee(m, lay, f, p);
    auto root = b.build({Root{&f, p.name}}, 0, p.name, decl);
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

// --pir-declared-types (docs/PIR.md "Declared pointee types"): the debug information of a unit lowered with -g.
namespace {

// typedef / const / volatile / restrict / atomic: the type they name
const ir::DIType* di_strip(const ir::Module& m, std::string ref, std::string* out_ref) {
    for (int guard = 0; guard < 64 && !ref.empty(); ++guard) {
        auto it = m.ditypes.find(ref);
        if (it == m.ditypes.end()) return nullptr;
        const auto& t = it->second;
        if (t.kind == "derived" && (t.tag == "DW_TAG_typedef" || t.tag == "DW_TAG_const_type" ||
                                    t.tag == "DW_TAG_volatile_type" || t.tag == "DW_TAG_restrict_type" ||
                                    t.tag == "DW_TAG_atomic_type")) {
            ref = t.base;
            continue;
        }
        if (out_ref) *out_ref = ref;
        return &t;
    }
    return nullptr;
}

// the pointee of the (unstripped) pointer type `ref`
std::optional<DeclaredPointee> pointee_of(const ir::Module& m, const Layout& lay, const std::string& ref) {
    const auto* pt = di_strip(m, ref, nullptr);
    if (!pt || pt->kind != "derived" || pt->tag != "DW_TAG_pointer_type" || pt->base.empty()) return std::nullopt;
    DeclaredPointee d;
    const auto* t = di_strip(m, pt->base, &d.ref);
    if (!t || t->fwd || t->size_bits == 0 || t->size_bits % 8) return std::nullopt;
    d.bytes = t->size_bits / 8;
    const bool s = t->tag == "DW_TAG_structure_type", un = t->tag == "DW_TAG_union_type";
    d.composite = t->kind == "composite" && (s || un || t->tag == "DW_TAG_array_type");
    d.spelling = s ? "struct " + t->name : un ? "union " + t->name : t->name.empty() ? t->tag : t->name;
    if ((s || un) && !t->name.empty()) {
        const std::string key = (s ? "struct." : "union.") + t->name;
        if (auto it = m.types.find(key); it != m.types.end() && it->second.kind == ir::Type::Struct) {
            try {
                if (lay.alloc_size(it->second) == d.bytes) d.ir_struct = "%" + key;
            } catch (const Unenc&) {
            }
        }
    }
    return d;
}

}  // namespace

std::optional<DeclaredPointee> declared_param_pointee(const ir::Module& m, const Layout& lay, const ir::Function& f,
                                                      const ir::Param& p) {
    if (f.dbg.empty() || p.name.empty()) return std::nullopt;
    for (const auto& v : m.diparams)
        if (v.scope == f.dbg && v.name == p.name) return pointee_of(m, lay, v.type);
    return std::nullopt;
}

std::optional<DeclaredPointee> declared_member_pointee(const ir::Module& m, const Layout& lay, const std::string& ref,
                                                       uint64_t off) {
    std::string cur = ref;
    for (int guard = 0; guard < 32; ++guard) {
        const auto* t = di_strip(m, cur, &cur);
        if (!t || t->kind != "composite") return std::nullopt;
        if (t->tag == "DW_TAG_array_type") {
            std::string eref;
            const auto* e = di_strip(m, t->base, &eref);
            if (!e || e->size_bits == 0 || e->size_bits % 8) return std::nullopt;
            const uint64_t esz = e->size_bits / 8;
            if (t->size_bits && off >= t->size_bits / 8) return std::nullopt;
            off %= esz;
            if (e->kind == "derived" && e->tag == "DW_TAG_pointer_type") return off == 0 ? pointee_of(m, lay, eref) : std::nullopt;
            cur = eref;
            continue;
        }
        if (t->tag != "DW_TAG_structure_type" && t->tag != "DW_TAG_union_type") return std::nullopt;
        auto el = m.md_tuples.find(t->elements);
        if (el == m.md_tuples.end()) return std::nullopt;
        std::string next;
        uint64_t next_off = 0;
        for (const auto& r : el->second) {
            auto it = m.ditypes.find(r);
            if (it == m.ditypes.end() || it->second.tag != "DW_TAG_member" || it->second.offset_bits % 8) continue;
            const uint64_t mo = it->second.offset_bits / 8;
            std::string mref;
            const auto* mt = di_strip(m, it->second.base, &mref);
            if (!mt) continue;
            if (mt->kind == "derived" && mt->tag == "DW_TAG_pointer_type") {
                if (mo == off) return pointee_of(m, lay, mref);
                continue;
            }
            const uint64_t msz = (it->second.size_bits ? it->second.size_bits : mt->size_bits) / 8;
            if (mt->kind == "composite" && off >= mo && off < mo + msz) {
                next = mref;
                next_off = off - mo;
                break;
            }
        }
        if (next.empty()) return std::nullopt;
        cur = next;
        off = next_off;
    }
    return std::nullopt;
}

}  // namespace prism::pir::pirmem
