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

#include <algorithm>
#include <fstream>
#include <functional>
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
    // MMIO mappings (pir-ptr-patterns (c)): the mapped size, so a `regs + offset` field gets a mined "valid N" row
    static const std::map<std::string, int> kMmio{
        {"ioremap", 1},          {"ioremap_wc", 1},     {"ioremap_uc", 1},       {"ioremap_cache", 1},
        {"ioremap_np", 1},       {"ioremap_wt", 1},     {"devm_ioremap", 2},     {"devm_ioremap_wc", 2},
        {"devm_ioremap_uc", 2},  {"pci_iomap_range", 3}, {"pci_iomap", 2},       {"pcim_iomap", 2},
        {"of_iomap", -1},        {"pmap_mapdev", 1},    {"pmap_mapdev_attr", 1}, {"acpi_os_map_iomem", 1},
        {"acpi_os_map_memory", 1}, {"memremap", 1},     {"devm_memremap", 2}};
    if (auto it = kMmio.find(n); it != kMmio.end()) return it->second;
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
    // Constant initialisers (ops tables: `static const struct file_operations fops = { .read = f, ... }`): one line per
    // pointer field of every struct aggregate, with "init": true, kind "func" (a function address, "value" its name) or
    // "null" (a field the initialiser leaves out or sets to NULL). Evidence for function-pointer fields that every
    // instance sets (mine_fields.py `nonnull`); the other rules ignore "init" lines.
    std::function<void(const std::string&, const ir::Type&, const ir::Value&, int)> walk =
        [&](const std::string& gname, const ir::Type& ty, const ir::Value& v, int depth) {
            if (depth > 8 || v.kind != ir::Value::Aggregate) return;
            try {
                const auto& rt = lay.resolve(ty);
                if (rt.kind == ir::Type::Struct) {
                    for (std::size_t k = 0; k < v.elems.size() && k < rt.elems.size(); ++k) {
                        const auto& e = v.elems[k];
                        const auto& fty = lay.field_type(rt, static_cast<unsigned>(k));
                        if (fty.kind == ir::Type::Ptr && ty.text.starts_with("%struct.")) {
                            const bool fn = e.v.kind == ir::Value::Global &&
                                            (m.find(e.v.name) || std::find(m.declarations.begin(), m.declarations.end(),
                                                                           e.v.name) != m.declarations.end());
                            const bool null = e.v.kind == ir::Value::Null || e.v.kind == ir::Value::Zero;
                            if (!fn && !null) continue;
                            nlohmann::json j;
                            j["unit"] = unit;
                            j["fn"] = "@" + gname;
                            j["struct"] = ty.text;
                            j["off"] = lay.field_offset(rt, static_cast<unsigned>(k));
                            j["width"] = lay.store_size(fty);
                            j["kind"] = fn ? "func" : "null";
                            if (fn) j["value"] = e.v.name;
                            j["init"] = true;
                            out << j.dump() << '\n';
                        } else if (e.v.kind == ir::Value::Aggregate) {
                            walk(gname, fty, e.v, depth + 1);
                        }
                    }
                } else if (rt.kind == ir::Type::Array && !rt.elems.empty()) {
                    for (const auto& e : v.elems) walk(gname, rt.elems[0], e.v, depth + 1);
                }
            } catch (const std::exception&) {
            } catch (const pirmem::Unenc&) {
            }
        };
    for (const auto& g : m.globals)
        if (!g.init.empty()) walk(g.name, g.ty, g.init[0].v, 0);
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

// --pir-param-preconditions FILE (docs/PIR.md "Parameter preconditions"). TSV, 6 columns:
//   function  parameter  condition  scope  status  source
// '#' lines, blank lines and a header line starting with "function\t" are skipped.
std::vector<ParamPrecondition> load_param_preconditions(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = "cannot read " + file.string();
        return {};
    }
    std::vector<ParamPrecondition> out;
    std::string line;
    const auto bad = [&](int no, const std::string& why) {
        error = file.string() + ":" + std::to_string(no) + ": " + why;
        return std::vector<ParamPrecondition>{};
    };
    for (int no = 1; std::getline(in, line); ++no) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.starts_with("function\t")) continue;
        std::vector<std::string> c;
        std::stringstream ss(line);
        for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
        if (c.size() != 6) return bad(no, "6 tab-separated columns expected (function, parameter, condition, scope, status, source)");
        ParamPrecondition r;
        r.function = c[0].starts_with("@") ? c[0].substr(1) : c[0];
        r.param = c[1];
        r.condition = c[2];
        r.scope = c[3].empty() ? "*" : c[3];
        r.status = c[4];
        r.source = c[5];
        if (r.function.empty() || r.param.empty()) return bad(no, "function and parameter are required");
        if (r.status.empty() || r.source.empty()) return bad(no, "status and source are required");
        std::vector<std::string> w;
        std::stringstream cs(r.condition);
        for (std::string x; cs >> x;) w.push_back(x);
        if (w.empty()) return bad(no, "empty condition");
        r.kind = w[0];
        try {
            if (r.kind == "valid" && w.size() == 2) {
                std::size_t used = 0;
                r.a = std::stoll(w[1], &used);
                if (used != w[1].size() || r.a <= 0 || r.a > (int64_t{1} << 30)) return bad(no, "valid needs a size in (0, 1 GiB]");
            } else if (r.kind == "range" && w.size() == 3) {
                std::size_t u1 = 0, u2 = 0;
                r.a = std::stoll(w[1], &u1);
                r.b = std::stoll(w[2], &u2);
                if (u1 != w[1].size() || u2 != w[2].size() || r.a > r.b) return bad(no, "range needs integers A <= B");
            } else if (r.kind == "nonnull" && w.size() == 1) {
            } else if (r.kind == "type" && (w.size() == 2 || (w.size() == 3 && (w[1] == "struct" || w[1] == "union")))) {
                std::string t = w.size() == 3 ? w[1] + "." + w[2] : w[1];
                if (t.starts_with("%")) t = t.substr(1);
                if (!t.starts_with("struct.") && !t.starts_with("union.")) t = "struct." + t;
                r.type_name = "%" + t;
            } else {
                return bad(no, "condition must be 'valid N', 'type S', 'nonnull' or 'range A B'");
            }
        } catch (const std::exception&) {
            return bad(no, "condition numbers must be integers");
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::string param_precondition_key(const ParamPrecondition& r) {
    return "param-precondition: " + r.function + "(" + r.param + ") " + r.condition + " (";
}

std::string param_precondition_text(const ParamPrecondition& r) {
    return param_precondition_key(r) + "--pir-param-preconditions; scope " + r.scope + "; status " + r.status +
           "; source " + r.source + "; not checked here)";
}

}  // namespace prism::pir
