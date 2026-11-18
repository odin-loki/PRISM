#pragma once

// Internal to src/leandb/: shard schema, text vectors, the global-statistics BM25 function.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <forge/db/sqlite.hpp>

#include "prism/leandb.hpp"

namespace prism::leandb::detail {

// Shard schema (kSchemaVersion). `items` is sorted by sort_key (rowid order is NOT the sort order:
// ids are stable hashes). Search shards add `fts`, `indexed`; eval shards have neither.
inline constexpr const char* kItemsSql =
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;"
    "CREATE TABLE srcs(id INTEGER PRIMARY KEY, name TEXT NOT NULL, use TEXT NOT NULL, license TEXT);"
    "CREATE TABLE items(id INTEGER PRIMARY KEY, src INTEGER NOT NULL REFERENCES srcs(id), tbl INTEGER NOT NULL, orig_id INTEGER,"
    " kind TEXT, module TEXT, name TEXT, statement TEXT, proof TEXT, norm_hash BLOB, eval_overlap INTEGER NOT NULL,"
    " searchable INTEGER NOT NULL, simhash BLOB, symvec BLOB);";
inline constexpr const char* kIndexSql =
    "CREATE VIRTUAL TABLE fts USING fts5(name, statement, content='items', content_rowid='id', tokenize='unicode61 remove_diacritics 0');"
    "CREATE TABLE indexed(id INTEGER PRIMARY KEY);";

inline constexpr double kWeightName = 2.0, kWeightStatement = 1.0;  // bm25 column weights (build and search agree)

const char* tbl_name(int t);
int tbl_code(std::string_view name);
int use_class(std::string_view use);

// Lower-cased alphanumeric words (identifier parts split at '_' and '.').
std::vector<std::string> words(std::string_view text);

// 256-bit SimHash of the words: the cheap text/identifier vector (Hamming ANN).
std::array<std::uint64_t, 4> simhash(std::string_view text);

// ---- BM25 with global statistics (an exact reproduction of FTS5's bm25() over the union of shards) ----
struct bm25_stats {
    double n_docs = 0;
    double avg_len = 1;           // total tokens / docs over every shard
    std::vector<double> df;       // per query phrase: documents containing it, over every shard
};
// Registers `prism_bm25(fts, w_name, w_statement)` on the connection. It reads the statistics through
// set_bm25_stats() (thread-local: a connection is used by one thread at a time).
void register_bm25(forge::db::connection& c);
void set_bm25_stats(const bm25_stats* s);

}  // namespace prism::leandb::detail
