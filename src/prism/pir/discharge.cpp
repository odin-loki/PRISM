// Call-site discharge of the automatic pointer harness: the call graph of a unit, the grounding over a call graph,
// and the whole-build merge (docs/HARNESS_GENERATOR.md "Whole-build call-site discharge", docs/PIR.md).

#include "discharge.hpp"

#include "prism/laws.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>

namespace prism::pir::discharge {

using nlohmann::json;
namespace fs = std::filesystem;

bool is_harness_line(const std::string& a) {
    return a.find("(auto harness)") != std::string::npos || a.starts_with("auto harness for ") ||
           a.find("pointer parameters point to distinct objects") != std::string::npos;
}

std::string invented_objects_line() {
    return "objects PRISM assumes into existence in the composed proofs (pointer results of unknown or indirect calls, "
           "asm outputs, the global harness, current, field-invariant objects, external objects of library models) "
           "are distinct from the objects the callers pass (the model allocates them fresh; the call sites do not check "
           "it) (call-site discharge)";
}

json obligation_json(const ObligationNode& o) {
    json j;
    j["name"] = o.name;
    j["bytes"] = o.bytes;
    j["member_off"] = o.member_off;
    j["align"] = o.align;
    j["check_init"] = o.check_init;
    json n = json::array();
    for (const auto& [off, c] : o.nested)
        if (c) n.push_back({{"offset", off}, {"node", obligation_json(*c)}});
    j["nested"] = n;
    return j;
}

std::shared_ptr<ObligationNode> obligation_from_json(const json& j) {
    if (!j.is_object() || !j.contains("bytes") || !j["bytes"].is_number_unsigned()) return nullptr;
    auto o = std::make_shared<ObligationNode>();
    o->name = j.value("name", std::string("?"));
    o->bytes = j["bytes"].get<uint64_t>();
    o->member_off = j.value("member_off", uint64_t{0});
    o->align = j.value("align", 16u);
    // a missing flag means the strict check (every byte initialised)
    o->check_init = j.value("check_init", true);
    if (j.contains("nested") && j["nested"].is_array())
        for (const auto& n : j["nested"]) {
            if (!n.is_object() || !n.contains("offset") || !n.contains("node")) return nullptr;
            auto c = obligation_from_json(n["node"]);
            if (!c) return nullptr;
            o->nested.emplace_back(n["offset"].get<uint64_t>(), c);
        }
    return o;
}

namespace {

std::string symbol_at(const std::string& t, std::size_t at, std::size_t* end) {  // `@name` at t[at] == '@'
    std::size_t i = at + 1;
    std::string name;
    if (i < t.size() && t[i] == '"') {
        std::size_t q = t.find('"', i + 1);
        if (q == std::string::npos) q = t.size();
        name = t.substr(i + 1, q - i - 1);
        i = std::min(q + 1, t.size());
    } else {
        while (i < t.size() &&
               (std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_' || t[i] == '.' || t[i] == '$' || t[i] == '-'))
            name += t[i++];
    }
    *end = i;
    return name;
}

void tokens_of(const std::string& s, std::set<std::string>& out) {
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 2 && !std::isdigit(static_cast<unsigned char>(cur[0]))) out.insert(cur);
        cur.clear();
    };
    for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' || ch == '$') cur += ch;
        else flush();
    }
    flush();
}

bool proved_status(const std::string& s) {
    return s == laws::PROVED || s == laws::PROVED_UNBOUNDED || s == laws::PROVED_CERTIFIED;
}

}  // namespace

