// pir invariants -> C (`prism svcomp`).
//
// The pir stage exports its proved loop invariants as structured conjuncts
// over IR value names (extra["invariant_conjuncts"], see
// src/prism/pir/houdini.inc export_invariants). The C text needs the C
// variable behind each value at the loop head and its C type: the task is
// compiled once more with full debug information (-g, otherwise the same
// front end and passes as the pir stage) and the llvm.dbg.value records
// give both. A value is exported as variable X only when the mapping is
// certain at the loop head (see current_var); a conjunct only when C's
// meaning of the rendered expression is the proved bit-vector relation
// (see render_conjunct). Anything else is left out: a subset of proved
// conjuncts is still a proved invariant.
#include "internal.hpp"

#include "../stages/common.hpp"
#include "prism/config.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

namespace {

constexpr const char* IR_PASSES = "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer";

struct Record {
    std::string block;
    int index = 0;
    std::string value;
    std::string var;  // metadata id of the DILocalVariable
};

struct IrFunc {
    std::vector<std::string> blocks;
    std::map<std::string, std::vector<std::string>> succ;
    std::map<std::string, std::string> defs;                   // value -> block
    std::map<std::string, std::set<std::string>> phis;         // block -> its phi values
    std::vector<Record> records;                               // dbg.value records
    std::vector<std::pair<std::string, std::vector<std::string>>> loops;  // llvm.loop md -> branch targets
};

using Md = std::map<std::string, std::string>;

std::optional<std::string> md_field(const std::string& body, const std::string& key) {
    Regex re("\\b" + re_escape(key) + R"(: ("[^"]*"|[^,)]+))");
    auto m = re.search_match(body);
    if (!m) return std::nullopt;
    std::string v = m->group(1);
    std::size_t a = 0, b = v.size();
    while (a < b && v[a] == '"') ++a;
    while (b > a && v[b - 1] == '"') --b;
    return v.substr(a, b - a);
}

std::string md_get(const Md& md, const std::string& ref) {
    auto it = md.find(ref);
    return it == md.end() ? "" : it->second;
}

// The function's blocks, value definitions, dbg.value records and loop
// branches, and the module's metadata lines ("!N" -> body).
std::pair<std::optional<IrFunc>, Md> parse_debug_ir(const std::string& ir, const std::string& fn) {
    Md md;
    static const Regex md_re(R"(^(!\d+) = (?:distinct )?(.*)$)");
    auto lines = splitlines(ir);
    for (const auto& line : lines)
        if (auto m = md_re.match_prefix(line)) md[m->group(1)] = m->group(2);
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].starts_with("define ") && lines[i].find("@" + fn + "(") != std::string::npos) {
            start = i;
            break;
        }
    if (!start) return {std::nullopt, md};
    IrFunc f;
    std::string cur = "entry";
    f.blocks.push_back(cur);
    int idx = 0;
    static const Regex value_re(R"(%([\w.]+))");
    {
        const auto& def = lines[*start];
        auto paren = def.find('(');
        if (paren != std::string::npos)
            for (const auto& m : value_re.finditer(std::string_view(def).substr(paren + 1))) f.defs[m.group(1)] = cur;
    }
    static const Regex label_re(R"(^([\w.]+):)");
    static const Regex def_re(R"(%([\w.]+) = (\w+))");
    static const Regex dbg_re(R"(@llvm\.dbg\.value\(metadata \S+ (%[\w.]+|[-\w.]+), metadata (!\d+),)");
    static const Regex target_re(R"(label %([\w.]+))");
    static const Regex loop_re(R"(!llvm\.loop (!\d+))");
    for (std::size_t i = *start + 1; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line.starts_with("}")) break;
        if (auto lab = label_re.match_prefix(line)) {
            cur = lab->group(1);
            if (std::find(f.blocks.begin(), f.blocks.end(), cur) == f.blocks.end()) f.blocks.push_back(cur);
            idx = 0;
            continue;
        }
        std::string t = strip(line);
        if (t.empty() || t.front() == ';') continue;
        ++idx;
        if (auto d = def_re.match_prefix(t)) {
            f.defs[d->group(1)] = cur;
            if (d->group(2) == "phi") f.phis[cur].insert(d->group(1));
        }
        if (auto r = dbg_re.search_match(t)) {
            std::string v = r->group(1);
            f.records.push_back(Record{cur, idx, v.starts_with("%") ? v.substr(1) : "#" + v, r->group(2)});
        }
        if (t.starts_with("br ") || t.starts_with("switch ")) {
            std::vector<std::string> targets;
            for (const auto& m : target_re.finditer(t)) targets.push_back(m.group(1));
            auto& s = f.succ[cur];
            s.insert(s.end(), targets.begin(), targets.end());
            if (auto lm = loop_re.search_match(t)) {
                auto it = std::find_if(f.loops.begin(), f.loops.end(), [&](const auto& p) { return p.first == lm->group(1); });
                if (it == f.loops.end()) f.loops.emplace_back(lm->group(1), targets);
                else it->second.insert(it->second.end(), targets.begin(), targets.end());
            }
        }
    }
    return {f, md};
}

