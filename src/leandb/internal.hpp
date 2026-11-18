#pragma once

// Internal to src/leandb/: shard schema, text vectors.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prism/leandb.hpp"

namespace prism::leandb::detail {

// Shard schema (kSchemaVersion). Rows are written through forge::db::shard_writer, sorted by `key` (a BLOB that compares
// as memcmp). Search shards add `fts`, `indexed` and the per-shard HNSW tables `ann_text` / `ann_sym` (written by the
// writer's finalize hook); eval shards have `items`, `srcs`, `meta` only.
inline constexpr const char* kItemsSql =
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;"
    "CREATE TABLE srcs(id INTEGER PRIMARY KEY, name TEXT NOT NULL, use TEXT NOT NULL, license TEXT);"
    "CREATE TABLE items(key BLOB NOT NULL, id INTEGER PRIMARY KEY, src INTEGER NOT NULL, tbl INTEGER NOT NULL,"
    " kind TEXT, module TEXT, name TEXT, statement TEXT, proof TEXT, norm_hash BLOB, eval_overlap INTEGER NOT NULL,"
    " searchable INTEGER NOT NULL, simhash BLOB, symvec BLOB);";
inline constexpr const char* kIndexSql =
    // rows that are stored but never indexed (eval-overlapping, unverified provenance) are moved here by the finalize hook,
    // so `items` of a search shard holds exactly the indexed rows (an external-content FTS5 table counts `items`)
    "CREATE TABLE excluded(key BLOB NOT NULL, id INTEGER PRIMARY KEY, src INTEGER NOT NULL, tbl INTEGER NOT NULL,"
    " kind TEXT, module TEXT, name TEXT, statement TEXT, proof TEXT, norm_hash BLOB, eval_overlap INTEGER NOT NULL,"
    " searchable INTEGER NOT NULL, simhash BLOB, symvec BLOB);"
    "CREATE VIRTUAL TABLE fts USING fts5(name, statement, content='items', content_rowid='id', tokenize='unicode61 remove_diacritics 0');"
    "CREATE TABLE indexed(id INTEGER PRIMARY KEY);";

inline constexpr double kWeightName = 2.0, kWeightStatement = 1.0;  // bm25 column weights
inline constexpr std::size_t kEfSearch = 128;                        // forge::ann: recall@10 is only 0.86 at ef 64

const char* tbl_name(int t);
int tbl_code(std::string_view name);
int use_class(std::string_view use);

// Lower-cased alphanumeric words (non-ASCII bytes stay inside a word).
std::vector<std::string> words(std::string_view text);

// 256-bit SimHash of the words: the cheap text/identifier vector (Hamming ANN), as 4 words.
std::vector<std::uint64_t> simhash(std::string_view text);

}  // namespace prism::leandb::detail
