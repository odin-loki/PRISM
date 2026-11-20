// Struct-field value invariants for the automatic pointer harness (--pir-field-invariants) and the store miner
// that collects their evidence (PRISM_PIR_MINE_FIELDS). Docs: docs/USER_GUIDE.md, include/prism/pir.hpp
// FieldInvariant.
//
// The miner reads one module and writes one JSON line per store whose address is a constant field of a named
// struct (`getelementptr %struct.T, p, 0, K`): the stored value's kind is
//   alloc   the result of an allocator call with a constant size (kmalloc / kzalloc / devm_kzalloc / malloc /
//           zfs_kmem_alloc ...; the size in "size")
//   global  the address of a global (its size)
//   func    the address of a function
//   const   an integer constant ("value")
//   null    a null pointer
//   other   anything else (a copy, a computed value)
// An invariant is derived offline from every unit's lines (OS-Proving tools/prism_clusters/mine_fields.py) and is
// only ever an assumption: each row used is named in the verdict with its status and source.

#include "prism/pir.hpp"

#include "translate_mem.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>
#include <ostream>
#include <regex>
#include <sstream>

namespace prism::pir {

namespace {

// allocator-like callee: size argument index, or -1
int alloc_size_arg(const std::string& n) {
    static const std::regex first(R"(^(__)?(k|kv|v)(m|z|c)?alloc(_noprof|_node_noprof|_node)?$|^kzalloc_noprof$|^kmalloc_noprof$|^__kmalloc_noprof$|^__kmalloc_large_noprof$|^vzalloc(_noprof)?$|^vmalloc(_noprof)?$|^kvzalloc(_noprof)?$|^kvmalloc(_noprof)?$|^malloc$|^zfs_kmem_alloc$|^kmem_alloc$|^kmem_zalloc$)");
    static const std::regex second(R"(^devm_k(m|z)alloc(_noprof)?$)");
    static const std::regex third(R"(^__kmalloc_cache_noprof$|^kmalloc_trace(_noprof)?$)");
    if (std::regex_match(n, first)) return 0;
    if (std::regex_match(n, second)) return 1;
    if (std::regex_match(n, third)) return 2;
    return -1;
}

}  // namespace

void mine_field_stores(const ir::Module& m, const std::string& unit, std::ostream& out) {
    pirmem::Layout lay(m);
    for (const auto& f : m.functions) {
        if (f.is_model || f.blocks.empty()) continue;
        std::map<std::string, const ir::Inst*> defs;
        for (const auto& bl : f.blocks)
            for (const auto& in : bl.insts)
                if (!in.result.empty()) defs[in.result] = &in;
        for (const auto& bl : f.blocks)
            for (const auto& in : bl.insts) {
                if (in.op != "store" || in.ops.size() < 2 || in.ops[1].v.kind != ir::Value::Local) continue;
                auto d = defs.find(in.ops[1].v.name);
                if (d == defs.end()) continue;
                const ir::Inst& g = *d->second;
                if (g.op != "getelementptr" || g.ops.size() != 3 || g.ety.kind != ir::Type::Struct ||
                    !g.ety.text.starts_with("%struct.") || g.ops[1].v.kind != ir::Value::Int || g.ops[1].v.bits != 0 ||
                    g.ops[2].v.kind != ir::Value::Int)
                    continue;
                uint64_t off = 0;
                unsigned width = 0;
                try {
                    const auto& st = lay.resolve(g.ety);
                    if (st.kind != ir::Type::Struct) continue;
                    off = lay.field_offset(st, static_cast<unsigned>(g.ops[2].v.bits));
                    width = static_cast<unsigned>(lay.store_size(in.ops[0].ty));
                } catch (const pirmem::Unenc&) {
                    continue;
                } catch (const std::exception&) {
                    continue;
                }
                nlohmann::json j;
                j["unit"] = unit;
                j["fn"] = f.name;
                j["struct"] = g.ety.text;
                j["off"] = off;
                j["width"] = width;
                const auto& v = in.ops[0].v;
                if (v.kind == ir::Value::Int) {
                    j["kind"] = "const";
                    const unsigned w = in.ops[0].ty.bits ? in.ops[0].ty.bits : 64;
                    uint64_t b = v.bits;
                    if (w < 64) {
                        const uint64_t mask = (uint64_t{1} << w) - 1;
                        b &= mask;
                        if (b >> (w - 1)) b |= ~mask;  // sign-extend
                    }
                    j["value"] = static_cast<int64_t>(b);
                } else if (v.kind == ir::Value::Null || (v.kind == ir::Value::Zero && in.ops[0].ty.kind == ir::Type::Ptr)) {
                    j["kind"] = "null";
                } else if (v.kind == ir::Value::Zero) {
                    j["kind"] = "const";
                    j["value"] = 0;
                } else if (v.kind == ir::Value::Global) {
                    if (m.find(v.name)) {
                        j["kind"] = "func";
                    } else if (const auto* gl = m.find_global(v.name)) {
                        j["kind"] = "global";
                        try {
                            j["size"] = lay.alloc_size(gl->ty);
                        } catch (...) {
                            j["size"] = 0;
                        }
                    } else {
                        j["kind"] = "func";  // a declared function
                    }
                } else if (v.kind == ir::Value::Local) {
                    j["kind"] = "other";
                    if (auto c = defs.find(v.name); c != defs.end() && c->second->op == "call" && !c->second->callee.empty()) {
                        const int si = alloc_size_arg(c->second->callee);
                        if (si >= 0 && static_cast<std::size_t>(si) < c->second->ops.size() &&
                            c->second->ops[static_cast<std::size_t>(si)].v.kind == ir::Value::Int) {
                            j["kind"] = "alloc";
                            j["size"] = c->second->ops[static_cast<std::size_t>(si)].v.bits;
                            j["callee"] = c->second->callee;
                        }
                    }
                } else {
                    j["kind"] = "other";
                }
                out << j.dump() << '\n';
            }
    }
}

namespace {
bool glob_match(const char* p, const char* s) {
    for (; *p; ++p, ++s) {
        if (*p == '*') {
            while (p[1] == '*') ++p;
            if (!p[1]) return true;
            for (; *s; ++s)
                if (glob_match(p + 1, s)) return true;
            return false;
        }
        if (!*s || (*p != '?' && *p != *s)) return false;
    }
    return !*s;
}
}  // namespace

bool field_invariant_applies(const FieldInvariant& fi, const std::string& function, const std::string& unit_path) {
    if (fi.scope.empty()) return true;
    const auto colon = fi.scope.find(':');
    const std::string kind = fi.scope.substr(0, colon), pat = fi.scope.substr(colon + 1);
    if (kind == "fn") return glob_match(pat.c_str(), function.c_str());
    return glob_match(pat.c_str(), unit_path.c_str());  // unit: and module: both match the unit's path
}

std::vector<FieldInvariant> load_field_invariants(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = "cannot read " + file.string();
        return {};
    }
    std::vector<FieldInvariant> out;
    std::string line;
    for (int no = 1; std::getline(in, line); ++no) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c;
        std::stringstream ss(line);
        for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
        if (c.size() < 8) {
            error = file.string() + ":" + std::to_string(no) + ": 8 columns expected (struct, offset, width, kind, a, b, status, source)";
            return {};
        }
        FieldInvariant fi;
        fi.struct_type = c[0].starts_with("%") ? c[0] : "%" + c[0];
        try {
            fi.offset = std::stoull(c[1]);
            fi.width = static_cast<unsigned>(std::stoul(c[2]));
            fi.a = c[4].empty() ? 0 : std::stoll(c[4]);
            fi.b = c[5].empty() ? 0 : std::stoll(c[5]);
        } catch (const std::exception&) {
            error = file.string() + ":" + std::to_string(no) + ": offset, width, a, b must be integers";
            return {};
        }
        fi.kind = c[3];
        fi.status = c[6];
        fi.source = c[7];
        if (fi.kind != "valid" && fi.kind != "nonnull" && fi.kind != "range") {
            error = file.string() + ":" + std::to_string(no) + ": kind must be valid, nonnull or range";
            return {};
        }
        if (fi.status.empty() || fi.source.empty()) {
            error = file.string() + ":" + std::to_string(no) + ": status and source are required";
            return {};
        }
        if (c.size() > 8 && !c[8].empty()) {
            fi.scope = c[8];
            if (!fi.scope.starts_with("fn:") && !fi.scope.starts_with("unit:") && !fi.scope.starts_with("module:")) {
                error = file.string() + ":" + std::to_string(no) + ": scope must be fn:GLOB, unit:GLOB or module:GLOB (or empty)";
                return {};
            }
        }
        if ((fi.kind == "valid" || fi.kind == "nonnull") && fi.width != 8) {
            error = file.string() + ":" + std::to_string(no) + ": a pointer invariant has width 8";
            return {};
        }
        if (fi.kind == "valid" && (fi.a <= 0 || fi.a > (int64_t{1} << 30))) {
            error = file.string() + ":" + std::to_string(no) + ": valid needs a size in (0, 1 GiB]";
            return {};
        }
        if (fi.kind == "range" && (fi.width == 0 || fi.width > 8 || fi.a > fi.b)) {
            error = file.string() + ":" + std::to_string(no) + ": range needs width 1..8 and a <= b";
            return {};
        }
        out.push_back(std::move(fi));
    }
    return out;
}

}  // namespace prism::pir
