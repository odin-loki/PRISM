#pragma once

// Call-site discharge of the automatic pointer harness, within a unit and over a whole build
// (docs/HARNESS_GENERATOR.md section 2.2 and "Whole-build call-site discharge"; docs/PIR.md).
//
// The harness of a function g is the precondition P_g of its proof. A call site of g discharges P_g when the
// checks of class HARNESS-OBLIGATION hold there (translate.cpp call_obligations), checked under the caller's own
// context. P_g holds at every invocation of g when (a) every caller of g is known (the closed world: static and
// not address-taken, or, over a whole build, an external symbol only its link unit can call), (b) every call site
// is PROVED, and (c) the context each site was checked in is itself unconditional: either the caller has no
// automatic harness, or its own harness is discharged the same way (grounded). What is left of g's assumptions
// is then its own non-harness assumptions plus the assumptions the call-site checks rested on. When that is
// nothing, g is PROVED (proofs/Prism/Discharge.lean `grounded_safe`).

#include "prism/export.hpp"
#include "prism/pir.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace prism::pir::discharge {

// An assumption line that belongs to an automatic pointer harness (removed when the harness is discharged).
PRISM_API bool is_harness_line(const std::string& a);
// The line a discharged result states when the proofs it composes used objects PRISM assumes into existence.
PRISM_API std::string invented_objects_line();

PRISM_API nlohmann::json obligation_json(const ObligationNode& o);
// nullptr on a malformed node
PRISM_API std::shared_ptr<ObligationNode> obligation_from_json(const nlohmann::json& j);

// The call-graph facts of one unit's IR.
struct UnitGraph {
    std::set<std::string> defined;     // definitions (library models excluded)
    std::set<std::string> internal;    // internal / private definitions (C `static`)
    std::set<std::string> weak;        // weak, linkonce, common, available_externally definitions
    std::set<std::string> declared;    // declared, not defined
    std::set<std::string> taken;       // symbols used other than as the callee of a direct call (address taken)
    std::set<std::string> asm_tokens;  // identifiers in module asm and inline asm strings
    std::map<std::string, std::map<std::string, int>> calls;  // caller -> callee -> direct call instructions
    std::map<std::string, std::set<std::string>> callers;     // callee -> callers (definitions, models excluded)
    std::map<std::string, int> ptr_params;                     // definition -> pointer parameters
    struct Indirect {
        std::string caller;
        int line = 0;
        std::string kind;    // "field" (load of a struct field), "vtable", "pointer"
        std::string detail;  // "%struct.T.K" for a field
        std::vector<std::string> targets;  // the ops-table targets of the field, when the table names them
        int candidates = 0;  // address-taken functions of the unit (the translator's candidate set without a table)
    };
    std::vector<Indirect> indirect;
};
PRISM_API UnitGraph unit_graph(const ir::Module& m, const std::string& ir_text, const std::vector<OpsField>& ops);
// record "callgraph" of --pir-discharge-out
PRISM_API nlohmann::json unit_graph_json(const UnitGraph& g, const std::string& file, const std::string& unit);

// One (caller, callee) pair: the obligations of the callee checked in one translation of the caller.
struct Site {
    std::string caller, callee;
    int sites = 0;                         // direct call instructions of the callee in the caller
    std::string verdict, message;
    int line = 0;
    std::vector<std::string> assumptions;  // of the caller's translation (its context)
    std::vector<std::string> caller_harness;  // those that are the caller's own automatic harness
    bool invented = false;                 // the caller's translation has objects PRISM assumed
};
PRISM_API bool site_proved(const Site& s);

struct Node {
    std::string display;                // source name for messages
    bool closed = false;                // every caller is known (closed world)
    std::string why_not;                // why not, or why the harness has no plan
    std::vector<std::string> callers;   // keys
};

struct Grounding {
    bool ok = false;
    std::string reason;                    // why not
    std::vector<std::string> inherited;    // assumptions of the callers' contexts (deduplicated, in order)
    bool invented = false;
    int sites = 0;                         // call sites of the function itself
    std::vector<std::string> callers;      // display names of its callers
};

// The least fixed point of "P_x holds at every invocation of x" over a call graph. ground(x) is ok when x is
// closed, every caller's site is PROVED, and every caller whose site rested on the caller's own harness is itself
// grounded. A cycle is never grounded (recursion: the precondition would have to be an inductive invariant).
class Grounder {
public:
    std::map<std::string, Node> nodes;
    std::map<std::pair<std::string, std::string>, Site> sites;  // (caller, callee)
    Grounding ground(const std::string& key);
    // level 1 (the in-unit discharge of section 2.2): closed and every site PROVED, contexts not looked at.
    // Empty when discharged, else the first failure.
    std::string sites_failure(const std::string& key) const;

private:
    std::map<std::string, Grounding> memo_;
    std::set<std::string> active_;
};

// What a discharged result becomes. `own`: the function's assumptions; `residual`: harness parts the obligation
// does not check (initialisation of objects over the cap); `before`: the verdict before the assumptions made it
// PROVED-ASSUMING (extra.verdict_before_assumptions). `grounded`: g (ok) or nullptr for a level-1 discharge.
struct Outcome {
    std::string status;
    std::vector<std::string> assumptions;
    bool proved = false;   // status is PROVED / PROVED-UNBOUNDED: nothing is assumed
};
PRISM_API Outcome outcome(const std::string& before, const std::vector<std::string>& own,
                          const std::vector<std::string>& residual, bool own_invented, const Grounding& g,
                          const std::string& discharged_line);

// ---------------------------------------------------------------------------------------------------------------
// Whole build (prism-qa callsite-discharge)
// ---------------------------------------------------------------------------------------------------------------

struct WholeBuildOptions {
    std::vector<std::filesystem::path> inputs;  // --pir-discharge-out files (or directories of *.jsonl)
    // compile_commands.json files of the configuration (several are merged: a C database and one of the assembler
    // objects, say). An entry may carry "link_units": [...] (the build's own link map: "vmlinux", "module:NAME"),
    // which replaces the link units read from its flags.
    std::vector<std::filesystem::path> compile_dbs;
    // the databases list every object of every link unit (assembler and Rust included). Without it no external
    // function is closed: a database filtered to C cannot show what else is linked into a module.
    bool complete_db = false;
    // "linux": an external symbol that is not exported is called only from its own link unit (vmlinux, or one
    // module: -DMODULE / KBUILD_MODNAME of the compile command), so its callers in that link unit are all its
    // callers. "none" (default): any external symbol may be called from outside the build; only static
    // functions are closed. EXPORT_SYMBOL makes the address taken (__ADDRESSABLE) and names the symbol in asm, so an
    // exported function is never closed.
    std::string link_rule = "none";
    std::string config;  // configuration name, for the records
};

struct WholeBuildResult {
    std::vector<nlohmann::json> decisions;  // one per candidate (record "callsite-discharge")
    std::vector<nlohmann::json> todo;       // per unit: plans to check there, or call-graph facts to collect
    std::vector<nlohmann::json> edges;      // the call graph (direct edges, indirect sites)
    nlohmann::json summary;
    std::vector<std::string> errors;
};
PRISM_API WholeBuildResult whole_build(const WholeBuildOptions& opt);

}  // namespace prism::pir::discharge
