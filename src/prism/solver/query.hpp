#pragma once

// Internal to src/prism/solver/: Z3 term helpers shared by query.cpp and portfolio.cpp.

#ifdef PRISM_HAS_Z3

#include <z3++.h>

#include "prism/solver.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::solver::detail {

std::vector<z3::expr> collect_consts(const z3::expr& f);
std::string const_name(const z3::expr& e);
// The same formula with every 1-argument `and`/`or` replaced by its argument
// (0-argument ones by true/false). Z3 prints `(or x)`, which SMT-LIB and
// Bitwuzla reject ("expected at least 2 arguments"); the pir VCs have them.
// Z3's bvsmul_noovfl / bvsmul_noudfl / bvumul_noovfl (not SMT-LIB either)
// become the same predicate on the double-width product. Equivalence-
// preserving: only the SMT-LIB2 members (Bitwuzla, extra solvers) get it.
z3::expr portable_smt2(const z3::expr& f);
// bitblast() minus the translation: `c` must be a fresh context holding only f
// (translated from the caller's), so the CNF depends on the formula alone.
std::optional<Cnf> bitblast_fresh(z3::context& c, const z3::expr& f, std::string* why);
// SMT-LIB literal (#b.., #x.., (_ bvN W), true, false) of sort s, else nullopt.
std::optional<z3::expr> parse_literal(z3::context& c, const z3::sort& s, std::string_view v);

}  // namespace prism::solver::detail

#endif
