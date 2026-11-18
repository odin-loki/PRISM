#pragma once

// prism::sim term IR: a small hash-consed DAG over the SMT terms PRISM sees
// (Bool, QF_BV, linear Int, arrays, opaque applications). It is built from
// PRISM's own term type, z3::expr (the type solver/query.cpp and the pir
// encoder use), parsed by Z3's SMT-LIB reader; no second parser is written.
// Everything below the z3 front end (canonicalisation, evaluation, edit
// distance) is Z3-free. Opt-in similarity search only: nothing here is on a
// verdict path.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prism/wide_int.hpp"

#ifdef PRISM_HAS_Z3
#include <z3++.h>
#endif

namespace prism::sim {

// The evaluation word. 128 bits where the compiler has __int128, else 64
// (MSVC build): wider bit-vectors are then evaluated as opaque terms.
#if PRISM_HAS_INT128
using word = unsigned __int128;
using sword = __int128;
inline constexpr unsigned kMaxWidth = 128;
#else
using word = std::uint64_t;
using sword = std::int64_t;
inline constexpr unsigned kMaxWidth = 64;
#endif

enum class sk : std::uint8_t { boolean, bv, integer, array, other };

struct sort_t {
    sk kind = sk::other;
    std::uint32_t width = 0;      // bv width; array: innermost element width (0 when not a bv)
    std::uint32_t idx_width = 0;  // array: index width
    std::uint8_t depth = 0;       // array nesting depth (0 for scalars)
    bool operator==(const sort_t&) const = default;
};

#define PRISM_SIM_OPS(X)                                                                                     \
    X(var) X(lit) X(tru) X(fls) X(ilit) X(not_) X(and_) X(or_) X(xor_) X(implies) X(ite) X(eq) X(distinct) \
    X(bvnot) X(bvneg) X(bvand) X(bvor) X(bvxor) X(bvnand) X(bvnor) X(bvxnor) X(bvadd) X(bvsub) X(bvmul)     \
    X(bvudiv) X(bvurem) X(bvsdiv) X(bvsrem) X(bvsmod) X(bvshl) X(bvlshr) X(bvashr) X(concat) X(extract)    \
    X(zext) X(sext) X(repeat) X(rotl) X(rotr) X(bvult) X(bvule) X(bvugt) X(bvuge) X(bvslt) X(bvsle)        \
    X(bvsgt) X(bvsge) X(bvcomp) X(iadd) X(isub) X(imul) X(ineg) X(ile) X(ilt) X(ige) X(igt) X(idiv)        \
    X(imod) X(select) X(store) X(constarr) X(opaque)

enum class op : std::uint8_t {
#define X(n) n,
    PRISM_SIM_OPS(X)
#undef X
        count_
};
std::string_view op_name(op o);

struct node {
    op o = op::opaque;
    sort_t s;
    std::uint32_t aux = 0;   // var: ordinal; extract: hi; zext/sext/repeat/rotl/rotr: amount
    std::uint32_t aux2 = 0;  // extract: lo
    word val = 0;            // lit / ilit (ilit: two's complement)
    std::uint64_t tag = 0;   // opaque: hash of the declaration name
    std::vector<std::uint32_t> kids;
};

// Nodes are in dependency order (every child index is smaller than its
// parent's), the root is the last node, so one forward pass evaluates it.
struct term {
    std::vector<node> nodes;
    std::uint32_t root = 0;
    std::uint32_t n_vars = 0;  // distinct variables (ordinals 0..n_vars-1 after canonicalise)
    bool canonical = false;
    std::size_t size() const { return nodes.size(); }
};

// ---- front end (PRISM_HAS_Z3) ----
#ifdef PRISM_HAS_Z3
// nullopt (with why) when the term is deeper than max_depth or empty.
std::optional<term> from_z3(const z3::expr& e, std::string* why = nullptr, unsigned max_depth = 20000);
#endif
// SMT-LIB2 text (declare-fun/assert/...): the conjunction of its assertions.
// nullopt, with why, if it does not parse or Z3 is not compiled in.
std::optional<term> from_smt2(std::string_view text, std::string* why = nullptr);
std::optional<term> from_smt2_file(const std::string& path, std::string* why = nullptr);

// ---- canonical form (canon.cpp) ----
// 1. AC operators (bvadd bvmul bvand bvor bvxor and or xor) are flattened and
//    their operands ordered by a variable-name-free structural hash; = and
//    distinct (and bvnand/bvnor/bvxnor/bvcomp/iadd/imul pairs) are ordered
//    only. Ties (operands that differ only by which variables they use) keep
//    their input order, so the form is sound but not complete: it is graph
//    canonicalisation, not an isomorphism test.
// 2. Variables are renamed by first use in a depth-first walk of the ordered
//    term (node.aux = ordinal).
// 3. Literal handling: values stay in node.val (the semantic signature needs
//    them); abstract_lit() says which ones the structural vector sees.
term canonicalise(const term& t);
// true: 0, 1, -1, a power of two, MIN or MAX of the sort (kept); false: the
// literal is abstracted to its sort in the structural vector.
bool keeps_literal(const node& n);

// ---- structural hashes (canon.cpp) ----
std::uint64_t mix64(std::uint64_t x);
std::uint64_t hash_combine(std::uint64_t h, std::uint64_t x);
// Hash of one node's own label (operator, sort, kept literal, parameters), children excluded.
std::uint64_t node_label_hash(const node& n);
// Variable-name-free hash of the subtree under every node (literals abstracted).
std::vector<std::uint64_t> shape_hashes(const term& t);

// ---- tree edit distance (ted.cpp) ----
// Zhang-Shasha ordered-tree edit distance between the tree expansions of two
// canonical terms (unit costs, node label = operator + sort + kept literal),
// normalised by max(size). nullopt when an expansion exceeds max_nodes.
std::optional<double> tree_edit_distance(const term& a, const term& b, std::size_t max_nodes = 160);

}  // namespace prism::sim
