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
    std::map<uint64_t, std::vector<Root>> ptr_fields;  // byte offset -> pointer values loaded from there
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

void scan_one(const ir::Module& m, const Layout& lay, const ir::Function& g, const std::string& rootname,
              int64_t base, int depth, Use& u, std::set<std::pair<const ir::Function*, std::string>>& visited) {
    if (!visited.insert({&g, rootname + "@" + std::to_string(base)}).second) return;
    std::map<std::string, int64_t> alias{{rootname, base}};
    // pointers derived from the root at constant offsets (fixpoint: blocks are not in dominance order)
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& bl : g.blocks)
            for (auto& in : bl.insts) {
                if (in.op != "getelementptr" || in.result.empty() || alias.count(in.result)) continue;
                int64_t a = 0;
                if (in.ops.empty() || !is_local(in.ops[0], alias, &a)) continue;
                try {
                    if (auto d = const_gep_offset(lay, in)) {
                        alias[in.result] = a + *d;
                        changed = true;
                    }
                } catch (const Unenc&) {
                }
            }
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
                        continue;
                    }
                    const uint64_t sz = lay.store_size(*acc);
                    u.max_end = std::max<uint64_t>(u.max_end, static_cast<uint64_t>(a) + sz);
                    if (base == 0 && addr->v.name == rootname && acc->kind != ir::Type::Ptr && !u.scalar_ty &&
                        (acc->kind == ir::Type::Int || acc->kind == ir::Type::Float))
                        u.scalar_ty = *acc;
                    if (in.op == "load" && acc->kind == ir::Type::Ptr && !in.result.empty())
                        u.ptr_fields[static_cast<uint64_t>(a)].push_back({&g, in.result});
                    continue;
                }
                if (in.op == "getelementptr" && in.ops.size() >= 1 && is_local(in.ops[0], alias, &a) && a < 0)
                    u.negative = true;
                if (in.op == "call" && !in.callee.empty() && depth < 3) {
                    const auto* callee = m.find(in.callee);
                    if (!callee || callee->blocks.empty() || callee == &g) continue;
                    // call operands: the arguments (the callee is not among `ops`)
                    for (std::size_t i = 0; i < in.ops.size() && i < callee->params.size(); ++i)
                        if (is_local(in.ops[i], alias, &a) && a >= 0 && callee->params[i].ty.kind == ir::Type::Ptr)
                            scan_one(m, lay, *callee, callee->params[i].name, a, depth + 1, u, visited);
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
        for (auto& r : roots) scan_one(m, lay, *r.fn, r.name, 0, 0, u, visited);
        auto n = std::make_shared<HarnessNode>();
        n->name = name;
        if (u.negative) {
            if (depth == 0) fail = "access before the start of the pointed-to object (container_of style negative offset)";
            return nullptr;
        }
        try {
            if (u.struct_ty) {
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
            n->elems = std::max<uint64_t>(n->elems, (u.max_end + esz - 1) / esz);
            n->bytes = n->elems * esz;
        } catch (const Unenc& e) {
            if (depth == 0) fail = e.reason;
            return nullptr;
        }
        ++plan.objects;
        plan.assumptions.push_back(name + " points to the start of a valid object of " + std::to_string(n->bytes) +
                                   " bytes (" + std::to_string(n->elems) + " x " + n->type.text + "; " + n->basis +
                                   "), 16-byte aligned, fields initialised with arbitrary values (auto harness)");
        const uint64_t psz = lay.store_size(ir::parse_type("ptr"));
        for (auto& [off, rs] : u.ptr_fields) {
            if (off + psz > n->bytes) continue;
            const std::string fname = name + "->@" + std::to_string(off);
            if (depth >= opt.depth) {
                n->unbound_fields.push_back(off);
                continue;
            }
            if (plan.objects >= opt.max_objects) {
                n->unbound_fields.push_back(off);
                continue;
            }
            auto child = build(rs, depth + 1, fname);
            if (!child || !child->type_known) {
                if (child) {  // no type evidence: its object was counted and listed, take it back
                    --plan.objects;
                    plan.assumptions.pop_back();
                }
                n->unbound_fields.push_back(off);
                continue;
            }
            n->nested.emplace_back(off, std::move(child));
        }
        return n;
    }
};

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
    return std::move(b.plan);
}

}  // namespace prism::pir::pirmem
