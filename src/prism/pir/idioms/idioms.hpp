#pragma once

// Loop idiom library (docs/TARGETED_PROVING.md W3, docs/PIR.md "Loop idioms").
//
// A loop is summarised by an abstract shape (its header phis and how the
// back edges update them, its exit conditions and its memory accesses, with
// names and most constants dropped); an idiom is a pattern over that shape
// with parameterised invariant templates. Matching a loop instantiates the
// templates with the loop's own variables, constants and bounds.
//
// Nothing here proves anything. The instantiated templates are candidates
// for Houdini (houdini.inc), which asks the solver for base, step and the
// final query exactly as for its generic family; an idiom can only make the
// search shorter (its candidates are tried first, alone) or possible (a
// template the generic family does not have).

#include "prism/pir.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace prism::pir::idioms {

// A natural loop: header block and body (blocks of fn, body[b] != 0).
struct LoopView {
    int header = -1;
    std::vector<char> body;
};

// The loops of fn by back edges (b -> h with h dominating b), one per
// header, bodies merged; the same loops as the encoder's analysis for the
// reducible CFGs clang emits.
std::vector<LoopView> find_loops(const Function& fn);

// Template terms and candidates: the same meaning as Houdini's InvTerm /
// InvCand (houdini.inc), converted there one to one.
struct TTerm {
    enum Kind { Phi, Scope, Const, Entry, Size } kind = Const;
    int idx = -1;  // Phi/Entry: phi index in the header; Scope: variable
    uint64_t c = 0;
    unsigned w = 0;
};

struct TCand {
    // Cmp:    a rel b
    // MaxLe:  a <= max(entry(a), b)                     (sext: signed)
    // Scale:  a - entry(a) rel k * (b - entry(b))       (k = mask; integers, sext: signed)
    // ScaleV: a - entry(a) == v * (b - entry(b))
    // DiffCmp (new): (a - b) rel v, in the phis' width (wrapping, as the program computes it)
    // ScaleVK (new): a - entry(a) rel k * v * (b - entry(b))   (integers; rel Eq/Sle/Sge)
    // ModLin (new):  ((a - b - v) & mask) == ((entry(a) - entry(b) - entry(v)) & mask)   (a, b, v phis)
    // MemInit:       forall x < ext(phi ip): p[x] initialised        (eb-byte elements)
    // MemConst:      forall x < ext(phi ip): p[x] == v
    // MemRingInit (new): forall x < mask with (x - tail) mod mask < count: p[x] initialised
    //                (tail = phi ip, count = phi ip2, zero-extended; mask = the ring's capacity)
    enum Kind { Cmp, MaxLe, Scale, ScaleV, DiffCmp, ScaleVK, ModLin, MemInit, MemConst, MemRingInit } kind = Cmp;
    int loop = -1;  // index into the loops given to instantiate()
    Op rel = Op::Eq;
    TTerm a, b, v;
    uint64_t mask = 0;
    bool sext = false;
    int p = -1;           // memory templates: the object (a scope pointer variable)
    int ip = -1, ip2 = -1;  // memory templates: index phi (and count phi)
    unsigned eb = 1;      // element bytes
    std::string idiom;  // the idiom that made it
};

// What the matcher needs from the caller (Houdini passes its own notion).
struct Context {
    // v is defined before loop L and readable at its header
    std::function<bool(int L, int v)> scope;
    // v is used as a signed value somewhere in fn
    std::function<bool(int v)> signed_use;
};

// Abstract shape of loop L (readable, stable text; names and constant values
// dropped except masks, unit steps and small trip counts).
std::string loop_shape(const Function& fn, const std::vector<LoopView>& loops, std::size_t L);

struct Match {
    std::string idiom;            // e.g. "masked-index"
    std::vector<TCand> cands;     // instantiated templates
};

// The idioms that match loop L, with their templates instantiated.
std::vector<Match> match_loop(const Function& fn, const std::vector<LoopView>& loops, std::size_t L,
                              const Context& ctx);

// The candidates of every matching idiom of every loop (deduplicated).
std::vector<TCand> instantiate(const Function& fn, const std::vector<LoopView>& loops, const Context& ctx,
                               std::vector<std::string>* matched = nullptr);

// Default Context computed from fn alone (dominators, signed uses), for the
// stage rows and the miner.
Context default_context(const Function& fn, const std::vector<LoopView>& loops);

// Per-loop JSON for a pir row (extra.loop_shapes):
// [{"header": block, "line": source line or 0, "shape": text, "idioms": [names]}]
std::string loop_shapes_json(const Function& fn);

// Names and one-line descriptions of the library's idioms (the table in
// docs and `prism-qa idioms`).
const std::vector<std::pair<std::string, std::string>>& catalogue();

}  // namespace prism::pir::idioms
