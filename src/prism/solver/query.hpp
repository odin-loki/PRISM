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
// bitblast() minus the translation: `c` must be a fresh context holding only f
// (translated from the caller's), so the CNF depends on the formula alone.
std::optional<Cnf> bitblast_fresh(z3::context& c, const z3::expr& f, std::string* why);
// SMT-LIB literal (#b.., #x.., (_ bvN W), true, false) of sort s, else nullopt.
std::optional<z3::expr> parse_literal(z3::context& c, const z3::sort& s, std::string_view v);

}  // namespace prism::solver::detail

#endif
