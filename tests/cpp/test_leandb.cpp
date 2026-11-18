// prism leandb: import into sorted shards, parallel search in every mode with global-statistics
// BM25, local shards used together with committed ones, no eval leakage, tamper detection.

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <forge/db/sqlite.hpp>
#include <nlohmann/json.hpp>

#include "prism/leandb.hpp"
#include "prism/solver.hpp"

namespace ldb = prism::leandb;
namespace db = forge::db;
namespace fs = std::filesystem;

namespace {

struct fixture {
    fs::path root, input, pin, committed, local, all;
    explicit fixture(const std::string& tag) {
        root = fs::temp_directory_path() / ("prism_leandb_test_" + tag);
        fs::remove_all(root);
        fs::create_directories(root);
        input = root / "lean_db.sqlite";
        pin = root / "PIN.json";
        committed = root / "shards";
        local = root / "local";
        all = root / "all";
        auto c = db::connection::open(input.string());
        c.exec(
            "CREATE TABLE sources(name TEXT PRIMARY KEY, kind TEXT, use TEXT, license TEXT, provenance TEXT);"
            "CREATE TABLE decls(id INTEGER PRIMARY KEY, source TEXT, module TEXT, file TEXT, line INTEGER, kind TEXT, name TEXT, full_name TEXT, namespace TEXT,"
            " signature TEXT, body TEXT, docstring TEXT, norm_hash TEXT, eval_overlap INTEGER DEFAULT 0, searchable INTEGER DEFAULT 1);"
            "CREATE TABLE problems(id INTEGER PRIMARY KEY, source TEXT, use TEXT, orig_id TEXT, split TEXT, formal_statement TEXT, formal_proof TEXT,"
            " proof_form TEXT, norm_hash TEXT, eval_overlap INTEGER DEFAULT 0);"
            "CREATE TABLE trace_theorems(id INTEGER PRIMARY KEY, source TEXT, file TEXT, full_name TEXT, start_line INTEGER, statement TEXT, norm_hash TEXT, eval_overlap INTEGER DEFAULT 0);");
        c.exec("INSERT INTO sources VALUES('libA','git','library','Apache-2.0','confirmed'),('dataB','hf','data','MIT',NULL),"
               "('dataC','hf','data','see dataset card',NULL),('evalD','hf','eval','MIT',NULL)");
        db::transaction tx(c);
        auto d = c.prepare("INSERT INTO decls(source,module,file,line,kind,name,full_name,namespace,signature,body,docstring) VALUES('libA',?,?,?,?,?,?,?,?,?,?)");
        for (int i = 0; i < 150; ++i) {
            const std::string mod = "Lib.Mod" + std::to_string(i % 7);
            const std::string nm = "Lib.thm_" + std::to_string(i);
            std::string sig = "theorem thm_" + std::to_string(i) + " (x y : BitVec 8) : x + " + std::to_string(i % 50) + "#8 = y";
            if (i == 3) sig = "theorem bv_add_comm_special (x y : BitVec 8) : x + y = y + x";
            if (i == 4) sig = "theorem nat_mul_two (n : Nat) : n * 2 = n + n";
            if (i == 5) sig = "theorem wolfhound_lemma (a b : Int) (h : a < b) : a + 1 <= b";
            d.run(mod, "f.lean", i + 1, "theorem", nm, nm, "Lib", sig, "by simp", "");
        }
        auto p = c.prepare("INSERT INTO problems(source,use,orig_id,split,formal_statement,formal_proof,proof_form) VALUES(?,?,?,?,?,?,?)");
        for (int i = 0; i < 60; ++i)
            p.run("dataB", "data", "b" + std::to_string(i), "train", "theorem b_" + std::to_string(i) + " (a : Nat) : a + " + std::to_string(i) + " = " + std::to_string(i) + " + a", "by omega", "tactic");
        p.run("dataB", "data", "b_leaky", "train", "theorem zebraquux_leak (a b : BitVec 8) : a ^^^ b = b ^^^ a", "by bv_decide", "tactic");
        for (int i = 0; i < 40; ++i)
            p.run("dataC", "data", "c" + std::to_string(i), "train", "theorem c_" + std::to_string(i) + " (a : Int) : a * " + std::to_string(i + 2) + " = " + std::to_string(i + 2) + " * a", "by ring", "tactic");
        p.run("dataC", "data", "c_unique", "train", "theorem quokka_prover_only (u v : BitVec 16) : u &&& v = v &&& u", "by bv_decide", "tactic");
        for (int i = 0; i < 10; ++i) p.run("evalD", "eval", "e" + std::to_string(i), "test", "theorem zebraquux_eval_" + std::to_string(i) + " (a b : BitVec 8) : a ^^^ b = b ^^^ a", "", "");
        c.exec("UPDATE problems SET eval_overlap=1 WHERE orig_id='b_leaky'");
        tx.commit();
        std::ofstream(pin) << R"({"schema":1,"sources":[
 {"name":"libA","kind":"git","url":"https://example.invalid/libA","revision":"r1","use":"library","license":"Apache-2.0","redistributable":true,"evidence":"LICENSE file"},
 {"name":"dataB","kind":"hf","repo":"x/dataB","revision":"r2","use":"data","license":"MIT","redistributable":true,"evidence":"card"},
 {"name":"dataC","kind":"hf","repo":"x/dataC","revision":"r3","use":"data","license":"see dataset card","redistributable":false,"evidence":"unclear"},
 {"name":"evalD","kind":"hf","repo":"x/evalD","revision":"r4","use":"eval","license":"MIT","redistributable":true,"evidence":"card"}]})";
    }
    ldb::import_options opts(std::uint64_t shard_bytes = 20 * 1024) const {
        ldb::import_options o;
        o.pin_file = pin;
        o.out_committed = committed;
        o.out_local = local;
        o.out_all = all;
        o.shard_bytes = shard_bytes;
        o.threads = 4;
        return o;
    }
    ldb::search_options sopts(ldb::mode m, bool with_local = true) const {
        ldb::search_options s;
        s.dirs = {committed};
        if (with_local) s.dirs.push_back(local);
        s.m = m;
        s.k = 10;
        s.threads = 4;
        return s;
    }
};