UnitGraph unit_graph(const ir::Module& m, const std::string& t, const std::vector<OpsField>& ops) {
    UnitGraph g;
    // linkage and module asm, line by line
    std::size_t pos = 0;
    while (pos < t.size()) {
        std::size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        const std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        if (line.starts_with("define ")) {
            auto at = line.find('@');
            if (at == std::string::npos) continue;
            std::size_t ee = 0;
            const auto name = symbol_at(line, at, &ee);
            const auto pre = " " + line.substr(0, at) + " ";
            if (pre.find(" internal ") != std::string::npos || pre.find(" private ") != std::string::npos) g.internal.insert(name);
            for (const char* w : {" weak ", " weak_odr ", " linkonce ", " linkonce_odr ", " common ", " available_externally ",
                                  " extern_weak "})
                if (pre.find(w) != std::string::npos) g.weak.insert(name);
        } else if (line.starts_with("module asm ")) {
            tokens_of(line.substr(11), g.asm_tokens);
        }
    }
    for (std::size_t at = t.find('@'); at != std::string::npos; at = t.find('@', at + 1)) {
        std::size_t e = 0;
        const auto name = symbol_at(t, at, &e);
        if (e >= t.size() || t[e] != '(') g.taken.insert(name);  // not a call, define or declare: its address is used
    }
    for (const auto& fn : m.functions) {
        if (fn.is_declaration || fn.is_model) continue;
        g.defined.insert(fn.name);
        int np = 0;
        for (const auto& p : fn.params)
            if (p.ty.kind == ir::Type::Ptr) ++np;
        g.ptr_params[fn.name] = np;
        std::map<std::string, const ir::Inst*> def;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts)
                if (!in.result.empty()) def[in.result] = &in;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts) {
                if (in.is_asm) {
                    tokens_of(in.asm_text, g.asm_tokens);
                    continue;
                }
                if (!in.callee.empty()) {
                    if (in.callee.starts_with("llvm.")) continue;
                    ++g.calls[fn.name][in.callee];
                    g.callers[in.callee].insert(fn.name);
                    continue;
                }
                if ((in.op != "call" && in.op != "invoke") || !in.callee_op) continue;
                UnitGraph::Indirect ic;
                ic.caller = fn.name;
                if (auto it = m.locs.find(in.dbg); it != m.locs.end()) ic.line = it->second.line;
                ic.kind = "pointer";
                const auto& cop = *in.callee_op;
                if (cop.v.kind == ir::Value::Local)
                    if (auto d = def.find(cop.v.name); d != def.end() && d->second->op == "load" && !d->second->ops.empty()) {
                        const auto& src = d->second->ops[0];
                        const ir::Inst* gep = nullptr;
                        if (src.v.kind == ir::Value::Local)
                            if (auto d2 = def.find(src.v.name); d2 != def.end()) {
                                if (d2->second->op == "getelementptr") gep = d2->second;
                                else if (d2->second->op == "load") ic.kind = "vtable";
                            }
                        if (gep && gep->ety.kind == ir::Type::Struct && gep->ops.size() >= 3 &&
                            gep->ops.back().v.kind == ir::Value::Int) {
                            ic.kind = "field";
                            const int k = static_cast<int>(gep->ops.back().v.bits);
                            ic.detail = gep->ety.text + "." + std::to_string(k);
                            for (const auto& f : ops)
                                if (f.struct_type == gep->ety.text && f.index == k) ic.targets = f.targets;
                        }
                    }
                g.indirect.push_back(std::move(ic));
            }
    }
    for (const auto& d : m.declarations)
        if (!g.defined.count(d)) g.declared.insert(d);
    int ntaken = 0;
    for (const auto& n : g.taken)
        if (g.defined.count(n) || g.declared.count(n)) ++ntaken;
    for (auto& ic : g.indirect) ic.candidates = ntaken;
    return g;
}

json unit_graph_json(const UnitGraph& g, const std::string& file, const std::string& unit) {
    json r;
    r["schema"] = "prism-pir-discharge/1";
    r["record"] = "callgraph";
    r["file"] = file;
    r["unit"] = unit;
    json fns = json::object();
    for (const auto& n : g.defined) {
        json f;
        f["linkage"] = g.internal.count(n) ? "internal" : g.weak.count(n) ? "weak" : "external";
        f["ptr_params"] = g.ptr_params.count(n) ? g.ptr_params.at(n) : 0;
        json c = json::object();
        if (auto it = g.calls.find(n); it != g.calls.end())
            for (const auto& [callee, k] : it->second) c[callee] = k;
        f["calls"] = c;
        fns[n] = f;
    }
    r["functions"] = fns;
    r["declared"] = g.declared;
    json taken = json::array();
    for (const auto& n : g.taken)
        if (g.defined.count(n) || g.declared.count(n)) taken.push_back(n);
    r["taken"] = taken;
    r["asm_tokens"] = g.asm_tokens;
    json ind = json::array();
    for (const auto& ic : g.indirect)
        ind.push_back({{"caller", ic.caller}, {"line", ic.line}, {"kind", ic.kind}, {"detail", ic.detail},
                       {"targets", ic.targets}, {"candidates", ic.candidates}});
    r["indirect"] = ind;
    return r;
}

bool site_proved(const Site& s) { return proved_status(s.verdict); }

