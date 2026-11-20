// leandb: pin file, import into sorted shards, manifest, verify.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include <forge/ann/shard_ann.hpp>
#include <forge/db/parallel.hpp>
#include <forge/db/shards.hpp>
#include <forge/db/sqlite.hpp>
#include <forge/task/pool.hpp>
#include <nlohmann/json.hpp>

#include "internal.hpp"
#include "prism/sim/ann.hpp"
#include "prism/sim/lean_goal.hpp"
#include "prism/sim/symbolic_metric.hpp"
#include "prism/solver.hpp"

namespace prism::leandb {
using json = nlohmann::ordered_json;
namespace db = forge::db;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
std::string dump(const json& j) { return j.dump(1, ' ', false, json::error_handler_t::replace) + "\n"; }

// The id comes from the row's natural key (file, line, kind, name; or the dataset id and statement hash), never from the
// input database's row id, which depends on which sources were normalised before it: a rebuild of one source from upstream
// then reproduces exactly that source's shards.
std::int64_t stable_id(const std::string& source, int tbl, const std::string& natural) {
    const auto h = solver::sha256_hex(source + std::string(1, '\0') + std::to_string(tbl) + std::string(1, '\0') + natural);
    return static_cast<std::int64_t>(std::stoull(h.substr(0, 15), nullptr, 16));  // 60 bits
}

}  // namespace

std::string sort_key(const item& it) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%020lld", static_cast<long long>(it.id));
    return std::string(1, static_cast<char>('0' + detail::use_class(it.use))) + "|" + std::to_string(detail::tbl_code(it.tbl)) + "|" + it.source + "|" +
           it.module + "|" + it.name + "|" + buf;
}

const pin_source* pin::find(const std::string& name) const {
    for (const auto& s : sources)
        if (s.name == name) return &s;
    return nullptr;
}

std::optional<pin> load_pin(const fs::path& file, std::string* why) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        if (why) *why = "no pin file " + file.string();
        return std::nullopt;
    }
    const std::string text = slurp(file);
    try {
        const auto j = json::parse(text);
        pin p;
        p.sha256 = solver::sha256_hex(text);
        for (const auto& s : j.at("sources")) {
            pin_source ps;
            ps.name = s.at("name").get<std::string>();
            ps.kind = s.value("kind", "");
            ps.url = s.value("url", s.value("repo", ""));
            ps.record = s.value("record", "");
            ps.revision = s.value("revision", "");
            ps.use = s.value("use", "data");
            ps.license = s.value("license", "");
            ps.redistributable = s.value("redistributable", false);
            ps.evidence = s.value("evidence", "");
            p.sources.push_back(std::move(ps));
        }
        return p;
    } catch (const std::exception& e) {
        if (why) *why = std::string("bad pin file: ") + e.what();
        return std::nullopt;
    }
}