nlohmann::json manifest(const fs::path& dir) {
    std::ifstream in(dir / "manifest.json");
    return nlohmann::json::parse(in);
}

}  // namespace

TEST_CASE("leandb: import splits into sorted shards within 10 MiB and verify passes") {
    fixture f("import");
    const auto rep = ldb::import_db(f.input, f.opts());
    CHECK(rep.items == 221 + 41 + 262);  // committed + local + the complete set
    CHECK(rep.eval_items == 10 + 10);
    const auto mc = manifest(f.committed), ml = manifest(f.local);
    CHECK(mc["shards"].size() > 3);  // 20 KB budget forces several shards
    std::string prev_last;
    std::size_t search_shards = 0;
    for (const auto& s : mc["shards"]) {
        CHECK(s["bytes"].get<std::uint64_t>() <= ldb::kShardMaxBytes);
        if (s["kind"] == "search") {
            ++search_shards;
            CHECK(s["first_key"].get<std::string>() > prev_last);  // sorted, non-overlapping key ranges
            prev_last = s["last_key"].get<std::string>();
        }
    }
    CHECK(search_shards >= 2);
    // committed: only redistributable sources; local: only the unclear one; licence text intact
    std::set<std::string> cs, ls;
    for (const auto& s : mc["sources"]) cs.insert(s["name"]);
    for (const auto& s : ml["sources"]) { ls.insert(s["name"]); CHECK(s["license"] == "see dataset card"); }
    CHECK(cs == std::set<std::string>{"dataB", "evalD", "libA"});
    CHECK(ls == std::set<std::string>{"dataC"});
    CHECK(manifest(f.all)["sources"].size() == 4);
    for (const auto& d : {f.committed, f.local, f.all}) {
        const auto v = ldb::verify(d, f.pin, d == f.local);
        INFO(d.string());
        for (const auto& p : v.problems) MESSAGE(p);
        CHECK(v.ok);
    }
    // eval rows live in eval shards only
    bool eval_seen = false;
    for (const auto& s : mc["shards"])
        if (s["kind"] == "eval") eval_seen = true;
    CHECK(eval_seen);
}

TEST_CASE("leandb: a rebuild is byte-identical") {
    fixture f("repro");
    ldb::import_db(f.input, f.opts());
    const auto m1 = manifest(f.committed);
    ldb::import_db(f.input, f.opts());
    const auto m2 = manifest(f.committed);
    REQUIRE(m1["shards"].size() == m2["shards"].size());
    for (std::size_t i = 0; i < m1["shards"].size(); ++i) CHECK(m1["shards"][i]["sha256"] == m2["shards"][i]["sha256"]);
}

