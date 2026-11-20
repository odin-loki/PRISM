#pragma once

// Lean-goal adapter: a theorem statement in the decidable fragments (BitVec n,
// Nat and Int with + - * / %, Bool, Fin n) becomes a prism::sim::term, so a
// goal gets a sym_vec like any SMT formula and premise retrieval can merge
// symbolic-ANN hits with BM25 (prism leandb search --mode hybrid).
//
// Accepted: `theorem NAME binders : goal`, `lemma`, `example`, with binders
// `(x y : T)`, `{x : T}`, `[..]` (ignored), hypotheses `(h : prop)`, goals with
// `∀ x y : T, body` / `∀ (x : T), body`, and the operators
//   + - * / %  <<< >>> &&& ||| ^^^ ~~~  = == ≠ != < ≤ <= > ≥ >=  ∧ ∨ ¬ → ↔  && || !
//   if c then a else b, numerals (`5`, `0x1F`, `5#8`), true/false/True/False.
// Hypotheses become premises of an implication. Modelling notes:
//  * Nat is an Int variable without the x >= 0 side condition; truncated Nat
//    subtraction is ite(a < b, 0, a - b); Fin n is an Int variable (its bound
//    is not added). BitVec `<` and `≤` are unsigned, as in Lean.
//  * Anything else (Real, functions, structures, `x.toNat`, lists, ...) is not
//    translatable: nullopt with a reason, which the import counts.

#include <optional>
#include <string>
#include <string_view>

#include "prism/sim/term.hpp"

namespace prism::sim {

std::optional<term> lean_goal_to_term(std::string_view statement, std::string* why = nullptr);

}  // namespace prism::sim