std::vector<std::string> scope_chain(const Md& md, std::optional<std::string> ref) {
    std::vector<std::string> out;
    while (ref && !ref->empty() && std::find(out.begin(), out.end(), *ref) == out.end() && out.size() < 64) {
        out.push_back(*ref);
        std::string body = md_get(md, *ref);
        ref = body.find("DILexicalBlock") != std::string::npos ? md_field(body, "scope") : std::nullopt;
    }
    return out;
}

// (signed, bits) of an integer C type (through typedefs and qualifiers).
std::optional<std::pair<bool, int>> c_type(const Md& md, std::optional<std::string> ref) {
    static const Regex derived_tag(R"(tag: DW_TAG_(typedef|const_type|volatile_type))");
    for (int k = 0; k < 16; ++k) {
        std::string body = md_get(md, ref.value_or(""));
        if (body.starts_with("!DIBasicType(")) {
            std::string enc = md_field(body, "encoding").value_or("");
            int size = static_cast<int>(parse_ll(md_field(body, "size").value_or("0")).value_or(0));
            if (enc == "DW_ATE_signed" || enc == "DW_ATE_signed_char") return std::make_pair(true, size);
            if (enc == "DW_ATE_unsigned" || enc == "DW_ATE_unsigned_char") return std::make_pair(false, size);
            return std::nullopt;  // _Bool, floating point
        }
        if (body.starts_with("!DIDerivedType(") && derived_tag.search(body)) {
            ref = md_field(body, "baseType");
            continue;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Blocks reachable from src's successors without passing through stop.
std::set<std::string> reach(const IrFunc& f, const std::string& src, const std::string& stop) {
    std::set<std::string> seen;
    std::vector<std::string> work;
    if (auto it = f.succ.find(src); it != f.succ.end()) work = it->second;
    while (!work.empty()) {
        std::string b = work.back();
        work.pop_back();
        if (seen.contains(b) || b == stop) continue;
        seen.insert(b);
        if (auto it = f.succ.find(b); it != f.succ.end()) work.insert(work.end(), it->second.begin(), it->second.end());
    }
    return seen;
}

// The C variable whose current value at the loop head (the start of
// header) is value, with its C type; nullopt unless certain:
//  * the value is described by dbg.value records of exactly one variable,
//    whose name no other variable of the function has, and whose scope
//    encloses the loop;
//  * after its record in the defining block D, no other record of that
//    variable lies on a path from D to the header (the header's own phi
//    records included, unless the value is that phi).
std::optional<CVar> current_var(const IrFunc& f, const Md& md, const std::string& value, const std::string& header,
                                const std::vector<std::string>& loop_scope) {
    std::vector<const Record*> recs;
    std::set<std::string> vars;
    for (const auto& r : f.records)
        if (r.value == value) {
            recs.push_back(&r);
            vars.insert(r.var);
        }
    if (vars.size() != 1) return std::nullopt;
    const std::string var = *vars.begin();
    const std::string body = md_get(md, var);
    auto name = md_field(body, "name");
    static const Regex ident(R"([A-Za-z_]\w*\z)");
    if (!name || !fullmatch(ident, *name)) return std::nullopt;
    std::set<std::string> all_vars;
    for (const auto& r : f.records) all_vars.insert(r.var);
    int same_name = 0;
    for (const auto& v : all_vars)
        if (md_field(md_get(md, v), "name") == name) ++same_name;
    if (same_name != 1) return std::nullopt;
    auto scope = md_field(body, "scope");
    if (!scope || std::find(loop_scope.begin(), loop_scope.end(), *scope) == loop_scope.end()) return std::nullopt;
    auto ty = c_type(md, md_field(body, "type"));
    if (!ty) return std::nullopt;
    auto dit = f.defs.find(value);
    if (dit == f.defs.end()) return std::nullopt;
    const std::string& d = dit->second;
    int last = -1;
    for (const auto* r : recs)
        if (r->block == d) last = std::max(last, r->index);
    if (last < 0) return std::nullopt;
    if (d != header) {
        for (const auto& r : f.records)
            if (r.var == var && r.block == d && r.index > last) return std::nullopt;
        std::set<std::string> toward = {header};
        for (const auto& b : f.blocks)
            if (b == header || reach(f, b, d).contains(header)) toward.insert(b);
        std::set<std::string> region;
        for (const auto& b : reach(f, d, d))
            if (toward.contains(b)) region.insert(b);
        for (const auto& r : f.records)
            if (r.var == var && region.contains(r.block)) return std::nullopt;
    } else {
        auto pit = f.phis.find(header);
        if (pit == f.phis.end() || !pit->second.contains(value)) return std::nullopt;  // defined in the header after the loop head
    }
    return CVar{*name, ty->first, ty->second};
}

const std::map<std::string, std::string>& rel_c() {
    static const std::map<std::string, std::string> k = {{"eq", "=="}, {"ne", "!="}, {"ule", "<="}, {"ult", "<"},
                                                         {"uge", ">="}, {"ugt", ">"}, {"sle", "<="}, {"slt", "<"},
                                                         {"sge", ">="}, {"sgt", ">"}};
    return k;
}

std::optional<__int128> big_int(const json& v) {
    if (v.is_number_integer()) return static_cast<__int128>(v.get<long long>());
    if (v.is_number_unsigned()) return static_cast<__int128>(v.get<unsigned long long>());
    if (!v.is_string()) return std::nullopt;
    std::string s = strip(v.get<std::string>());
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i >= s.size() || s.size() - i > 38) return std::nullopt;
    __int128 x = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        x = x * 10 + (s[i] - '0');
    }
    return neg ? -x : x;
}

}  // namespace