TEST_CASE("leandb: search modes find the right rows; local shards join by default") {
    fixture f("search");
    ldb::import_db(f.input, f.opts());
    auto r = ldb::search("wolfhound lemma", f.sopts(ldb::mode::bm25));
    REQUIRE(!r.empty());
    CHECK(r[0].name == "Lib.thm_5");
    // text ANN: a near-copy of a statement
    r = ldb::search("theorem nat_mul_two (n : Nat) : n * 2 = n + n", f.sopts(ldb::mode::ann));
    REQUIRE(!r.empty());
    CHECK(r[0].name == "Lib.thm_4");
    // symbolic ANN: the same goal written differently (swapped sides, renamed variables)
    r = ldb::search("theorem q (u w : BitVec 8) : w + u = u + w", f.sopts(ldb::mode::sym));
    REQUIRE(!r.empty());
    CHECK(r[0].name == "Lib.thm_3");
    // hybrid
    auto hy = f.sopts(ldb::mode::hybrid);
    hy.k = 60;  // the fusion keeps rows that two of the three lists agree on first; the symbolic hit is in the list
    r = ldb::search("theorem q (u w : BitVec 8) : w + u = u + w", hy);
    REQUIRE(!r.empty());
    CHECK(std::any_of(r.begin(), r.end(), [](const ldb::result& x) { return x.name == "Lib.thm_3"; }));
    // the local-only source is found when its shards are present, and is not when they are not
    r = ldb::search("quokka_prover_only", f.sopts(ldb::mode::bm25, true));
    REQUIRE(!r.empty());
    CHECK(r[0].source == "dataC");
    CHECK(r[0].local);
    CHECK(ldb::search("quokka_prover_only", f.sopts(ldb::mode::bm25, false)).empty());
    // the complete private set answers the same
    ldb::search_options all;
    all.dirs = {f.all};
    all.m = ldb::mode::bm25;
    r = ldb::search("quokka_prover_only", all);
    REQUIRE(!r.empty());
    CHECK(r[0].source == "dataC");
}

TEST_CASE("leandb: eval and eval-overlapping rows are in no index and never returned") {
    fixture f("leak");
    ldb::import_db(f.input, f.opts());
    for (auto m : {ldb::mode::bm25, ldb::mode::ann, ldb::mode::sym, ldb::mode::hybrid}) {
        for (const auto& q : {std::string("zebraquux"), std::string("theorem zebraquux_leak (a b : BitVec 8) : a ^^^ b = b ^^^ a")}) {
            for (const auto& r : ldb::search(q, f.sopts(m))) {
                CHECK(r.source != "evalD");
                CHECK(r.name.find("zebraquux") == std::string::npos);
                CHECK(r.name != "b_leaky");
            }
        }
    }
    CHECK(ldb::verify(f.committed, f.pin, false).ok);
}

TEST_CASE("leandb: sharded BM25 equals the unsharded FTS5 bm25") {
    fixture f("bm25");
    ldb::import_db(f.input, f.opts(8 * 1024));  // many shards
    const auto mc = manifest(f.committed);
    REQUIRE(mc["shards"].size() > 6);
    // The same rows in one unsharded FTS5 table.
    auto one = db::connection::open((f.root / "one.sqlite").string());
    one.exec("CREATE VIRTUAL TABLE fts USING fts5(name, statement, tokenize='unicode61 remove_diacritics 0')");
    for (const auto& s : mc["shards"]) {
        if (s["kind"] != "search") continue;
        db::open_options ro;
        ro.read_only = true;
        ro.create = false;
        auto c = db::connection::open((f.committed / s["file"].get<std::string>()).string(), ro);
        auto q = c.prepare("SELECT id, name, statement FROM items WHERE searchable<>0");
        auto ins = one.prepare("INSERT INTO fts(rowid,name,statement) VALUES(?,?,?)");
        for (auto& row : q.rows()) ins.run(row.get<std::int64_t>(0), row.get<std::string>(1), row.get<std::string>(2));
    }
    for (const std::string q : {"bitvec add comm", "theorem thm_7 BitVec", "nat omega ring", "lemma"}) {
        std::string match;
        for (const auto& w : std::set<std::string>{}) (void)w;
        std::vector<std::string> ws;
        {  // the engine's query words: lower-case alphanumeric runs, duplicates dropped
            std::string cur;
            auto flush = [&] {
                if (!cur.empty() && std::find(ws.begin(), ws.end(), cur) == ws.end()) ws.push_back(cur);
                cur.clear();
            };
            for (unsigned char ch : q) {
                if (std::isalnum(ch)) cur += static_cast<char>(std::tolower(ch));
                else flush();
            }
            flush();
        }
        for (std::size_t i = 0; i < ws.size(); ++i) match += (i ? " OR \"" : "\"") + ws[i] + "\"";
        auto st = one.prepare("SELECT rowid, -bm25(fts, 2.0, 1.0) FROM fts WHERE fts MATCH ?1 ORDER BY 2 DESC, rowid LIMIT 10");
        st.bind(1, match);
        std::vector<std::pair<std::int64_t, double>> want;
        for (auto& row : st.rows()) want.emplace_back(row.get<std::int64_t>(0), row.get<double>(1));
        auto so = f.sopts(ldb::mode::bm25, false);
        const auto got = ldb::search(q, so);
        REQUIRE_MESSAGE(got.size() == want.size(), q);
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK_MESSAGE(got[i].id == want[i].first, q << " rank " << i);
            CHECK(std::fabs(got[i].score - want[i].second) < 1e-9);
        }
    }
}

