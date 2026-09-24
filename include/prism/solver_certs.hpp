#pragma once

// The certificate store of the solver cache (<cache>/certs/): the LRAT proof
// of every certified UNSAT answer, kept so a later certified request can
// re-check it (cake_lpr + Lean's checker) instead of solving again.
//
//  - proofs are stored packed (a lossless varint encoding of text LRAT,
//    typically 4-6x smaller) and unpacked into the request's private work
//    directory before a checker reads them: a checker never reads the store,
//    so pruning can never pull a proof from under a running check;
//  - the CNF is not stored: a re-check bit-blasts the formula again and
//    checks against that fresh CNF (its sha256 must equal the recorded one);
//  - the store has a size cap (SolveOptions::cert_cache_max_bytes, else
//    $PRISM_CERT_CACHE_MAX, else 2 GiB) and is pruned least-recently-used
//    (file mtime; a re-checked hit touches its proof); a proof larger than
//    the cap is not kept, and the entry then says certified=false;
//  - work directories of solver processes that were killed (prism-solve-*
//    in the temp dir whose pid is gone) are swept, since they can hold
//    multi-GB proofs.

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

namespace prism::solver::certs {

inline constexpr std::uint64_t kDefaultCapBytes = 2ull << 30;  // 2 GiB

// "2G", "512M", "64K", "1000" (bytes), "0" (keep no proofs). nullopt-like:
// returns false on a malformed value.
bool parse_size(const std::string& s, std::uint64_t& out);

// The cap in bytes: `opt_bytes` when >= 0, else $PRISM_CERT_CACHE_MAX when
// it parses, else kDefaultCapBytes. *why names the source (for notes).
std::uint64_t cap_bytes(std::int64_t opt_bytes, std::string* why = nullptr);

// Text LRAT -> packed file, and back. Both stream; false (and *why) on a
// line that is not LRAT or an I/O error. unpack writes canonical text LRAT
// ("id lits 0 hints 0" / "id d ids 0"), which is the same proof.
bool pack_lrat(const std::filesystem::path& text_in, const std::filesystem::path& packed_out, std::string* why);
bool unpack_lrat(const std::filesystem::path& packed_in, const std::filesystem::path& text_out, std::string* why);

// Path of a query's packed proof in the store.
std::filesystem::path proof_path(const std::filesystem::path& cache_root, const std::string& query_hash);

// Pack `lrat` into the store for `query_hash` (atomically: temp file +
// rename), then prune the store to the cap. false (and *why) when the proof
// was not kept (cap 0, larger than the cap, I/O error).
bool store(const std::filesystem::path& cache_root, const std::string& query_hash,
           const std::filesystem::path& lrat, std::uint64_t cap, std::string* why);

// Unpack the stored proof of `query_hash` into `text_out` and mark it used
// (mtime = now). false (and *why) when it is missing (pruned) or unreadable.
bool fetch(const std::filesystem::path& cache_root, const std::string& query_hash,
           const std::filesystem::path& text_out, std::string* why);

struct PruneStats {
    std::uint64_t bytes_before = 0, bytes_after = 0;
    std::size_t removed = 0;
};

// Delete least-recently-used files of <cache_root>/certs until the store is
// at most `cap` bytes (it prunes down to 80% of the cap, so a full store is
// not pruned on every write). Never deletes a proof in `keep` (query hashes)
// or one this process is reading (fetch in progress). Stale temp files of
// interrupted writes (older than an hour) and the stored CNFs / text proofs
// of the previous layout count and are pruned like proofs.
PruneStats prune(const std::filesystem::path& cache_root, std::uint64_t cap, const std::set<std::string>& keep = {});

// Remove prism-solve-* work directories under the temp dir whose process no
// longer exists. Returns the number removed. Never touches a directory of a
// live process.
std::size_t sweep_orphan_work_dirs();

}  // namespace prism::solver::certs
