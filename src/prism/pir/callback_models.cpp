// Driver environment models (--pir-callback-models, docs/DRIVER_ENV_MODELS.md): which kind of driver callback a function
// is (the ops-table field or registration call the unit binds it to) and the preconditions the kernel core guarantees for
// its parameters. Pure IR analysis; translate() turns each binding into named assumptions.

#include "prism/pir.hpp"
#include "translate_mem.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace prism::pir {

namespace {

constexpr uint64_t kAnyField = ~uint64_t{0};

// One place the unit registers a function: (container, byte offset) and, for a constant initialiser, the instance
struct Site {
    std::string container;   // "%struct.T" or "call:FN"
    uint64_t off = 0;
    const ir::Value* inst = nullptr;  // the T aggregate of a constant initialiser naming the function
    ir::Type inst_ty;
};

void walk_init(const pirmem::Layout& lay, const ir::Type& ty, const ir::Value& v, const std::string& fn,
               std::vector<Site>& out, int depth) {
    if (depth > 8 || v.kind != ir::Value::Aggregate) return;
    try {
        const auto& rt = lay.resolve(ty);
        if (rt.kind == ir::Type::Struct) {
            for (std::size_t k = 0; k < v.elems.size(); ++k) {
                const auto& e = v.elems[k];
                if (e.v.kind == ir::Value::Global && e.ty.kind == ir::Type::Ptr && e.v.name == fn) {
                    Site s;
                    s.container = ty.text;
                    s.off = lay.field_offset(rt, static_cast<unsigned>(k));
                    s.inst = &v;
                    s.inst_ty = ty;
                    out.push_back(std::move(s));
                } else if (e.v.kind == ir::Value::Aggregate) {
                    walk_init(lay, lay.field_type(rt, static_cast<unsigned>(k)), e.v, fn, out, depth + 1);
                }
            }
        } else if (rt.kind == ir::Type::Array && !rt.elems.empty()) {
            for (const auto& e : v.elems)
                if (e.v.kind == ir::Value::Aggregate) walk_init(lay, rt.elems[0], e.v, fn, out, depth + 1);
        }
    } catch (const pirmem::Unenc&) {
    }
}

// `getelementptr %struct.T, p, 0, K` as an instruction or a constant expression: (T, byte offset of field K)
std::optional<std::pair<std::string, uint64_t>> field_site(const pirmem::Layout& lay, const ir::Type& ety,
                                                           const std::vector<ir::Operand>& ops) {
    if (ops.size() != 3) return std::nullopt;
    if (ops[1].v.kind != ir::Value::Int || ops[1].v.bits != 0 || ops[2].v.kind != ir::Value::Int) return std::nullopt;
    try {
        const auto& rt = lay.resolve(ety);
        if (rt.kind != ir::Type::Struct) return std::nullopt;
        return std::make_pair(ety.text, lay.field_offset(rt, static_cast<unsigned>(ops[2].v.bits)));
    } catch (const pirmem::Unenc&) {
        return std::nullopt;
    }
}

std::vector<Site> sites_of(const ir::Module& m, const pirmem::Layout& lay, const std::string& fn) {
    std::vector<Site> out;
    for (const auto& g : m.globals)
        if (!g.init.empty()) walk_init(lay, g.ty, g.init[0].v, fn, out, 0);
    for (const auto& f : m.functions) {
        if (f.is_model) continue;
        std::map<std::string, const ir::Inst*> defs;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts)
                if (!in.result.empty()) defs[in.result] = &in;
        for (const auto& b : f.blocks)
            for (const auto& in : b.insts) {
                if (in.op == "store" && in.ops.size() >= 2 && in.ops[0].v.kind == ir::Value::Global && in.ops[0].v.name == fn) {
                    const auto& slot = in.ops[1].v;
                    std::optional<std::pair<std::string, uint64_t>> k;
                    if (slot.kind == ir::Value::Local) {
                        if (auto it = defs.find(slot.name); it != defs.end() && it->second->op == "getelementptr")
                            k = field_site(lay, it->second->ety, it->second->ops);
                    } else if (slot.kind == ir::Value::ConstExpr && slot.ce_op == "getelementptr") {
                        k = field_site(lay, slot.ce_ty, slot.elems);
                    }
                    if (k) out.push_back(Site{k->first, k->second, nullptr, {}});
                } else if ((in.op == "call" || in.op == "invoke") && !in.callee.empty()) {
                    for (std::size_t i = 0; i < in.ops.size(); ++i)
                        if (in.ops[i].v.kind == ir::Value::Global && in.ops[i].v.name == fn)
                            out.push_back(Site{"call:" + in.callee, i, nullptr, {}});
                }
            }
    }
    return out;
}

