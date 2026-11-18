// Ops tables derived from the unit itself (--pir-ops-auto, docs/HARNESS_PHASE2.md "Function-pointer fields").

#include "prism/pir.hpp"
#include "translate_mem.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace prism::pir {

namespace {

using Key = std::pair<std::string, int>;  // (IR struct type spelling, field index)

bool is_function_symbol(const ir::Module& m, const std::string& name) {
    return m.find(name) != nullptr || std::find(m.declarations.begin(), m.declarations.end(), name) != m.declarations.end();
}

// `getelementptr %struct.T, p, 0, K` (an instruction or a constant expression): the field it names
std::optional<Key> field_of_gep(const ir::Inst& g) {
    if (g.op != "getelementptr" || g.ops.size() != 3) return std::nullopt;
    if (g.ops[1].v.kind != ir::Value::Int || g.ops[1].v.bits != 0 || g.ops[2].v.kind != ir::Value::Int) return std::nullopt;
    return Key{g.ety.text, static_cast<int>(g.ops[2].v.bits)};
}

std::optional<Key> field_of_const_gep(const ir::Value& v) {
    if (v.kind != ir::Value::ConstExpr || v.ce_op != "getelementptr" || v.elems.size() != 3) return std::nullopt;
    if (v.elems[1].v.kind != ir::Value::Int || v.elems[1].v.bits != 0 || v.elems[2].v.kind != ir::Value::Int) return std::nullopt;
    return Key{v.ce_ty.text, static_cast<int>(v.elems[2].v.bits)};
}

void walk_init(const ir::Module& m, const pirmem::Layout& lay, const ir::Type& ty, const ir::Value& v,
               std::map<Key, std::set<std::string>>& out, int depth) {
    if (depth > 8) return;
    if (v.kind != ir::Value::Aggregate) return;
    try {
        const auto& rt = lay.resolve(ty);
        if (rt.kind == ir::Type::Struct) {
            for (std::size_t k = 0; k < v.elems.size(); ++k) {
                const auto& e = v.elems[k];
                if (e.v.kind == ir::Value::Global && e.ty.kind == ir::Type::Ptr && is_function_symbol(m, e.v.name))
                    out[Key{ty.text, static_cast<int>(k)}].insert(e.v.name);
                else if (e.v.kind == ir::Value::Aggregate)
                    walk_init(m, lay, lay.field_type(rt, static_cast<unsigned>(k)), e.v, out, depth + 1);
            }
        } else if (rt.kind == ir::Type::Array && !rt.elems.empty()) {
            for (const auto& e : v.elems)
                if (e.v.kind == ir::Value::Aggregate) walk_init(m, lay, rt.elems[0], e.v, out, depth + 1);
        }
    } catch (const pirmem::Unenc&) {
    }
}

}  // namespace

std::vector<OpsField> derive_ops_table(const ir::Module& m, std::size_t max_targets) {
    std::map<Key, std::set<std::string>> found;
    for (const auto& f : m.functions) {
        std::map<std::string, const ir::Inst*> defs;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts)
                if (!in.result.empty()) defs[in.result] = &in;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op != "store" || in.ops.size() < 2) continue;
                const auto& val = in.ops[0];
                if (val.v.kind != ir::Value::Global || val.ty.kind != ir::Type::Ptr || !is_function_symbol(m, val.v.name)) continue;
                const auto& slot = in.ops[1];
                std::optional<Key> k;
                if (slot.v.kind == ir::Value::Local) {
                    if (auto it = defs.find(slot.v.name); it != defs.end()) k = field_of_gep(*it->second);
                } else {
                    k = field_of_const_gep(slot.v);
                }
                if (k) found[*k].insert(val.v.name);
            }
    }
    pirmem::Layout lay(m);
    for (const auto& g : m.globals)
        if (!g.init.empty()) walk_init(m, lay, g.ty, g.init[0].v, found, 0);
    std::vector<OpsField> out;
    for (const auto& [k, targets] : found) {
        if (targets.empty() || targets.size() > max_targets) continue;
        OpsField f;
        f.struct_type = k.first;
        f.index = k.second;
        f.field = "#" + std::to_string(k.second);
        f.targets.assign(targets.begin(), targets.end());
        f.role = "unit-derived";
        out.push_back(std::move(f));
    }
    return out;
}

void merge_ops_tables(std::vector<OpsField>& explicit_table, const std::vector<OpsField>& derived) {
    for (const auto& d : derived) {
        const bool have = std::any_of(explicit_table.begin(), explicit_table.end(),
                                      [&](const OpsField& e) { return e.struct_type == d.struct_type && e.index == d.index; });
        if (!have) explicit_table.push_back(d);
    }
}

}  // namespace prism::pir