std::string Grounder::sites_failure(const std::string& key) const {
    auto it = nodes.find(key);
    if (it == nodes.end()) return "no call-graph node";
    const Node& n = it->second;
    if (!n.closed) return n.why_not;
    for (const auto& c : n.callers) {
        auto s = sites.find({c, key});
        const std::string cname = nodes.count(c) && !nodes.at(c).display.empty() ? nodes.at(c).display : c;
        if (s == sites.end()) return "the call site in " + cname + " was not checked";
        const Site& st = s->second;
        if (site_proved(st)) continue;
        if (st.verdict == laws::FAILED)
            return "the call site in " + cname + (st.line ? " (line " + std::to_string(st.line) + ")" : std::string()) +
                   " does not establish the harness: " + st.message;
        if (st.verdict == "NOT-ANALYSED") return "the call site in " + cname + " could not be analysed (" + st.message + ")";
        if (st.verdict == "NOT-CHECKED") return st.message;
        return "the obligations at the call site in " + cname + " were not decided (" + st.verdict + ")";
    }
    return {};
}

Grounding Grounder::ground(const std::string& key) {
    if (auto m = memo_.find(key); m != memo_.end()) return m->second;
    Grounding r;
    auto disp = [&](const std::string& k) {
        auto it = nodes.find(k);
        return it != nodes.end() && !it->second.display.empty() ? it->second.display : k;
    };
    if (active_.count(key)) {
        r.reason = disp(key) + " reaches itself through its callers (recursion: not grounded)";
        return r;  // not memoised: it depends on the frame that is still open
    }
    auto fail = [&](std::string why) {
        r.ok = false;
        r.reason = std::move(why);
        r.inherited.clear();
        memo_[key] = r;
        return r;
    };
    if (auto f = sites_failure(key); !f.empty()) return fail(f);
    active_.insert(key);
    const Node& n = nodes.at(key);
    std::set<std::string> seen;
    auto add = [&](const std::string& a) {
        if (seen.insert(a).second) r.inherited.push_back(a);
    };
    for (const auto& c : n.callers) {
        const Site& st = sites.at({c, key});
        r.sites += st.sites;
        r.callers.push_back(disp(c));
        r.invented = r.invented || st.invented;
        for (const auto& a : st.assumptions)
            if (std::find(st.caller_harness.begin(), st.caller_harness.end(), a) == st.caller_harness.end()) add(a);
        if (st.caller_harness.empty()) continue;  // the caller has no automatic harness: its context is unconditional
        auto up = ground(c);
        if (!up.ok) {
            active_.erase(key);
            return fail("the call site in " + disp(c) + " rests on " + disp(c) + "'s own harness, which is not discharged: " + up.reason);
        }
        r.invented = r.invented || up.invented;
        for (const auto& a : up.inherited) add(a);
    }
    active_.erase(key);
    r.ok = true;
    memo_[key] = r;
    return r;
}

Outcome outcome(const std::string& before, const std::vector<std::string>& own, const std::vector<std::string>& residual,
                bool own_invented, const Grounding& g, const std::string& discharged_line) {
    Outcome o;
    std::vector<std::string> kept;
    for (const auto& a : own)
        if (!is_harness_line(a)) kept.push_back(a);
    for (const auto& a : residual) kept.push_back(a);
    const bool invented = own_invented || g.invented;
    if (g.ok && kept.empty() && g.inherited.empty() && !invented && proved_status(before)) {
        o.status = before == laws::PROVED_UNBOUNDED ? std::string(laws::PROVED_UNBOUNDED) : std::string(laws::PROVED);
        o.proved = true;
        return o;
    }
    o.status = std::string(laws::PROVED_ASSUMING);
    o.assumptions = kept;
    o.assumptions.push_back(discharged_line);
    for (const auto& a : g.inherited) o.assumptions.push_back("at a call site (caller's context): " + a);
    if (invented) o.assumptions.push_back(invented_objects_line());
    return o;
}

// ---------------------------------------------------------------------------------------------------------------
// Whole build
// ---------------------------------------------------------------------------------------------------------------

namespace {

std::string norm(const std::string& p) {
    if (p.empty()) return p;
    return fs::path(p).lexically_normal().generic_string();
}

std::vector<std::string> shell_split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool any = false;
    char q = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += c;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            any = true;
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i];
            any = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

struct DbUnit {
    std::set<std::string> link_units;
    int commands = 0;
    bool assembler = false;
};