int64_t sval(const ir::Operand& o) {
    uint64_t b = o.v.bits;
    const unsigned w = o.ty.bits ? o.ty.bits : 64;
    if (w < 64) {
        const uint64_t mk = (uint64_t{1} << w) - 1;
        b &= mk;
        if (b >> (w - 1)) b |= ~mk;
    }
    return static_cast<int64_t>(b);
}

std::string model_text(const CallbackModel& r, const std::string& fn, const std::string& cond) {
    return "driver environment model: " + r.interface_ + " callback " + r.callback + " (@" + fn + ", parameter " +
           std::to_string(r.param) + "): " + cond + " (from " + r.source + "; --pir-callback-models; status " +
           r.status + "; not checked here)";
}

}  // namespace

std::vector<CallbackModel> load_callback_models(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = "cannot read " + file.string();
        return {};
    }
    std::vector<CallbackModel> out;
    std::string line;
    static const std::set<std::string> kinds{"range", "id-entry", "trailing-alloc", "buffer"};
    for (int no = 1; std::getline(in, line); ++no) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.starts_with("interface\t")) continue;
        std::vector<std::string> c;
        for (std::string::size_type s = 0;;) {
            const auto p = line.find('\t', s);
            c.push_back(line.substr(s, p == std::string::npos ? std::string::npos : p - s));
            if (p == std::string::npos) break;
            s = p + 1;
        }
        if (c.size() != 10) {
            error = "line " + std::to_string(no) + ": expected 10 tab-separated columns";
            return {};
        }
        CallbackModel r;
        r.interface_ = c[0];
        r.container = c[1];
        r.callback = c[3];
        r.kind = c[5];
        r.a = c[6];
        r.b = c[7];
        r.status = c[8];
        r.source = c[9];
        try {
            std::size_t used = 0;
            if (c[2] == "*") {
                r.field_off = kAnyField;  // every callback field of the container
            } else {
                r.field_off = std::stoull(c[2], &used);
                if (used != c[2].size()) throw std::invalid_argument(c[2]);
            }
            r.param = std::stoi(c[4], &used);
            if (used != c[4].size() || r.param < 0) throw std::invalid_argument(c[4]);
            if (r.kind == "range") {
                (void)std::stoll(r.a);
                (void)std::stoll(r.b);
                if (std::stoll(r.a) > std::stoll(r.b)) throw std::invalid_argument("empty range");
            } else if (r.kind == "id-entry") {
                (void)std::stoull(r.a);
            } else if (r.kind == "buffer") {
                if (std::stoull(r.a) == 0 || std::stoull(r.a) > (uint64_t{1} << 24)) throw std::invalid_argument("buffer size");
            }
        } catch (const std::exception&) {
            error = "line " + std::to_string(no) + ": bad number";
            return {};
        }
        if (!kinds.count(r.kind)) {
            error = "line " + std::to_string(no) + ": unknown kind " + r.kind;
            return {};
        }
        if (!r.container.starts_with("%struct.") && !r.container.starts_with("%union.") && !r.container.starts_with("call:")) {
            error = "line " + std::to_string(no) + ": container must be %struct.T or call:FN";
            return {};
        }
        if (r.source.empty() || r.status.empty()) {
            error = "line " + std::to_string(no) + ": status and source are required";
            return {};
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<CallbackBinding> resolve_callback_bindings(const ir::Module& m, const ir::Function& f,
                                                       const std::vector<CallbackModel>& models,
                                                       std::vector<std::string>* skipped) {
    std::vector<CallbackBinding> out;
    if (models.empty()) return out;
    pirmem::Layout lay(m);
    const auto sites = sites_of(m, lay, f.name);
    if (sites.empty()) return out;
    auto skip = [&](const CallbackModel& r, const std::string& why) {
        if (skipped) skipped->push_back("driver environment model not applied: " + r.interface_ + " callback " + r.callback +
                                        " parameter " + std::to_string(r.param) + " (" + r.kind + "): " + why);
    };
    std::set<std::pair<int, std::string>> have;  // (param, kind): one binding of a kind per parameter
    for (const auto& r : models) {
        std::vector<const Site*> hits;
        for (const auto& s : sites)
            if (s.container == r.container && (r.field_off == kAnyField || s.off == r.field_off)) hits.push_back(&s);
        if (hits.empty()) continue;
        if (static_cast<std::size_t>(r.param) >= f.params.size()) {
            skip(r, "the function has " + std::to_string(f.params.size()) + " parameters");
            continue;
        }
        const auto& p = f.params[static_cast<std::size_t>(r.param)];
        if (have.count({r.param, r.kind})) continue;
        CallbackBinding b;
        b.param = r.param;
        b.kind = r.kind;
        if (r.kind == "range") {
            if (p.ty.kind != ir::Type::Int) {
                skip(r, "parameter is not an integer");
                continue;
            }
            b.lo = std::stoll(r.a);
            b.hi = std::stoll(r.b);
            b.text = model_text(r, f.name, p.name + " is in [" + r.a + ", " + r.b + "] (signed)");
        } else if (r.kind == "id-entry") {
            if (p.ty.kind != ir::Type::Ptr) {
                skip(r, "parameter is not a pointer");
                continue;
            }
            // the container instances naming the callback: all must name the same id table
            std::set<std::string> tables;
            bool bad = false;
            const uint64_t want = std::stoull(r.a);
            for (const auto* s : hits) {
                if (!s->inst) {  // registered by a store: the instance is not a constant
                    bad = true;
                    continue;
                }
                try {
                    const auto& rt = lay.resolve(s->inst_ty);
                    bool found = false;
                    for (std::size_t k = 0; k < s->inst->elems.size(); ++k)
                        if (lay.field_offset(rt, static_cast<unsigned>(k)) == want) {
                            const auto& e = s->inst->elems[k];
                            if (e.v.kind == ir::Value::Global) tables.insert(e.v.name);
                            else bad = true;
                            found = true;
                            break;
                        }
                    if (!found) bad = true;
                } catch (const pirmem::Unenc&) {
                    bad = true;
                }
            }
            if (bad || tables.size() != 1) {
                skip(r, "no single constant id table in the unit's container instances");
                continue;
            }
            const auto* g = m.find_global(*tables.begin());
            if (!g || g->external || g->init.empty()) {
                skip(r, "id table @" + *tables.begin() + " has no initialiser in the unit");
                continue;
            }
            try {
                const auto& at = lay.resolve(g->ty);
                const uint64_t eb = at.kind == ir::Type::Array && !at.elems.empty() ? lay.alloc_size(at.elems[0]) : 0;
                const uint64_t n = eb ? lay.alloc_size(g->ty) / eb : 0;
                if (n < 2) {
                    skip(r, "id table @" + g->name + " is not an array of at least 2 entries");
                    continue;
                }
                b.global = g->name;
                b.elem_bytes = eb;
                b.count = n - 1;
            } catch (const pirmem::Unenc&) {
                skip(r, "id table @" + g->name + " has no layout");
                continue;
            }
            b.text = model_text(r, f.name,
                                p.name + " points to one of the entries 0.." + std::to_string(b.count - 1) + " of the driver's id table @" +
                                    b.global + " (" + std::to_string(b.count + 1) + " entries, the last the terminator; the core "
                                    "probes with the entry that matched)");
        } else if (r.kind == "buffer") {
            if (p.ty.kind != ir::Type::Ptr) {
                skip(r, "parameter is not a pointer");
                continue;
            }
            b.extra_bytes = std::stoull(r.a);
            b.text = model_text(r, f.name, p.name + " points to the start of a valid buffer of " + r.a +
                                               " bytes, initialised with arbitrary values");
        } else if (r.kind == "trailing-alloc") {
            if (p.ty.kind != ir::Type::Ptr) {
                skip(r, "parameter is not a pointer");
                continue;
            }
            // a = "fn:arg,fn:arg": the private size is the constant argument of every such call in the unit
            std::map<std::string, std::size_t> fns;
            for (std::string::size_type s = 0; s <= r.a.size();) {
                const auto e = r.a.find(',', s);
                const std::string item = r.a.substr(s, e == std::string::npos ? std::string::npos : e - s);
                if (const auto c = item.find(':'); c != std::string::npos) {
                    try {
                        fns[item.substr(0, c)] = std::stoul(item.substr(c + 1));
                    } catch (const std::exception&) {
                    }
                }
                if (e == std::string::npos) break;
                s = e + 1;
            }
            std::set<int64_t> sizes;
            bool nonconst = false;
            for (const auto& g : m.functions) {
                if (g.is_model) continue;
                for (const auto& bl : g.blocks)
                    for (const auto& in : bl.insts) {
                        if (in.op != "call" && in.op != "invoke") continue;
                        auto it = fns.find(in.callee);
                        if (it == fns.end()) continue;
                        if (it->second >= in.ops.size() || in.ops[it->second].v.kind != ir::Value::Int) nonconst = true;
                        else sizes.insert(sval(in.ops[it->second]));
                    }
            }
            if (nonconst || sizes.size() != 1 || *sizes.begin() <= 0 || *sizes.begin() > (int64_t{1} << 24)) {
                skip(r, sizes.empty() && !nonconst ? "the unit allocates no such object"
                                                   : "the unit's allocations do not agree on one constant private size");
                continue;
            }
            b.extra_bytes = static_cast<uint64_t>(*sizes.begin());
            b.struct_type = r.b;
            b.text = model_text(r, f.name,
                                p.name + " is a " + r.b + " allocated by this driver (" + r.a + ") with its private area of " +
                                    std::to_string(b.extra_bytes) + " bytes after the struct (arbitrary contents)");
        }
        have.insert({r.param, r.kind});
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace prism::pir