namespace {

struct key_rec {
    std::string key;
    std::int64_t row = 0;
    int tbl = 0;
    std::uint32_t src = 0;
    std::uint64_t bytes = 0;
};

struct src_info {
    std::string name, use, license;
    bool redistributable = false;
    bool unverified = false;  // provenance 'unverified' in the input: stored, never indexed
};

struct shard_out {
    fs::path tmp;
    std::uint64_t bytes = 0, rows = 0, indexed = 0, tokens = 0;
    std::string first_key, last_key, sha256;
    std::set<std::string> sources;
    bool eval = false;
};

std::string hex_to_blob(const std::string& h) {
    if (h.size() != 64) return {};
    std::string out;
    for (std::size_t i = 0; i < 64; i += 2) {
        auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
        const int a = nib(h[i]), b = nib(h[i + 1]);
        if (a < 0 || b < 0) return {};
        out += static_cast<char>(a * 16 + b);
    }
    return out;
}

struct row_item {
    item it;
    std::uint32_t src = 0;
};

// Fetch the rows of one table by id from the input database.
void fetch_rows(db::connection& in, int tbl, const std::vector<std::int64_t>& ids, const std::vector<std::uint32_t>& src_of,
                const std::vector<src_info>& srcs, const std::map<std::string, std::uint32_t>& src_index, std::vector<row_item>& out) {
    (void)src_of;
    const char* sql[] = {
        "SELECT d.id, d.source, d.kind, COALESCE(NULLIF(d.module,''),d.namespace,''), COALESCE(NULLIF(d.full_name,''),d.name,''), d.signature, d.body, d.docstring, "
        "d.norm_hash, d.eval_overlap, d.searchable, "
        "d.file || ':' || COALESCE(d.line,0) || ':' || d.kind || ':' || COALESCE(NULLIF(d.full_name,''),d.name,'') FROM decls d WHERE d.id IN ",
        "SELECT p.id, p.source, COALESCE(p.proof_form,''), COALESCE(p.split,''), COALESCE(p.orig_id,''), p.formal_statement, p.formal_proof, p.use, p.norm_hash, "
        "p.eval_overlap, 1, "
        "COALESCE(p.orig_id,'') || ':' || COALESCE(p.norm_hash,'') FROM problems p WHERE p.id IN ",
        "SELECT t.id, t.source, 'trace', COALESCE(t.file,''), COALESCE(t.full_name,''), t.statement, NULL, NULL, t.norm_hash, t.eval_overlap, 1, "
        "COALESCE(t.file,'') || ':' || COALESCE(t.full_name,'') || ':' || COALESCE(t.start_line,0) FROM trace_theorems t WHERE t.id IN "};
    for (std::size_t b = 0; b < ids.size(); b += 400) {
        const std::size_t e = std::min(ids.size(), b + 400);
        std::string q = sql[tbl];
        q += "(";
        for (std::size_t i = b; i < e; ++i) q += (i > b ? "," : "") + std::to_string(ids[i]);
        q += ")";
        auto st = in.prepare(q);
        for (auto& r : st.rows()) {
            row_item ri;
            item& it = ri.it;
            it.tbl = detail::tbl_name(tbl);
            it.orig_id = 0;  // the input row id is not stored (see stable_id)
            it.source = r.get<std::string>(1);
            ri.src = src_index.at(it.source);
            it.use = srcs[ri.src].use;
            if (tbl == 0) {
                it.kind = r.get<std::string>(2);
                it.module = r.get<std::string>(3);
                it.name = r.get<std::string>(4);
                it.statement = r.get<std::string>(5);
                it.proof = r.is_null(6) ? "" : r.get<std::string>(6);
                const std::string doc = r.is_null(7) ? "" : r.get<std::string>(7);
                if (!doc.empty()) it.statement += "\n/-- " + doc + " -/";
                it.searchable = r.get<std::int64_t>(10) != 0;
            } else if (tbl == 1) {
                it.kind = r.get<std::string>(2);
                it.module = r.get<std::string>(3);
                it.name = r.is_null(4) ? "" : r.get<std::string>(4);
                it.statement = r.get<std::string>(5);
                it.proof = r.is_null(6) ? "" : r.get<std::string>(6);
                it.use = r.get<std::string>(7);
            } else {
                it.kind = "trace";
                it.module = r.get<std::string>(3);
                it.name = r.get<std::string>(4);
                it.statement = r.is_null(5) ? "" : r.get<std::string>(5);
            }
            it.norm_hash = r.is_null(8) ? "" : r.get<std::string>(8);
            it.eval_overlap = r.get<std::int64_t>(9) != 0;
            it.id = stable_id(it.source, tbl, r.get<std::string>(11));
            if (it.use == "eval" || it.eval_overlap || srcs[ri.src].unverified) it.searchable = false;
            out.push_back(std::move(ri));
        }
    }
}

struct set_result {
    std::uint64_t items = 0, searchable = 0, eval_items = 0, eval_overlap = 0, shards = 0, bytes = 0;
};

db::shard_schema make_schema(bool eval, const src_info& src, std::uint32_t src_id) {
    db::shard_schema sc;
    sc.ddl = std::string(detail::kItemsSql) + (eval ? "" : detail::kIndexSql);
    sc.table = "items";
    sc.columns = {"key", "id", "src", "tbl", "kind", "module", "name", "statement", "proof", "norm_hash", "eval_overlap", "searchable", "simhash", "symvec"};
    sc.key_index = 0;
    sc.schema_version = kSchemaVersion;
    if (!eval) sc.fts_table = "fts";
    const forge::ann::hnsw_params hp{16, 100, detail::kEfSearch, 0x5eed};
    auto ann_text = forge::ann::ann_finalize<std::vector<std::uint64_t>, forge::ann::hamming>(
        "SELECT id, simhash FROM items WHERE searchable<>0 AND simhash IS NOT NULL ORDER BY id", hp, forge::ann::hamming{}, "ann_text");
    auto ann_sym = forge::ann::ann_finalize<sim::sym_vec, sim::symbolic_metric>(
        "SELECT id, symvec FROM items WHERE searchable<>0 AND symvec IS NOT NULL ORDER BY id", hp, sim::symbolic_metric{}, "ann_sym");
    sc.finalize = [=](db::connection& c) {
        c.execute("INSERT INTO srcs(id,name,use,license) VALUES(?,?,?,?)", static_cast<std::int64_t>(src_id), src.name, src.use, src.license);
        auto m = c.prepare("INSERT INTO meta(key,value) VALUES(?,?)");
        m.run("schema", std::to_string(kSchemaVersion));
        m.run("kind", eval ? "eval" : "search");
        if (eval) return;
        c.exec("INSERT INTO excluded SELECT * FROM items WHERE searchable=0");
        c.exec("DELETE FROM items WHERE searchable=0");
        c.exec("INSERT INTO fts(rowid,name,statement) SELECT id,name,statement FROM items WHERE searchable<>0 ORDER BY id");
        c.exec("INSERT INTO indexed(id) SELECT id FROM items WHERE searchable<>0 ORDER BY id");
        c.exec("INSERT INTO fts(fts) VALUES('optimize')");
        if (c.scalar<std::int64_t>("SELECT count(*) FROM items WHERE searchable<>0 AND simhash IS NOT NULL") > 0) ann_text(c);
        if (c.scalar<std::int64_t>("SELECT count(*) FROM items WHERE searchable<>0 AND symvec IS NOT NULL") > 0) ann_sym(c);
    };
    return sc;
}

std::vector<std::byte> bytes_of(const std::vector<std::uint64_t>& v) {
    std::vector<std::byte> out;
    forge::ann::codec<std::vector<std::uint64_t>>::encode(v, out);
    return out;
}

set_result build_set(const fs::path& input, const fs::path& dir, const std::string& klass, const std::vector<src_info>& srcs,
                     const std::map<std::string, std::uint32_t>& src_index, const std::function<bool(const src_info&)>& take, const import_options& opt,
                     const pin& pn, const std::string& input_sha, std::atomic<std::uint64_t>& translatable, std::atomic<std::uint64_t>& tried) {
    fs::create_directories(dir);
    for (const char* sub : {"search", "eval"}) {
        std::error_code ec;
        fs::remove_all(dir / sub, ec);
    }
    db::open_options ro;
    ro.read_only = true;
    ro.create = false;
    auto in = db::connection::open(input.string(), ro);
    // ---- pass 1: keys ----
    std::vector<key_rec> keys;
    const char* ksql[] = {
        "SELECT d.id, d.source, COALESCE(NULLIF(d.module,''),d.namespace,''), COALESCE(NULLIF(d.full_name,''),d.name,''), COALESCE(length(d.signature),0)+COALESCE(length(d.body),0)+"
        "COALESCE(length(d.docstring),0), s.use, 0, d.eval_overlap, "
        "d.file || ':' || COALESCE(d.line,0) || ':' || d.kind || ':' || COALESCE(NULLIF(d.full_name,''),d.name,'') FROM decls d JOIN sources s ON s.name=d.source",
        "SELECT p.id, p.source, COALESCE(p.split,''), COALESCE(p.orig_id,''), COALESCE(length(p.formal_statement),0)+COALESCE(length(p.formal_proof),0), p.use, 1, p.eval_overlap, "
        "COALESCE(p.orig_id,'') || ':' || COALESCE(p.norm_hash,'') FROM problems p",
        "SELECT t.id, t.source, COALESCE(t.file,''), COALESCE(t.full_name,''), COALESCE(length(t.statement),0), s.use, 2, t.eval_overlap, "
        "COALESCE(t.file,'') || ':' || COALESCE(t.full_name,'') || ':' || COALESCE(t.start_line,0) FROM trace_theorems t JOIN sources s ON s.name=t.source"};
    set_result res;
    std::vector<std::uint8_t> is_eval;  // parallel to keys
    for (int tbl = 0; tbl < 3; ++tbl) {
        if (!in.table_exists(detail::tbl_name(tbl))) continue;
        auto st = in.prepare(ksql[tbl]);
        for (auto& r : st.rows()) {
            const std::string source = r.get<std::string>(1);
            auto si = src_index.find(source);
            if (si == src_index.end() || !take(srcs[si->second])) continue;
            if (!opt.only_sources.empty() && std::find(opt.only_sources.begin(), opt.only_sources.end(), source) == opt.only_sources.end()) continue;
            item it;
            it.id = stable_id(source, tbl, r.get<std::string>(8));
            it.source = source;
            it.use = r.get<std::string>(5);
            it.tbl = detail::tbl_name(tbl);
            it.module = r.get<std::string>(2);
            it.name = r.get<std::string>(3);
            key_rec k;
            k.key = sort_key(it);
            k.row = r.get<std::int64_t>(0);
            k.tbl = tbl;
            k.src = si->second;
            k.bytes = static_cast<std::uint64_t>(r.get<std::int64_t>(4)) + it.name.size() + it.module.size();
            keys.push_back(std::move(k));
            is_eval.push_back(it.use == "eval");
            ++res.items;
            if (it.use == "eval") ++res.eval_items;
            if (r.get<std::int64_t>(7)) ++res.eval_overlap;
            if (opt.max_items && res.items >= opt.max_items) break;
        }
    }
    std::vector<std::size_t> order(keys.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a].key < keys[b].key; });
    // ---- groups: contiguous runs of the sorted order with one (use class, table, source) ----
    // A shard never spans two groups: a source's shards depend on that source's rows only, so an update of one source
    // rewrites only its shards, and rebuilding one source from upstream reproduces exactly its shards.
    struct group { bool eval; std::uint32_t src; int tbl; std::vector<std::size_t> idx; };
    std::vector<group> groups;
    for (auto i : order) {
        const bool ev = is_eval[i] != 0;
        if (groups.empty() || groups.back().eval != ev || groups.back().src != keys[i].src || groups.back().tbl != keys[i].tbl)
            groups.push_back({ev, keys[i].src, keys[i].tbl, {}});
        groups.back().idx.push_back(i);
    }
    // ---- write each group through forge::db::shard_writer, then merge the group directories into one set per kind ----
    forge::task::pool pool(opt.threads);
    struct built { db::shard_manifest m; fs::path dir; bool eval; };
    std::vector<built> done;
    db::shard_options so;
    so.presorted = true;
    so.budget = std::min<std::uint64_t>(opt.shard_bytes, kShardMaxBytes);
    so.wave_bytes = 128ull << 20;
    const fs::path scratch = dir / ".scratch";
    fs::remove_all(scratch);
    std::size_t gi = 0;
    for (auto& g : groups) {
        const fs::path gdir = scratch / ("g" + std::to_string(gi++));
        db::shard_writer w(gdir, make_schema(g.eval, srcs[g.src], g.src), so, &pool);
        std::set<std::int64_t> seen;  // identical natural keys are the same declaration: keep the first (ids are primary keys)
        for (std::size_t b = 0; b < g.idx.size(); b += 20000) {
            const std::size_t e = std::min(g.idx.size(), b + 20000);
            std::vector<std::int64_t> ids;
            for (std::size_t j = b; j < e; ++j) ids.push_back(keys[g.idx[j]].row);
            std::vector<row_item> rows;
            auto conn = db::connection::open(input.string(), ro);
            fetch_rows(conn, g.tbl, ids, {}, srcs, src_index, rows);
            std::sort(rows.begin(), rows.end(), [](const row_item& a, const row_item& c) { return sort_key(a.it) < sort_key(c.it); });
            std::vector<row_item> uniq;
            for (auto& r : rows)
                if (seen.insert(r.it.id).second) uniq.push_back(std::move(r));
            std::vector<std::vector<db::value>> out(uniq.size());
            forge::task::parallel_blocks(&pool, uniq.size(), 64, [&](std::size_t, std::size_t lo, std::size_t hi) {
                for (std::size_t j = lo; j < hi; ++j) {
                    item& it = uniq[j].it;
                    db::value sh = std::monostate{}, sy = std::monostate{}, nh = std::monostate{};
                    if (!g.eval && it.searchable) {
                        sh = bytes_of(detail::simhash(it.name + " " + it.statement));
                        if (it.tbl != "trace_theorems") {
                            ++tried;
                            if (auto t = sim::lean_goal_to_term(it.statement)) {
                                std::vector<std::byte> bb;
                                sim::encode_sym_vec(sim::embed(*t), bb);
                                sy = std::move(bb);
                                ++translatable;
                            }
                        }
                    }
                    const std::string h = hex_to_blob(it.norm_hash);
                    if (!h.empty()) nh = std::vector<std::byte>(reinterpret_cast<const std::byte*>(h.data()), reinterpret_cast<const std::byte*>(h.data()) + h.size());
                    out[j] = {sort_key(it), it.id, static_cast<std::int64_t>(uniq[j].src), static_cast<std::int64_t>(detail::tbl_code(it.tbl)), it.kind, it.module,
                              it.name, it.statement, it.proof, std::move(nh), static_cast<std::int64_t>(it.eval_overlap ? 1 : 0),
                              static_cast<std::int64_t>(it.searchable && !g.eval ? 1 : 0), std::move(sh), std::move(sy)};
                }
            });
            for (auto& row : out) w.add(std::move(row));
        }
        done.push_back({w.finish(), gdir, g.eval});
    }
    // ---- merge: one directory per kind, shards renumbered in key order, one manifest ----
    json man;
    std::set<std::string> used;
    for (bool eval : {false, true}) {
        db::shard_manifest fin;
        bool first = true;
        std::size_t n = 0;
        const fs::path sdir = dir / (eval ? "eval" : "search");
        for (auto& d : done) {
            if (d.eval != eval) continue;
            if (first) { fin = d.m; fin.shards.clear(); fin.rows = 0; fin.bytes = 0; first = false; fs::create_directories(sdir); }
            for (auto info : d.m.shards) {
                char nm[32];
                std::snprintf(nm, sizeof nm, "shard-%06zu.fdb", n++);
                fs::rename(d.dir / info.file, sdir / nm);
                info.file = nm;
                fin.rows += info.rows;
                fin.bytes += info.bytes;
                if (!eval && info.fts_rows > 0) res.searchable += static_cast<std::uint64_t>(info.fts_rows);
                fin.shards.push_back(std::move(info));
            }
        }
        if (first) continue;
        std::ofstream(sdir / "manifest.json", std::ios::binary) << fin.to_text();
        res.shards += fin.shards.size();
        res.bytes += fin.bytes;
    }
    fs::remove_all(scratch);
    for (auto i : order) used.insert(srcs[keys[i].src].name);
    json srcj = json::array();
    for (const auto& n : used) {
        const auto* ps = pn.find(n);
        json s;
        s["name"] = n;
        if (ps) {
            s["use"] = ps->use;
            s["license"] = ps->license;  // exactly as pinned
            s["revision"] = ps->revision.empty() ? ps->record : ps->revision;
            s["redistributable"] = ps->redistributable;
        }
        srcj.push_back(std::move(s));
    }
    man["schema"] = kSchemaVersion;
    man["class"] = klass;
    man["shard_max_bytes"] = kShardMaxBytes;
    man["layout"] = "search/ and eval/ are forge::db shard sets (manifest.json each); sort key: use class | table | source | module | name | id";
    man["input_sha256"] = input_sha;
    man["pin_sha256"] = pn.sha256;
    man["sources"] = std::move(srcj);
    man["totals"] = {{"items", res.items}, {"searchable", res.searchable}, {"eval_items", res.eval_items}, {"shards", res.shards}, {"bytes", res.bytes}};
    std::ofstream(dir / "leandb.json", std::ios::binary) << dump(man);
    return res;
}

}  // namespace