// link units of one compile command (Linux kbuild): a module object has -DMODULE and KBUILD_MODNAME (several names
// joined by ':' for an object linked into several modules); everything else is vmlinux.
std::set<std::string> linux_link_units(const std::vector<std::string>& args) {
    bool module = false;
    std::string modname;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        if (a == "-D" && i + 1 < args.size()) a = "-D" + args[++i];
        if (a == "-DMODULE") module = true;
        if (a.starts_with("-D__KBUILD_MODNAME=")) {
            modname = a.substr(19);
            if (modname.starts_with("kmod_")) modname = modname.substr(5);
        } else if (a.starts_with("-DKBUILD_MODNAME=") && modname.empty()) {
            modname = a.substr(17);
        }
    }
    std::erase_if(modname, [](char c) { return c == '"' || c == '\'' || c == '\\'; });
    std::set<std::string> out;
    if (!module) {
        out.insert("vmlinux");
        return out;
    }
    std::string cur;
    for (char c : modname + ":") {
        if (c == ':') {
            if (!cur.empty()) out.insert("module:" + cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (out.empty()) out.insert("module:?");
    return out;
}

struct CgFile {
    json rec;
    std::map<std::string, std::string> linkage;  // defined function -> linkage
    std::set<std::string> taken, asm_tokens;
    std::map<std::string, std::set<std::string>> callers;  // callee -> callers
    std::map<std::string, std::map<std::string, int>> calls;
};

}  // namespace

WholeBuildResult whole_build(const WholeBuildOptions& opt) {
    WholeBuildResult R;
    // ---- inputs
    std::vector<fs::path> files;
    for (const auto& p : opt.inputs) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(p, ec))
                if (e.is_regular_file() && e.path().extension() == ".jsonl") files.push_back(e.path());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    std::map<std::string, CgFile> cg;                          // file -> call graph
    std::map<std::string, json> plans;                         // key -> plan record
    std::map<std::pair<std::string, std::string>, json> site_recs;  // (caller key, file::callee name) -> site record
    std::vector<json> candidates;
    std::set<std::string> cand_keys;
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in) {
            R.errors.push_back("cannot read " + f.string());
            continue;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto j = json::parse(line, nullptr, false);
            if (!j.is_object() || j.value("schema", "") != "prism-pir-discharge/1") continue;
            const std::string rec = j.value("record", "");
            const std::string file = norm(j.value("file", ""));
            if (rec == "callgraph") {
                if (cg.count(file)) continue;  // the same unit from another run: the same IR
                CgFile c;
                c.rec = j;
                if (j.contains("functions") && j["functions"].is_object())
                    for (const auto& [n, fj] : j["functions"].items()) {
                        c.linkage[n] = fj.value("linkage", "external");
                        if (fj.contains("calls") && fj["calls"].is_object())
                            for (const auto& [callee, k] : fj["calls"].items()) {
                                c.calls[n][callee] = k.get<int>();
                                c.callers[callee].insert(n);
                            }
                    }
                for (const auto& n : j.value("taken", json::array())) c.taken.insert(n.get<std::string>());
                for (const auto& n : j.value("asm_tokens", json::array())) c.asm_tokens.insert(n.get<std::string>());
                cg[file] = std::move(c);
            } else if (rec == "plan") {
                plans.emplace(file + "::" + j.value("function", ""), j);
            } else if (rec == "site") {
                const std::string ck = file + "::" + j.value("caller", "");
                site_recs[{ck, file + "::" + j.value("callee", "")}] = j;  // the latest record wins (a retry)
            } else if (rec == "candidate") {
                const std::string k = file + "::" + j.value("function", "");
                if (cand_keys.insert(k).second) candidates.push_back(j);
            }
        }
    }
    // ---- compile database: link units, assembler units, units compiled more than once
    std::map<std::string, DbUnit> db;
    const bool have_db = !opt.compile_dbs.empty();
    for (const auto& dbp : opt.compile_dbs) {
        std::ifstream in(dbp);
        auto j = in ? json::parse(in, nullptr, false) : json();
        if (!j.is_array()) {
            R.errors.push_back("cannot read the compile database " + dbp.string());
        } else {
            for (const auto& e : j) {
                if (!e.is_object()) continue;
                std::string file = e.value("file", "");
                const std::string dir = e.value("directory", "");
                if (!file.empty() && !fs::path(file).is_absolute() && !dir.empty()) file = (fs::path(dir) / file).string();
                file = norm(file);
                std::vector<std::string> args;
                if (e.contains("arguments") && e["arguments"].is_array())
                    for (const auto& a : e["arguments"]) args.push_back(a.get<std::string>());
                else
                    args = shell_split(e.value("command", ""));
                auto& u = db[file];
                ++u.commands;
                const auto ext = fs::path(file).extension().string();
                // anything but C (assembler, Rust, ...): its calls are not in any call graph here
                u.assembler = u.assembler || (ext != ".c" && ext != ".i");
                if (opt.link_rule == "linux") {
                    if (e.contains("link_units") && e["link_units"].is_array()) {  // the build's own link map
                        for (const auto& l : e["link_units"])
                            if (l.is_string()) u.link_units.insert(l.get<std::string>());
                    } else {
                        for (const auto& l : linux_link_units(args)) u.link_units.insert(l);
                    }
                }
            }
        }
    }
    std::map<std::string, std::vector<std::string>> link_members;  // link unit -> files
    for (const auto& [file, u] : db)
        for (const auto& l : u.link_units)
            if (l != "module:?") link_members[l].push_back(file);
    // a module object whose module is not known may be linked into any module: a member of all of them
    for (const auto& [file, u] : db)
        if (u.link_units.count("module:?"))
            for (auto& [l, files] : link_members)
                if (l.starts_with("module:")) files.push_back(file);

    // ---- nodes: (file, function) of a definition
    auto split_key = [](const std::string& k) {
        auto p = k.rfind("::");
        return std::pair<std::string, std::string>{k.substr(0, p), k.substr(p + 2)};
    };
    // the definition a call of `name` from `file` reaches: the file's own static, else the one external definition in
    // the link units of the file
    auto resolve = [&](const std::string& file, const std::string& name) -> std::string {
        if (auto c = cg.find(file); c != cg.end()) {
            auto l = c->second.linkage.find(name);
            if (l != c->second.linkage.end() && l->second == "internal") return file + "::" + name;
        }
        std::set<std::string> defs;
        auto scan = [&](const std::string& f) {
            auto c = cg.find(f);
            if (c == cg.end()) return;
            auto l = c->second.linkage.find(name);
            if (l != c->second.linkage.end() && l->second != "internal") defs.insert(f);
        };
        if (have_db && db.count(file)) {
            for (const auto& lu : db.at(file).link_units)
                for (const auto& f : link_members[lu]) scan(f);
        } else {
            for (const auto& [f, c] : cg) scan(f);
        }
        if (defs.size() != 1) return {};
        return *defs.begin() + "::" + name;
    };
    Grounder G;
    std::map<std::string, std::string> link_of;  // key -> link units text
    std::function<void(const std::string&)> build_node = [&](const std::string& key) {
        if (G.nodes.count(key)) return;
        auto [file, name] = split_key(key);
        Node n;
        n.display = name;
        G.nodes[key] = n;  // placeholder (cycles)
        auto c = cg.find(file);
        auto no = [&](const std::string& why) {
            G.nodes[key].closed = false;
            G.nodes[key].why_not = why;
        };
        if (c == cg.end()) return no("no call-graph facts for its unit " + file);
        auto lk = c->second.linkage.find(name);
        if (lk == c->second.linkage.end()) return no("not defined in " + file);
        if (auto p = plans.find(key); p == plans.end()) return no("no harness plan was recorded for it");
        else if (auto w = p->second.value("why_no_plan", std::string()); !w.empty()) return no(w);
        std::vector<std::string> callers;
        if (lk->second == "internal") {
            if (c->second.taken.count(name)) return no("its address is taken (it may be called through a pointer)");
            if (c->second.asm_tokens.count(name)) return no("it is named in inline or module assembly");
            if (auto it = c->second.callers.find(name); it != c->second.callers.end())
                for (const auto& f : it->second) callers.push_back(file + "::" + f);
        } else {
            if (lk->second == "weak") return no("weak or linkonce linkage (another definition may replace it)");
            if (opt.link_rule != "linux")
                return no("external linkage, and no link rule closes the build (--link none): code outside the build may call it");
            if (!have_db) return no("external linkage and no compile database: its link unit is not known");
            if (!opt.complete_db)
                return no("external linkage, and the compile database is not declared complete (--complete-db): objects it "
                          "does not list (assembler, Rust, units filtered out) may call it");
            auto du = db.find(file);
            if (du == db.end()) return no("its unit is not in the compile database");
            if (du->second.commands > 1) return no("its unit is compiled more than once in the build");
            std::string lus;
            for (const auto& l : du->second.link_units) lus += (lus.empty() ? "" : ",") + l;
            link_of[key] = lus;
            std::set<std::string> scope;
            for (const auto& l : du->second.link_units)
                for (const auto& f : link_members[l]) scope.insert(f);
            int missing = 0;
            std::string missing_ex, asm_ex;
            for (const auto& f : scope) {
                const auto& u = db.at(f);
                if (u.assembler) {
                    if (asm_ex.empty()) asm_ex = f;
                    continue;
                }
                if (u.commands > 1) return no("unit " + f + " of link unit " + lus + " is compiled more than once");
                if (!cg.count(f)) {
                    if (!missing++) missing_ex = f;
                }
            }
            if (!asm_ex.empty())
                return no("link unit " + lus + " has assembler or other non-C units (" + asm_ex + ", ...), which may call it");
            if (missing)
                return no(std::to_string(missing) + " unit(s) of link unit " + lus + " have no call-graph facts (" + missing_ex +
                          (missing > 1 ? ", ..." : "") + ")");
            int defs = 0;
            for (const auto& f : scope) {
                const auto& cf = cg.at(f);
                auto l = cf.linkage.find(name);
                if (l != cf.linkage.end() && l->second != "internal") ++defs;
                if (cf.taken.count(name)) return no("its address is taken in " + f + " (it may be called through a pointer)");
                if (cf.asm_tokens.count(name)) return no("it is named in inline or module assembly in " + f);
            }
            if (defs != 1) return no(std::to_string(defs) + " external definitions of the name in link unit " + lus);
            for (const auto& f : scope) {
                const auto& cf = cg.at(f);
                if (f != file) {
                    auto l = cf.linkage.find(name);
                    if (l != cf.linkage.end() && l->second == "internal") continue;  // its own static of that name
                }
                if (auto it = cf.callers.find(name); it != cf.callers.end())
                    for (const auto& fn : it->second) callers.push_back(f + "::" + fn);
            }
        }
        if (callers.empty()) return no("no call site in the build (an unused function keeps its assumption)");
        G.nodes[key].closed = true;
        G.nodes[key].callers = callers;
    };
    // ---- sites, by the definition they check
    for (const auto& [k, j] : site_recs) {
        auto [cfile, cname] = split_key(k.first);
        const std::string callee = j.value("callee", "");
        const std::string ckey = resolve(cfile, callee);
        if (ckey.empty()) continue;
        Site s;
        s.caller = k.first;
        s.callee = ckey;
        s.sites = j.value("sites", 0);
        s.verdict = j.value("verdict", "");
        s.message = j.value("message", "");
        s.line = j.value("line", 0);
        for (const auto& a : j.value("assumptions", json::array())) s.assumptions.push_back(a.get<std::string>());
        for (const auto& a : j.value("caller_harness", json::array())) s.caller_harness.push_back(a.get<std::string>());
        s.invented = j.value("invented", false);
        G.sites[{s.caller, s.callee}] = s;
    }
    // ---- the nodes the candidates need: the candidate, and every caller whose site rests on its own harness
    std::map<std::string, json> todo;  // file -> todo record
    auto todo_of = [&](const std::string& file) -> json& {
        auto& t = todo[file];
        if (t.is_null()) t = {{"schema", "prism-pir-discharge/1"}, {"record", "todo"}, {"file", file}, {"facts", false},
                              {"plans", json::array()}, {"callers", json::array()}};
        return t;
    };
    std::set<std::string> visited;
    std::function<void(const std::string&)> demand = [&](const std::string& key) {
        if (!visited.insert(key).second) return;
        build_node(key);
        const Node& n = G.nodes.at(key);
        auto [file, name] = split_key(key);
        if (!n.closed) {
            // missing call-graph facts of the link unit: a facts run of those units
            if (n.why_not.find("have no call-graph facts") != std::string::npos && have_db && db.count(file))
                for (const auto& l : db.at(file).link_units)
                    for (const auto& f : link_members[l])
                        if (!cg.count(f) && !db.at(f).assembler) todo_of(f)["facts"] = true;
            if (n.why_not.find("no call-graph facts for its unit") != std::string::npos) todo_of(file)["facts"] = true;
            return;
        }
        for (const auto& c : n.callers) {
            auto s = G.sites.find({c, key});
            if (s == G.sites.end()) {
                auto [cf, cn] = split_key(c);
                auto& t = todo_of(cf);
                if (auto p = plans.find(key); p != plans.end()) {
                    bool have = false;
                    for (const auto& x : t["plans"])
                        if (x.value("file", "") == file && x.value("function", "") == name) have = true;
                    if (!have) t["plans"].push_back(p->second);
                }
                if (std::find(t["callers"].begin(), t["callers"].end(), json(cn)) == t["callers"].end()) t["callers"].push_back(cn);
                continue;
            }
            if (site_proved(s->second) && !s->second.caller_harness.empty()) demand(c);
        }
    };
    for (const auto& c : candidates) demand(norm(c.value("file", "")) + "::" + c.value("function", ""));
    for (auto& [f, t] : todo) R.todo.push_back(t);

    // ---- decisions
    std::map<std::string, long long> by;
    for (const auto& c : candidates) {
        const std::string file = norm(c.value("file", ""));
        const std::string name = c.value("function", "");
        const std::string key = file + "::" + name;
        json d;
        d["schema"] = "prism-callsite-discharge/1";
        d["record"] = "callsite-discharge";
        d["config"] = opt.config;
        d["link_rule"] = opt.link_rule;
        d["file"] = file;
        d["unit"] = c.value("unit", "");
        d["function"] = name;
        d["source_name"] = c.value("source_name", name);
        d["start_line"] = c.value("start_line", 0);
        d["before"] = c.value("verdict", "");
        if (auto l = link_of.find(key); l != link_of.end()) d["link_unit"] = l->second;
        std::vector<std::string> own, residual;
        for (const auto& a : c.value("assumptions", json::array())) own.push_back(a.get<std::string>());
        if (auto p = plans.find(key); p != plans.end())
            for (const auto& a : p->second.value("residual", json::array())) residual.push_back(a.get<std::string>());
        auto gr = G.ground(key);
        std::string level;
        if (gr.ok) {
            std::string cs;
            for (const auto& x : gr.callers) cs += (cs.empty() ? "" : ", ") + x;
            const std::string line =
                "the automatic harness is discharged over the whole build (configuration " + opt.config + ", link rule " +
                opt.link_rule + "): every call site (" + std::to_string(gr.sites) + " in " + cs +
                ") is checked to pass what the harness assumed, and each caller's context is unconditional or its own "
                "harness is discharged the same way (grounded)";
            auto o = outcome(c.value("verdict_before_assumptions", std::string()), own, residual, c.value("invented", false), gr, line);
            d["verdict"] = o.status;
            d["assumptions"] = o.assumptions;
            d["sites"] = gr.sites;
            d["callers"] = gr.callers;
            level = o.proved ? "proved" : "grounded";
            d["reason"] = o.proved ? json("") : json("assumptions remain (listed)");
        } else {
            d["verdict"] = c.value("verdict", "");
            const auto sf = G.sites_failure(key);
            level = sf.empty() ? "discharged" : "none";
            d["reason"] = gr.reason;
        }
        d["level"] = level;
        ++by[level];
        R.decisions.push_back(d);
    }
    // ---- the call graph, for the record
    for (const auto& [file, c] : cg) {
        for (const auto& [caller, m] : c.calls)
            for (const auto& [callee, k] : m) {
                const auto r = resolve(file, callee);
                R.edges.push_back({{"kind", "direct"}, {"caller", file + "::" + caller}, {"callee", callee},
                                   {"resolved", r}, {"sites", k}});
            }
        for (const auto& ic : c.rec.value("indirect", json::array()))
            R.edges.push_back({{"kind", "indirect-" + ic.value("kind", std::string("pointer"))},
                               {"caller", file + "::" + ic.value("caller", std::string())},
                               {"callee", ic.value("detail", std::string())},
                               {"resolved", ic.value("targets", json::array()).empty() ? "unresolved" : "ops-table targets"},
                               {"sites", 1}});
    }
    R.summary = {{"schema", "prism-callsite-discharge-summary/1"}, {"config", opt.config}, {"link_rule", opt.link_rule},
                 {"units_with_facts", cg.size()}, {"db_units", db.size()}, {"candidates", candidates.size()},
                 {"by_level", by}, {"sites", G.sites.size()}, {"todo_units", R.todo.size()}};
    return R;
}

}  // namespace prism::pir::discharge