std::optional<std::string> render_conjunct(const json& conj,
                                           const std::function<std::optional<CVar>(const json&)>& term_of) {
    if (!conj.is_object()) return std::nullopt;
    const std::string rel = get_str(conj, "rel");
    auto side = [&](const json& j, bool is_signed) -> std::optional<std::string> {
        if (has(j, "v")) {
            auto tv = term_of(j);
            if (!tv) return std::nullopt;
            return tv->name;
        }
        long long w = has(j, "w") ? to_int(j.at("w")).value_or(0) : 0;
        if (!has(j, "c") || w < 1 || w > 64) return std::nullopt;
        auto v = big_int(j.at("c"));
        if (!v) return std::nullopt;
        const __int128 one = 1;
        if (is_signed && *v >= (one << (w - 1))) *v -= one << w;
        if (is_signed || *v <= 2147483647) return int_text(*v);
        return int_text(*v) + (w <= 32 ? "U" : "UL");
    };
    const json a = has(conj, "a") && truthy(conj.at("a")) ? conj.at("a") : json::object();
    const json b = has(conj, "b") && truthy(conj.at("b")) ? conj.at("b") : json::object();
    auto width = [](const json& j) { return has(j, "w") ? to_int(j.at("w")).value_or(0) : 0; };
    std::optional<CVar> ta = has(a, "v") ? term_of(a) : std::nullopt;
    if (!ta || width(a) != ta->bits) return std::nullopt;
    std::optional<CVar> tb = has(b, "v") ? term_of(b) : std::nullopt;
    if (has(b, "v") && (!tb || width(b) != tb->bits)) return std::nullopt;
    const int w = ta->bits;
    if (w < 1 || w > 64) return std::nullopt;
    const __int128 one = 1;
    if (rel.starts_with("mask:")) {
        auto m = parse_ll(rel.substr(5));
        if (!m || !has(b, "c") || static_cast<__int128>(*m) >= (one << (w - 1))) return std::nullopt;
        auto c = big_int(b.at("c"));
        if (!c) return std::nullopt;
        return "(" + ta->name + " & " + std::to_string(*m) + ") == " + int_text(*c & static_cast<__int128>(*m));
    }
    if (rel == "add" || rel == "sub") {
        const json c = has(conj, "c") && truthy(conj.at("c")) ? conj.at("c") : json::object();
        if (!tb || ta->is_signed || tb->is_signed || w < 32 || tb->bits != w || !has(c, "c"))
            return std::nullopt;  // only unsigned int/long arithmetic wraps like the bit-vectors
        auto sc = side(c, false);
        if (!sc) return std::nullopt;
        return ta->name + " " + (rel == "add" ? "+" : "-") + " " + tb->name + " == " + *sc;
    }
    auto it = rel_c().find(rel);
    if (it == rel_c().end()) return std::nullopt;
    std::optional<std::string> sb;
    if (rel == "eq" || rel == "ne") {
        if (tb && w < 32 && ta->is_signed != tb->is_signed) return std::nullopt;  // promotion keeps values: bit-equal is not value-equal
        sb = side(b, ta->is_signed);
    } else if (rel[0] == 's') {
        if (!ta->is_signed || (tb && !tb->is_signed)) return std::nullopt;
        sb = side(b, true);
    } else {
        if (!tb) {
            if (ta->is_signed) return std::nullopt;
        } else if (w < 32 && (ta->is_signed || tb->is_signed)) {
            return std::nullopt;
        } else if (w >= 32 && ta->is_signed && tb->is_signed) {
            return std::nullopt;
        }
        sb = side(b, false);
    }
    if (!sb) return std::nullopt;
    return ta->name + " " + it->second + " " + *sb;
}

