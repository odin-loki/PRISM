#pragma once

// Assume-guarantee stubs for callees without a body (--callee-contracts, docs/HARNESS_PHASE2.md).
//
// A call of a callee that has a contract (pir.hpp CalleeContract) is replaced by
//   1. the `requires` of the contract, asserted at the call site (a class FUNC-CONTRACT check: a violation
//      is a finding about the caller, not an assumption),
//   2. a havoc of exactly what `assigns` allows (arbitrary initialised bytes),
//   3. an arbitrary result, constrained by the `ensures` (which are ASSUMED).
// Everything the stub assumes is returned as one named assumption line (callee, source, status, the clauses),
// so a verdict that used a stub is PROVED-ASSUMING at best and says on what. A contract that cannot be
// applied (unknown syntax, types that do not match the call) is rejected with a reason and the call is
// handled as if there were no contract: never a silent assumption.

#include "prism/pir.hpp"
#include "translate_mem.hpp"

#include <optional>
#include <string>
#include <vector>

namespace prism::pir::pirmem {

struct StubCall {
    std::string callee;
    std::vector<Arg> args;
    std::vector<ir::Type> arg_types;
    ir::Type ret;
    int line = 0;
};

struct StubOutcome {
    bool applied = false;
    std::string why;                // not applied: the reason
    std::string assumption;         // applied: the named assumption (callee, source, status, clauses)
    std::optional<Arg> result;      // applied, non-void: the result value (type of the call)
};

// Emits the stub at block `cur`. On `applied == false` nothing was emitted.
StubOutcome apply_callee_contract(TrApi& t, MemTr& mt, const CalleeContract& c, const StubCall& call, int& cur);

// Syntax and signature check without emitting anything (tests, load-time diagnostics): empty = usable.
std::string callee_contract_problem(const CalleeContract& c);

}  // namespace prism::pir::pirmem
