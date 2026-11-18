#pragma once

// PRISM's Lean database (docs/LEANDB.md): the Lean proof data OS-Proving collected,
// stored as sorted SQLite shards of at most 10 MiB (committed in git) plus
// local-only shards for sources whose licence does not allow redistribution,
// searched in parallel through forge::db (BM25 with global statistics,
// hashed-text ANN, symbolic ANN over translatable goals, reciprocal-rank fusion).
// Opt-in tooling: `prism leandb ...`. Nothing here is on a verdict path.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "prism/export.hpp"

namespace prism::leandb {

namespace fs = std::filesystem;

inline constexpr std::uint64_t kShardMaxBytes = 10ull * 1024 * 1024;  // hard limit per file (git rule)
inline constexpr int kSchemaVersion = 1;

// One searchable thing: a library declaration, a problem statement (with proof), or a traced theorem.
struct item {
    std::int64_t id = 0;    // stable: first 63 bits of sha256(source NUL tbl NUL orig_id)
    std::string source;
    std::string use;        // library | data | eval
    std::string tbl;        // decls | problems | trace_theorems
    std::int64_t orig_id = 0;
    std::string kind;       // theorem, def, instance, ... (decls); problem split; trace theorem
    std::string module;     // normalised module / namespace
    std::string name;
    std::string statement;
    std::string proof;
    std::string norm_hash;
    bool eval_overlap = false;  // near/exact duplicate of an eval problem: kept, never indexed
    bool searchable = true;     // false: not in any search index (eval use or eval_overlap)
};

// The sort key of the shards: (use class, table, module, name, id). Justification in docs/LEANDB.md.
std::string sort_key(const item& it);

struct pin_source {
    std::string name, kind, url, record, revision, use, license;
    bool redistributable = false;  // clearly allowed by licence evidence: may be committed
    std::string evidence;          // what the licence / dataset card says
};
struct pin {
    std::vector<pin_source> sources;
    std::string sha256;  // of the PIN.json text
    const pin_source* find(const std::string& name) const;
};
std::optional<pin> load_pin(const fs::path& file, std::string* why = nullptr);

struct import_options {
    fs::path pin_file;       // data/leandb/PIN.json
    // Outputs (each optional, each with its own manifest.json):
    fs::path out_committed;  // sources the pin marks redistributable      (data/leandb/shards)
    fs::path out_local;      // every other source: local-only, gitignored  (data/leandb/local)
    fs::path out_all;        // every source in one set (the private complete copy)
    std::vector<std::string> only_sources;  // empty: all sources
    std::uint64_t shard_bytes = kShardMaxBytes;  // shard_writer budget (it cuts at 90% and splits any file that comes out over)
    unsigned threads = 0;
    std::uint64_t max_items = 0;  // 0: all (tests)
};
struct import_report {
    std::uint64_t items = 0, searchable = 0, eval_items = 0, eval_overlap = 0, translatable = 0, goals_tried = 0;
    std::uint64_t committed_shards = 0, committed_bytes = 0, local_shards = 0, local_bytes = 0;
    std::string input_sha256;
    double seconds = 0;
};
// Reads OS-Proving's normalised lean_db.sqlite and writes the shards. Throws forge exceptions on I/O failure.
PRISM_API import_report import_db(const fs::path& lean_db_sqlite, const import_options& opt);

enum class mode { bm25, ann, sym, hybrid };
struct search_options {
    // Shard directories (each with a manifest.json). Every directory that exists is searched together,
    // with BM25 statistics taken over all of them.
    std::vector<fs::path> dirs;
    mode m = mode::hybrid;
    std::size_t k = 10;
    unsigned threads = 0;
};
struct result {
    std::int64_t id = 0;
    double score = 0;  // bm25: -bm25 (larger better); ann/sym: 1 - distance; hybrid: RRF score
    std::string source, tbl, kind, module, name, statement;
    bool local = false;
};
PRISM_API std::vector<result> search(const std::string& query, const search_options& opt);

// A reusable searcher: opens the merged shard set once and keeps the per-shard HNSW graphs loaded between queries
// (the batch evaluation and any long-running caller use this; search() builds a throw-away one).
class PRISM_API searcher {
public:
    explicit searcher(const search_options& opt);
    ~searcher();
    searcher(const searcher&) = delete;
    searcher& operator=(const searcher&) = delete;
    std::vector<result> search(const std::string& query);

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

struct verify_report {
    bool ok = true;
    std::vector<std::string> problems;
    std::uint64_t shards = 0, bytes = 0;
};
// Every shard <= 10 MiB, sha256 equal to the manifest, no eval row in any index or vector table, pin hash equals leandb.json's.
PRISM_API verify_report verify(const fs::path& dir, const fs::path& pin_file, bool is_local);

// `prism leandb import|search|verify|build-local|pin ...`; argv[0] is the subcommand.
PRISM_API int leandb_main(int argc, char** argv);

}  // namespace prism::leandb