import_report import_db(const fs::path& input, const import_options& opt) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    auto pn = load_pin(opt.pin_file, &why);
    if (!pn) throw std::runtime_error("leandb import: " + why);
    db::open_options ro;
    ro.read_only = true;
    ro.create = false;
    std::vector<src_info> srcs;
    std::map<std::string, std::uint32_t> src_index;
    {
        auto in = db::connection::open(input.string(), ro);
        auto st = in.prepare("SELECT name, use, license, provenance FROM sources ORDER BY name");
        for (auto& r : st.rows()) {
            src_info s;
            s.name = r.get<std::string>(0);
            s.use = r.get<std::string>(1);
            // the licence text the pin records wins; the input's is the fallback
            const auto* ps = pn->find(s.name);
            s.license = ps ? ps->license : (r.is_null(2) ? "" : r.get<std::string>(2));
            s.redistributable = ps ? ps->redistributable : false;  // not in the pin: not redistributable
            s.unverified = !r.is_null(3) && r.get<std::string>(3) == "unverified";
            src_index[s.name] = static_cast<std::uint32_t>(srcs.size());
            srcs.push_back(std::move(s));
        }
    }
    import_report rep;
    rep.input_sha256 = solver::sha256_file(input);
    std::atomic<std::uint64_t> translatable{0}, tried{0};
    if (!opt.out_committed.empty()) {
        auto r = build_set(input, opt.out_committed, "committed", srcs, src_index, [](const src_info& s) { return s.redistributable; }, opt, *pn, rep.input_sha256,
                           translatable, tried);
        rep.committed_shards = r.shards;
        rep.committed_bytes = r.bytes;
        rep.items += r.items;
        rep.searchable += r.searchable;
        rep.eval_items += r.eval_items;
        rep.eval_overlap += r.eval_overlap;
    }
    if (!opt.out_local.empty()) {
        auto r = build_set(input, opt.out_local, "local", srcs, src_index, [](const src_info& s) { return !s.redistributable; }, opt, *pn, rep.input_sha256,
                           translatable, tried);
        rep.local_shards = r.shards;
        rep.local_bytes = r.bytes;
        rep.items += r.items;
        rep.searchable += r.searchable;
        rep.eval_items += r.eval_items;
        rep.eval_overlap += r.eval_overlap;
    }
    if (!opt.out_all.empty()) {
        auto r = build_set(input, opt.out_all, "all", srcs, src_index, [](const src_info&) { return true; }, opt, *pn, rep.input_sha256, translatable, tried);
        rep.committed_shards += r.shards;
        rep.committed_bytes += r.bytes;
        rep.items += r.items;
        rep.searchable += r.searchable;
        rep.eval_items += r.eval_items;
        rep.eval_overlap += r.eval_overlap;
    }
    rep.translatable = translatable;
    rep.goals_tried = tried;
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}