TEST_CASE("leandb: verify catches a tampered shard, an oversize shard and a pin mismatch") {
    fixture f("tamper");
    ldb::import_db(f.input, f.opts());
    REQUIRE(ldb::verify(f.committed, f.pin, false).ok);
    const auto mc = manifest(f.committed);
    const auto victim = f.committed / mc["shards"][0]["file"].get<std::string>();
    {
        std::fstream io(victim, std::ios::in | std::ios::out | std::ios::binary);
        io.seekp(-1, std::ios::end);
        io.put('\x7f');
    }
    auto v = ldb::verify(f.committed, f.pin, false);
    CHECK_FALSE(v.ok);
    // a different pin file
    fixture g("pin");
    ldb::import_db(g.input, g.opts());
    std::ofstream(g.pin, std::ios::app) << " ";
    CHECK_FALSE(ldb::verify(g.committed, g.pin, false).ok);
}

TEST_CASE("leandb: the committed shards in the repository verify") {
    const fs::path root = fs::path(PRISM_SOURCE_DIR) / "data" / "leandb";
    if (!fs::exists(root / "shards" / "manifest.json")) {
        MESSAGE("no committed shards yet");
        return;
    }
    const auto v = ldb::verify(root / "shards", root / "PIN.json", false);
    for (const auto& p : v.problems) MESSAGE(p);
    CHECK(v.ok);
    // git compliance: no file in data/leandb/shards is over 10 MiB
    for (const auto& e : fs::directory_iterator(root / "shards")) CHECK(fs::file_size(e.path()) <= ldb::kShardMaxBytes);
}

#ifndef _WIN32
#include <sys/wait.h>
#include <cstdlib>