std::optional<std::string> debug_ir(const fs::path& task) {
    Config cfg;
    auto cc = cfg.which({"clang-18", "clang"});
    auto opt = cfg.which({"opt-18", "opt"});
    if (!cc || !opt) return std::nullopt;
    auto r = stages_detail::run_argv({cc->string(), "-x", "c", "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone",
                                      "-fno-discard-value-names", "-fno-builtin-memcpy", "-g", "-std=c17", "-w",
                                      task.string(), "-o", "-"},
                                     "", 120.0);
    if (r.rc != 0 || r.timeout) return std::nullopt;
    auto o = stages_detail::run_argv({opt->string(), "-S", IR_PASSES}, r.out, 120.0);
    if (o.rc != 0 || o.timeout) return std::nullopt;
    return o.out;
}

std::optional<std::vector<std::vector<std::string>>> pir_invariant_texts(const fs::path& task, const json& finding) {
    const json& extra = extra_of(finding);
    json conj, loops;
    try {
        if (!has(extra, "invariant_conjuncts") || !has(extra, "invariant_loops")) return std::nullopt;
        conj = json::parse(pystr(extra.at("invariant_conjuncts")));
        loops = json::parse(pystr(extra.at("invariant_loops")));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    static const Regex line_marker(R"(^[ \t]*#[ \t]*(line[ \t]+)?\d+)", true);
    if (line_marker.search(read_text(task))) return std::nullopt;  // debug locations would name another file's lines
    auto ir = debug_ir(task);
    if (!ir) return std::nullopt;
    std::string fn = has(finding, "function") && truthy(finding.at("function")) ? pystr(finding.at("function")) : "main";
    auto [f, md] = parse_debug_ir(*ir, fn);
    if (!f) return std::nullopt;
    // loop start (line, col) -> header block (the target of the loop's back edge)
    std::map<LineCol, std::string> headers;
    std::map<LineCol, std::vector<std::string>> scopes;
    static const Regex loop_md(R"(!\{(!\d+), (!\d+))");
    for (const auto& [ref, targets] : f->loops) {
        auto m = loop_md.search_match(md_get(md, ref));
        std::set<std::string> uniq(targets.begin(), targets.end());
        if (!m || uniq.size() != 1) continue;
        std::string loc = md_get(md, m->group(2));
        if (loc.find("DILocation") == std::string::npos) continue;
        LineCol key{static_cast<long>(parse_ll(md_field(loc, "line").value_or("0")).value_or(0)),
                    static_cast<long>(parse_ll(md_field(loc, "column").value_or("0")).value_or(0))};
        headers[key] = targets.front();
        scopes[key] = scope_chain(md, md_field(loc, "scope"));
    }
    std::vector<std::vector<std::string>> out;
    if (!loops.is_array()) return out;
    for (std::size_t j = 0; j < loops.size(); ++j) {
        const json& loop = loops[j];
        auto num = [&](const char* k) -> long {
            return has(loop, k) && truthy(loop.at(k)) ? static_cast<long>(to_int(loop.at(k)).value_or(0)) : 0;
        };
        LineCol key{num("line"), num("column")};
        std::vector<std::string> texts;
        auto h = headers.find(key);
        if (h != headers.end() && conj.is_array() && j < conj.size() && conj[j].is_array()) {
            std::map<std::string, std::optional<CVar>> cache;
            const auto& scope = scopes[key];
            auto term_of = [&](const json& t) -> std::optional<CVar> {
                std::string v = has(t, "v") ? pystr(t.at("v")) : "None";
                auto it = cache.find(v);
                if (it == cache.end()) it = cache.emplace(v, current_var(*f, md, v, h->second, scope)).first;
                return it->second;
            };
            for (const auto& c : conj[j]) {
                auto txt = render_conjunct(c, term_of);
                if (txt && !txt->empty() && std::find(texts.begin(), texts.end(), *txt) == texts.end())
                    texts.push_back(*txt);
            }
        }
        out.push_back(std::move(texts));
    }
    return out;
}

}  // namespace prism::svcomp
