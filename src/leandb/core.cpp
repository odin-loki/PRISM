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

#include <forge/db/parallel.hpp>
#include <forge/db/sqlite.hpp>
#include <forge/task/pool.hpp>
#include <nlohmann/json.hpp>

#include "internal.hpp"
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

struct writer_ctx {
    const std::vector<src_info>* srcs;
    bool eval;
};

// Write one shard file from rows already sorted by sort_key. Returns its description.
shard_out write_shard(const fs::path& path, std::vector<row_item>& rows, const std::vector<src_info>& srcs, bool eval, std::atomic<std::uint64_t>& translatable,
                      std::atomic<std::uint64_t>& tried) {
    std::error_code ec;
    fs::remove(path, ec);
    db::open_options o;
    o.foreign_keys = true;
    o.pragmas = {"page_size=4096", "journal_mode=OFF", "synchronous=OFF", "locking_mode=EXCLUSIVE", "temp_store=MEMORY"};
    shard_out so;
    so.tmp = path;
    so.eval = eval;
    {
        auto c = db::connection::open(path.string(), o);
        c.exec(detail::kItemsSql);
        if (!eval) c.exec(detail::kIndexSql);
        db::transaction tx(c);
        std::map<std::uint32_t, bool> used;
        for (const auto& r : rows) used[r.src] = true;
        {
            auto ins = c.prepare("INSERT INTO srcs(id,name,use,license) VALUES(?,?,?,?)");
            for (auto& [s, _] : used) ins.run(static_cast<std::int64_t>(s), srcs[s].name, srcs[s].use, srcs[s].license);
        }
        auto ins = c.prepare("INSERT INTO items(id,src,tbl,orig_id,kind,module,name,statement,proof,norm_hash,eval_overlap,searchable,simhash,symvec) "
                             "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        db::statement fins, iins;
        if (!eval) {
            fins = c.prepare("INSERT INTO fts(rowid,name,statement) VALUES(?,?,?)");
            iins = c.prepare("INSERT INTO indexed(id) VALUES(?)");
        }
        for (auto& r : rows) {
            item& it = r.it;
            std::optional<std::vector<std::byte>> sym;
            std::string sh;
            if (!eval && it.searchable) {
                const auto h = detail::simhash(it.name + " " + it.statement);
                sh.assign(reinterpret_cast<const char*>(h.data()), sizeof h);
                if (it.tbl != "trace_theorems") {
                    ++tried;
                    if (auto t = sim::lean_goal_to_term(it.statement)) {
                        std::vector<std::byte> b;
                        sim::encode_sym_vec(sim::embed(*t), b);
                        sym = std::move(b);
                        ++translatable;
                    }
                }
            }
            const std::string nh = hex_to_blob(it.norm_hash);
            ins.reset();
            ins.clear_bindings();
            ins.bind(1, it.id).bind(2, static_cast<std::int64_t>(r.src)).bind(3, detail::tbl_code(it.tbl)).bind(4, it.orig_id).bind(5, it.kind)
                .bind(6, it.module).bind(7, it.name).bind(8, it.statement).bind(9, it.proof);
            if (nh.empty()) ins.bind(10, nullptr); else ins.bind(10, std::span<const std::byte>(reinterpret_cast<const std::byte*>(nh.data()), nh.size()));
            ins.bind(11, it.eval_overlap ? 1 : 0).bind(12, it.searchable && !eval ? 1 : 0);
            if (sh.empty()) ins.bind(13, nullptr); else ins.bind(13, std::span<const std::byte>(reinterpret_cast<const std::byte*>(sh.data()), sh.size()));
            if (sym) ins.bind(14, std::span<const std::byte>(*sym)); else ins.bind(14, nullptr);
            ins.step();
            if (!eval && it.searchable) {
                fins.reset();
                fins.bind(1, it.id).bind(2, it.name).bind(3, it.statement);
                fins.step();
                iins.reset();
                iins.bind(1, it.id);
                iins.step();
                ++so.indexed;
            }
            so.sources.insert(it.source);
        }
        so.rows = rows.size();
        if (!rows.empty()) {
            so.first_key = sort_key(rows.front().it);
            so.last_key = sort_key(rows.back().it);
        }
        if (!eval) {
            c.exec("INSERT INTO fts(fts) VALUES('optimize')");
            c.exec("CREATE VIRTUAL TABLE temp.fv USING fts5vocab(main, fts, 'row')");
            so.tokens = static_cast<std::uint64_t>(c.scalar_or<std::int64_t>(0, "SELECT COALESCE(sum(cnt),0) FROM temp.fv"));
            c.exec("DROP TABLE temp.fv");
        }
        auto m = c.prepare("INSERT INTO meta(key,value) VALUES(?,?)");
        m.run("schema", std::to_string(kSchemaVersion));
        m.run("kind", eval ? "eval" : "search");
        m.run("rows", std::to_string(so.rows));
        m.run("n_indexed", std::to_string(so.indexed));
        m.run("n_tokens", std::to_string(so.tokens));
        tx.commit();
    }
    so.bytes = fs::file_size(path);
    return so;
}

// Write rows as one shard, splitting in halves until each file is within the limit.
void write_fitting(const fs::path& dir, const std::string& stem, std::vector<row_item>& rows, const std::vector<src_info>& srcs, bool eval,
                   std::vector<shard_out>& out, std::atomic<std::uint64_t>& translatable, std::atomic<std::uint64_t>& tried, int depth = 0) {
    const fs::path tmp = dir / (stem + "." + std::to_string(depth) + "." + std::to_string(out.size()) + ".tmp");
    auto so = write_shard(tmp, rows, srcs, eval, translatable, tried);
    if (so.bytes <= kShardMaxBytes || rows.size() <= 1) {
        out.push_back(std::move(so));
        return;
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    const std::size_t half = rows.size() / 2;
    std::vector<row_item> a(std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.begin() + static_cast<std::ptrdiff_t>(half)));
    std::vector<row_item> b(std::make_move_iterator(rows.begin() + static_cast<std::ptrdiff_t>(half)), std::make_move_iterator(rows.end()));
    write_fitting(dir, stem, a, srcs, eval, out, translatable, tried, depth + 1);
    write_fitting(dir, stem, b, srcs, eval, out, translatable, tried, depth + 1);
}

struct set_result {
    std::uint64_t items = 0, searchable = 0, eval_items = 0, eval_overlap = 0, shards = 0, bytes = 0;
};

set_result build_set(const fs::path& input, const fs::path& dir, const std::string& klass, const std::vector<src_info>& srcs,
                     const std::map<std::string, std::uint32_t>& src_index, const std::function<bool(const src_info&)>& take, const import_options& opt,
                     const pin& pn, const std::string& input_sha, std::atomic<std::uint64_t>& translatable, std::atomic<std::uint64_t>& tried) {
    fs::create_directories(dir);
    for (auto& e : fs::directory_iterator(dir)) {
        const auto n = e.path().filename().string();
        if ((n.rfind("search-", 0) == 0 || n.rfind("eval-", 0) == 0) && (e.path().extension() == ".sqlite" || e.path().extension() == ".tmp")) fs::remove(e.path());
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
    // sorted order over (key); keep is_eval alongside
    std::vector<std::size_t> order(keys.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a].key < keys[b].key; });
    // ---- partition by estimated size (search and eval streams separately; sorted order keeps eval last) ----
    // A shard never spans two (use class, table, source) groups: a source's shards depend on that source's rows only, so an
    // update of one source rewrites only its shards, and rebuilding one source from upstream reproduces exactly its shards.
    struct part { bool eval; std::uint64_t group; std::vector<std::size_t> idx; };
    std::vector<part> parts;
    std::uint64_t acc = 0;
    for (auto i : order) {
        const bool ev = is_eval[i] != 0;
        const std::uint64_t group = (static_cast<std::uint64_t>(keys[i].src) << 8) | static_cast<std::uint64_t>(keys[i].tbl);
        const std::uint64_t est = keys[i].bytes * 2 + 220;
        if (parts.empty() || parts.back().eval != ev || parts.back().group != group || acc + est > opt.shard_bytes) {
            parts.push_back({ev, group, {}});
            acc = 0;
        }
        parts.back().idx.push_back(i);
        acc += est;
    }
    // ---- pass 2: fetch + write each partition in parallel ----
    std::vector<std::vector<shard_out>> done(parts.size());
    std::mutex err_m;
    std::string err;
    forge::task::pool pool(opt.threads);
    forge::task::parallel_blocks(&pool, parts.size(), 1, [&](std::size_t bi, std::size_t, std::size_t) {
        try {
            auto conn = db::connection::open(input.string(), ro);
            std::vector<std::vector<std::int64_t>> ids(3);
            for (auto i : parts[bi].idx) ids[static_cast<std::size_t>(keys[i].tbl)].push_back(keys[i].row);
            std::vector<row_item> rows;
            for (int t = 0; t < 3; ++t)
                if (!ids[static_cast<std::size_t>(t)].empty()) fetch_rows(conn, t, ids[static_cast<std::size_t>(t)], {}, srcs, src_index, rows);
            std::sort(rows.begin(), rows.end(), [](const row_item& a, const row_item& b) { return sort_key(a.it) < sort_key(b.it); });
            {  // identical natural keys are the same declaration: keep the first (ids are primary keys)
                std::set<std::int64_t> seen;
                std::vector<row_item> uniq;
                uniq.reserve(rows.size());
                for (auto& r : rows)
                    if (seen.insert(r.it.id).second) uniq.push_back(std::move(r));
                rows = std::move(uniq);
            }
            write_fitting(dir, "part" + std::to_string(bi), rows, srcs, parts[bi].eval, done[bi], translatable, tried);
        } catch (const std::exception& e) {
            std::lock_guard l(err_m);
            if (err.empty()) err = e.what();
        }
    });
    if (!err.empty()) throw std::runtime_error("leandb import: " + err);
    // ---- final names, hashes, manifest ----
    json man;
    man["schema"] = kSchemaVersion;
    man["class"] = klass;
    man["shard_max_bytes"] = kShardMaxBytes;
    man["sort_key"] = "use class | table | source | module | name | id (docs/LEANDB.md)";
    man["input_sha256"] = input_sha;
    man["pin_sha256"] = pn.sha256;
    json js = json::array(), srcj = json::array();
    std::set<std::string> used;
    std::map<bool, unsigned> counter{{false, 0}, {true, 0}};
    char nm[64];
    for (auto& group : done) {
        for (auto& so : group) {
            std::snprintf(nm, sizeof nm, "%s-%04u.sqlite", so.eval ? "eval" : "search", counter[so.eval]++);
            const fs::path fin = dir / nm;
            fs::rename(so.tmp, fin);
            so.sha256 = solver::sha256_file(fin);
            so.tmp = fin;
            json s;
            s["file"] = nm;
            s["kind"] = so.eval ? "eval" : "search";
            s["bytes"] = so.bytes;
            s["sha256"] = so.sha256;
            s["rows"] = so.rows;
            s["n_indexed"] = so.indexed;
            s["n_tokens"] = so.tokens;
            s["first_key"] = so.first_key;
            s["last_key"] = so.last_key;
            js.push_back(std::move(s));
            res.bytes += so.bytes;
            ++res.shards;
            used.insert(so.sources.begin(), so.sources.end());
            if (!so.eval) res.searchable += so.indexed;
        }
    }
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
    man["sources"] = std::move(srcj);
    man["shards"] = std::move(js);
    man["totals"] = {{"items", res.items}, {"searchable", res.searchable}, {"eval_items", res.eval_items}, {"shards", res.shards}, {"bytes", res.bytes}};
    std::ofstream(dir / "manifest.json", std::ios::binary) << dump(man);
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
    if (!fs::exists(dir / "manifest.json", ec)) { bad("no manifest.json in " + dir.string()); return vr; }
    json man;
    try { man = json::parse(slurp(dir / "manifest.json")); } catch (const std::exception& e) { bad(std::string("bad manifest: ") + e.what()); return vr; }
    std::string why;
    auto pn = load_pin(pin_file, &why);
    if (!pn) bad(why);
    else if (man.value("pin_sha256", "") != pn->sha256) bad("manifest pin_sha256 does not match " + pin_file.string());
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
    std::set<std::string> seen;
    for (const auto& s : man.value("shards", json::array())) {
        const std::string file = s.value("file", "");
        const fs::path p = dir / file;
        ++vr.shards;
        if (!fs::exists(p, ec)) { bad("missing shard " + file); continue; }
        const auto sz = fs::file_size(p);
        vr.bytes += sz;
        if (sz > kShardMaxBytes) bad(file + " is " + std::to_string(sz) + " bytes, over the 10 MiB limit");
        if (sz != s.value("bytes", std::uint64_t{0})) bad(file + " size differs from the manifest");
        if (solver::sha256_file(p) != s.value("sha256", "")) { bad(file + " sha256 differs from the manifest"); continue; }
        try {
            db::open_options ro;
            ro.read_only = true;
            ro.create = false;
            auto c = db::connection::open(p.string(), ro);
            const bool eval = s.value("kind", "") == "eval";
            if (eval) {
                if (c.table_exists("fts") || c.table_exists("indexed")) bad(file + ": an eval shard has an index");
                if (c.scalar<std::int64_t>("SELECT count(*) FROM items WHERE searchable<>0") != 0) bad(file + ": eval row marked searchable");
                continue;
            }
            const auto leak = c.scalar<std::int64_t>("SELECT count(*) FROM items i JOIN srcs s ON s.id=i.src WHERE s.use='eval'");
            if (leak) bad(file + ": " + std::to_string(leak) + " eval rows in a search shard");
            const auto idx_bad = c.scalar<std::int64_t>(
                "SELECT count(*) FROM indexed x JOIN items i ON i.id=x.id JOIN srcs s ON s.id=i.src WHERE s.use='eval' OR i.eval_overlap<>0 OR i.searchable=0");
            if (idx_bad) bad(file + ": " + std::to_string(idx_bad) + " eval or overlapping rows in the index");
            const auto n_idx = c.scalar<std::int64_t>("SELECT count(*) FROM indexed");
            const auto n_flag = c.scalar<std::int64_t>("SELECT count(*) FROM items WHERE searchable<>0");
            if (n_idx != n_flag) bad(file + ": indexed table and searchable flags disagree");
            if (static_cast<std::uint64_t>(n_idx) != s.value("n_indexed", std::uint64_t{0})) bad(file + ": n_indexed differs from the manifest");
            c.exec("CREATE VIRTUAL TABLE temp.fv USING fts5vocab(main, fts, 'row')");
            if (static_cast<std::uint64_t>(c.scalar_or<std::int64_t>(0, "SELECT COALESCE(sum(cnt),0) FROM temp.fv")) != s.value("n_tokens", std::uint64_t{0}))
                bad(file + ": FTS token total differs from the manifest (index and table disagree)");
            // every id the FTS index answers for must be in `indexed`: probe by the shard's own vocabulary
            const auto stray = c.scalar_or<std::int64_t>(
                0, "SELECT count(*) FROM (SELECT term FROM temp.fv LIMIT 200) t WHERE EXISTS (SELECT 1 FROM fts WHERE fts MATCH '\"' || t.term || '\"' AND rowid NOT IN "
                   "(SELECT id FROM indexed))");
            if (stray) bad(file + ": FTS answers for rows that are not in `indexed`");
        } catch (const std::exception& e) {
            bad(file + ": " + e.what());
        }
    }
    return vr;
}

}  // namespace prism::leandb