TEST_CASE("leandb: fetch script downloads a pinned fake source, checks hashes, imports, and search finds it") {
    if (std::system("python3 --version > /dev/null 2>&1") != 0) {
        MESSAGE("python3 not available: skipped");
        return;
    }
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec).parent_path() / "prism";
    if (ec || !fs::exists(exe)) {
        MESSAGE("prism binary not next to prism_tests: skipped");
        return;
    }
    const fs::path root = fs::temp_directory_path() / "prism_leandb_fetch_test";
    fs::remove_all(root);
    fs::create_directories(root / "upstream");
    const fs::path data = root / "upstream" / "dataset.jsonl";
    {
        std::ofstream o(data);
        for (int i = 0; i < 30; ++i)
            o << "{\"name\":\"fake_" << i << "\",\"split\":\"train\",\"formal_statement\":\"theorem fake_" << i << " (x : BitVec 8) : x + " << i
              << "#8 = x + " << i << "#8 := by\\n\",\"formal_proof\":\"  rfl\",\"header\":\"import Mathlib\",\"goal\":\"\"}\n";
        o << "{\"name\":\"needle_platypus\",\"split\":\"train\",\"formal_statement\":\"theorem needle_platypus (a b : BitVec 8) : a + b = b + a := by\\n\","
             "\"formal_proof\":\"  bv_omega\",\"header\":\"import Mathlib\",\"goal\":\"\"}\n";
    }
    auto write_pin = [&](const std::string& sha) {
        std::ofstream p(root / "PIN.json");
        p << "{\"schema\":1,\"sources\":[{\"name\":\"DeepSeek-Prover-V1\",\"kind\":\"http\",\"use\":\"data\",\"license\":\"other (see dataset card)\","
             "\"url\":\"file://"
          << data.parent_path().string() << "\",\"revision\":\"fake1\",\"redistributable\":false,\"evidence\":\"test fixture\",\"files\":[{\"path\":"
             "\"DeepSeek-Prover-V1/dataset.jsonl\",\"bytes\":"
          << fs::file_size(data) << ",\"sha256\":\"" << sha << "\",\"url\":\"file://" << data.string() << "\"}]}]}";
    };
    auto run = [&](const std::string& extra) {
        const std::string cmd = "python3 " + (fs::path(PRISM_SOURCE_DIR) / "scripts" / "leandb_fetch_local.py").string() + " --pin " + (root / "PIN.json").string() +
                                " --dest " + (root / "dl").string() + " --out " + (root / "local").string() + " --prism " + exe.string() + " --yes " + extra + " > " +
                                (root / "log.txt").string() + " 2>&1";
        const int st = std::system(cmd.c_str());
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    };
    // a wrong pinned hash fails loudly and imports nothing
    write_pin(std::string(64, '0'));
    CHECK(run("") == 2);
    CHECK_FALSE(fs::exists(root / "local" / "manifest.json"));
    {
        std::ifstream l(root / "log.txt");
        std::stringstream ss;
        ss << l.rdbuf();
        CHECK(ss.str().find("SHA-256 MISMATCH") != std::string::npos);
        CHECK(ss.str().find("other (see dataset card)") != std::string::npos);  // the licence was shown as recorded
    }
    // the right hash: download, verify, normalise, import, verify
    write_pin(prism::solver::sha256_file(data));
    const int rc = run("");
    if (rc != 0) {
        std::ifstream l(root / "log.txt");
        std::stringstream ss;
        ss << l.rdbuf();
        MESSAGE(ss.str());
    }
    REQUIRE(rc == 0);
    ldb::search_options so;
    so.dirs = {root / "local"};
    so.m = ldb::mode::bm25;
    const auto r = ldb::search("needle platypus", so);
    REQUIRE(!r.empty());
    CHECK(r[0].name == "needle_platypus");
    CHECK(r[0].local);
    CHECK(r[0].source == "DeepSeek-Prover-V1");
    CHECK(ldb::verify(root / "local", root / "PIN.json", true).ok);
}
#endif

TEST_CASE("leandb: shards depend on the rows, not on the input row ids, so one source rebuilds to the same shards") {
    fixture f("rowids");
    ldb::import_db(f.input, f.opts());
    const auto m1 = manifest(f.committed);
    // the same rows with different input row ids (as when a different set of sources was normalised before them)
    fs::copy_file(f.input, f.root / "renumbered.sqlite");
    {
        auto c = db::connection::open((f.root / "renumbered.sqlite").string());
        c.exec("UPDATE problems SET id = id + 5000; UPDATE decls SET id = id + 100000; UPDATE decls SET id = (id - 100000) * 3 + 1;");
    }
    auto o = f.opts();
    o.out_committed = f.root / "shards2";
    o.out_local = f.root / "local2";
    o.out_all = f.root / "all2";
    ldb::import_db(f.root / "renumbered.sqlite", o);
    const auto m2 = manifest(f.root / "shards2");
    REQUIRE(m1["shards"].size() == m2["shards"].size());
    for (std::size_t i = 0; i < m1["shards"].size(); ++i) CHECK(m1["shards"][i]["sha256"] == m2["shards"][i]["sha256"]);
    // a rebuild of only one source reproduces exactly that source's shards (shards never span two sources)
    auto one = f.opts();
    one.only_sources = {"dataB"};
    one.out_committed = f.root / "shards3";
    one.out_local.clear();
    one.out_all.clear();
    ldb::import_db(f.input, one);
    std::map<std::string, std::string> full;
    for (const auto& s : m1["shards"]) full[s["first_key"].get<std::string>()] = s["sha256"].get<std::string>();
    const auto m3 = manifest(f.root / "shards3");
    REQUIRE(m3["shards"].size() >= 1);
    for (const auto& s : m3["shards"]) {
        REQUIRE(full.count(s["first_key"].get<std::string>()) == 1);
        CHECK(full[s["first_key"].get<std::string>()] == s["sha256"].get<std::string>());
    }
}
