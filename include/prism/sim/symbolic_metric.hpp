#pragma once

// prism::sim: symbolic similarity, the way symbolic regression judges two
// expressions: by their shape and by what they compute.
//
//   sym_vec        = structural sparse vector (depth-weighted subtree-shape
//                    hashes, operator histogram, width / array-depth features)
//                    + semantic signature (the term evaluated on K fixed seeded
//                    assignments, outputs hashed into a bit vector).
//   symbolic_metric: d = alpha * (1 - weighted Jaccard of the sparse parts)
//                    + (1 - alpha) * (normalised Hamming of the semantic bits).
//
// d is a pseudo-metric in [0, 1]: d(x, x) = 0, symmetric. Alpha- and
// AC-equivalent formulas get the same sym_vec, so d = 0. Opt-in similarity
// search only; no verdict path, solver default or certified path uses it.
//
// forge::ann glue (codec, hnsw wiring) is in prism/sim/ann.hpp.
//
// Semantic signature, in detail (eval.cpp):
//  * Variables are matched by canonical order (first use in the canonical
//    term). Probe k gives the variable in slot j of its sort group the value
//    gen(seed, k, j, sort): the same on every formula, so two formulas that
//    compute the same function of their variables get the same bits.
//  * Structure can order variables of equivalent formulas differently (x - y
//    against (-y) + x). To make the signature invariant under that, the
//    signature is the lexicographic minimum over all permutations of the
//    slots inside each sort group, when the total number of permutations is
//    at most kMaxPerms (24, i.e. up to 4 variables of one sort). With more
//    variables the canonical order alone is used.
//  * Formulas with different variable counts or sorts are compared on the same
//    probe set: an unused variable changes nothing, a variable of another sort
//    gets a value of that sort's own generator, so the formulas then differ
//    wherever they actually depend on it. Mismatched sorts are never coerced.
//  * Probes: 24 "diagonal" probes (every variable the same corner value: 0, 1,
//    -1, MIN, MAX, 2, 4, 16, 0x55.., 0xAA.., ...), 64 probes with an
//    independent corner value per variable, the rest uniformly random.
//  * Semantics: Bool, QF_BV (SMT-LIB division by zero, shifts >= width), Int
//    as wrapping signed 128 bits (linear Int and Nat probe with small values),
//    arrays as base-hash plus store overrides. Anything else (UF, FP, Real,
//    bit-vectors wider than the evaluation word) is an opaque function of its
//    arguments' values: equal applications agree, nothing more is claimed.
//  * Output: 2 bits per probe (a Bool is replicated, any other value is
//    hashed). A signature that is the same on every probe ("constant on the
//    probe set", sym_vec::sem_constant) carries little information: for
//    verification conditions that are almost never true this is common, and
//    the structure then decides.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "prism/sim/term.hpp"

namespace prism::sim {

struct sim_options {
    unsigned depth = 3;      // deepest subtree-shape feature
    unsigned probes = 128;   // K; the signature has 2K bits
    std::uint64_t seed = 0x5eedc0deULL;
};
inline constexpr unsigned kMaxPerms = 24;

struct sym_vec {
    std::vector<std::pair<std::uint32_t, float>> feat;  // sorted by feature id, weights > 0
    std::vector<std::uint64_t> sem;                      // 2 bits per probe
    std::uint32_t nodes = 0;                             // distinct nodes of the canonical term
    std::uint32_t vars = 0;
    bool sem_constant = false;                           // same output on every probe
    bool operator==(const sym_vec&) const = default;
};

// The semantic signature of a canonical term (variable slots as described above).
std::vector<std::uint64_t> semantic_signature(const term& canonical, const sim_options& = {}, bool* constant = nullptr);
// The structural sparse vector of a canonical term.
std::vector<std::pair<std::uint32_t, float>> structural_features(const term& canonical, const sim_options& = {});
// canonicalise + structural + semantic.
sym_vec embed(const term& t, const sim_options& = {});
std::optional<sym_vec> embed_smt2(std::string_view text, std::string* why = nullptr, const sim_options& = {});
std::optional<sym_vec> embed_smt2_file(const std::string& path, std::string* why = nullptr, const sim_options& = {});

// 1 - sum(min) / sum(max) over the sparse vectors; 0 when both are empty.
double structural_distance(const sym_vec& a, const sym_vec& b);
// Hamming(a.sem, b.sem) / bits; vectors of different length are zero-padded.
double semantic_distance(const sym_vec& a, const sym_vec& b);

struct symbolic_metric {
    float alpha = 0.5f;  // weight of the structural part
    float distance(const sym_vec& a, const sym_vec& b) const {
        const double ds = structural_distance(a, b), dm = semantic_distance(a, b);
        return static_cast<float>(static_cast<double>(alpha) * ds + (1.0 - static_cast<double>(alpha)) * dm);
    }
};

// Byte codec (also behind forge::ann::codec<sym_vec> in sim/ann.hpp).
void encode_sym_vec(const sym_vec& v, std::vector<std::byte>& out);
sym_vec decode_sym_vec(const std::byte* data, std::size_t size);

}  // namespace prism::sim
