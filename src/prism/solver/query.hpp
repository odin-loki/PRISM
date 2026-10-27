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
// and every 0-argument one by true/false. Z3 prints `(or x)` and `(and )`,
// which SMT-LIB and Bitwuzla reject ("expected at least 2 arguments"); the
// pir VCs have the 1-argument form. Z3's bvsmul_noovfl / bvsmul_noudfl /
// bvumul_noovfl (not SMT-LIB either) become the same predicate on the
// double-width product. Equivalence-preserving: only the SMT-LIB2 members
// (Bitwuzla, extra solvers) get it.
z3::expr portable_smt2(const z3::expr& f);
// Drop Z3 tactic commands from an SMT-LIB2 script. `(check-sat-using ...)`
// becomes `(check-sat)`; a top-level named tactic (`bit-blast`, `simplify`,
// `apply`, ...) is removed. SMT-LIB commands are left as they are. Z3's
// benchmark printer emits `(check-sat)`; this is what the Bitwuzla file
// passes through so a tactic cannot reach it.
std::string bitwuzla_dialect(std::string_view smt);
// The script written for Bitwuzla and any other SMT-LIB2 member: portable
// formula, Z3's benchmark printer, then bitwuzla_dialect. In-process Z3 does
// not read it.
std::string external_smt2(const z3::expr& f, std::string_view logic);
// bitblast() minus the translation: `c` must be a fresh context holding only f
// (translated from the caller's), so the CNF depends on the formula alone.
std::optional<Cnf> bitblast_fresh(z3::context& c, const z3::expr& f, std::string* why);
// SMT-LIB literal (#b.., #x.., (_ bvN W), true, false) of sort s, else nullopt.
std::optional<z3::expr> parse_literal(z3::context& c, const z3::sort& s, std::string_view v);

}  // namespace prism::solver::detail

#endif