verify_report verify(const fs::path& dir, const fs::path& pin_file, bool is_local) {
    verify_report vr;
    auto bad = [&](const std::string& m) { vr.ok = false; vr.problems.push_back(m); };
    std::error_code ec;
    if (!fs::exists(dir / "leandb.json", ec)) { bad("no leandb.json in " + dir.string()); return vr; }
    json man;
    try { man = json::parse(slurp(dir / "leandb.json")); } catch (const std::exception& e) { bad(std::string("bad leandb.json: ") + e.what()); return vr; }
    std::string why;
    auto pn = load_pin(pin_file, &why);
    if (!pn) bad(why);
    else if (man.value("pin_sha256", "") != pn->sha256) bad("leandb.json pin_sha256 does not match " + pin_file.string());
    const std::string klass = man.value("class", "");
    if (pn)
        for (const auto& s : man.value("sources", json::array())) {
            const auto name = s.value("name", "");
            const auto* ps = pn->find(name);
            if (!ps) { bad("source " + name + " is not in the pin"); continue; }
            if (s.value("license", "") != ps->license) bad("licence of " + name + " differs from the pin");
            if (klass == "committed" && !ps->redistributable) bad("committed set contains non-redistributable source " + name);
        }
    (void)is_local;
    forge::task::pool pool(0);
    for (const char* sub : {"search", "eval"}) {
        const fs::path sd = dir / sub;
        if (!fs::exists(sd / "manifest.json", ec)) continue;
        const bool eval = std::string(sub) == "eval";
        try {
            db::shard_open_options oo;
            oo.verify = db::verify_mode::sha256;
            auto set = db::open(sd, oo);
            // sizes, then the manifest hashes of every shard (a mismatch throws)
            for (std::size_t i = 0; i < set.size(); ++i) {
                ++vr.shards;
                const auto sz = fs::file_size(set.path_of(i), ec);
                vr.bytes += sz;
                if (sz > kShardMaxBytes) bad(set.info(i).file + " is " + std::to_string(sz) + " bytes, over the 10 MiB limit");
            }
            try { set.verify_all(&pool, db::verify_mode::sha256); } catch (const std::exception& e) { bad(std::string(sub) + ": " + e.what()); continue; }
            for (std::size_t i = 0; i < set.size(); ++i) {
                const std::string file = std::string(sub) + "/" + set.info(i).file;
                auto lease = set.acquire(i);
                db::connection& c = *lease;
                if (eval) {
                    if (c.table_exists("fts") || c.table_exists("indexed")) bad(file + ": an eval shard has an index");
                    if (c.table_exists("ann_text") || c.table_exists("ann_sym")) bad(file + ": an eval shard has a vector index");
                    if (c.scalar<std::int64_t>("SELECT count(*) FROM items WHERE searchable<>0") != 0) bad(file + ": eval row marked searchable");
                    continue;
                }
                const auto leak = c.scalar<std::int64_t>("SELECT count(*) FROM items i JOIN srcs s ON s.id=i.src WHERE s.use='eval'");
                if (leak) bad(file + ": " + std::to_string(leak) + " eval rows in a search shard");
                const auto idx_bad = c.scalar<std::int64_t>(
                    "SELECT count(*) FROM indexed x JOIN items i ON i.id=x.id JOIN srcs s ON s.id=i.src WHERE s.use='eval' OR i.eval_overlap<>0 OR i.searchable=0");
                if (idx_bad) bad(file + ": " + std::to_string(idx_bad) + " eval or overlapping rows in the index");
                const auto n_idx = c.scalar<std::int64_t>("SELECT count(*) FROM indexed");
                if (n_idx != c.scalar<std::int64_t>("SELECT count(*) FROM items WHERE searchable<>0")) bad(file + ": indexed table and searchable flags disagree");
                if (set.info(i).fts_rows >= 0 && set.info(i).fts_rows != n_idx) bad(file + ": FTS row count differs from the manifest");
                // vector tables hold searchable rows only
                for (const char* t : {"ann_text", "ann_sym"}) {
                    if (!c.table_exists(t)) continue;
                }
                if (c.scalar<std::int64_t>("SELECT count(*) FROM excluded WHERE symvec IS NOT NULL OR simhash IS NOT NULL") != 0)
                    bad(file + ": vectors stored for excluded rows");
                if (c.scalar<std::int64_t>("SELECT count(*) FROM excluded x JOIN indexed i ON i.id=x.id") != 0) bad(file + ": an excluded row is in the index");
                // every id the FTS index answers for must be in `indexed`: probe by the shard's own vocabulary
                c.exec("CREATE VIRTUAL TABLE IF NOT EXISTS temp.fv USING fts5vocab(main, fts, 'row')");
                if (set.info(i).fts_tokens >= 0 && c.scalar_or<std::int64_t>(0, "SELECT COALESCE(sum(cnt),0) FROM temp.fv") != set.info(i).fts_tokens)
                    bad(file + ": FTS token total differs from the manifest (index and table disagree)");
                const auto stray = c.scalar_or<std::int64_t>(
                    0, "SELECT count(*) FROM (SELECT term FROM temp.fv LIMIT 200) t WHERE EXISTS (SELECT 1 FROM fts WHERE fts MATCH '\"' || t.term || '\"' AND rowid NOT IN "
                       "(SELECT id FROM indexed))");
                if (stray) bad(file + ": FTS answers for rows that are not in `indexed`");
                c.exec("DROP TABLE IF EXISTS temp.fv");
            }
        } catch (const std::exception& e) {
            bad(std::string(sub) + ": " + e.what());
        }
    }
    return vr;
}

}  // namespace prism::leandb
