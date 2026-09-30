// Doctests: the ltl stage (switch(state) machine extraction, the safety
// fragment it decides, and the GF / FG / U / F safety approximations that are
// BOUNDED, never PROVED). Linked into prism_tests next to test_main.cpp.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace laws = prism::laws;
using Trans = std::pair<std::string, std::string>;

fs::path td_root() {
    return fs::path(PRISM_SOURCE_DIR) / "testdata";
}

prism::FunctionInfo plant(const char* name) {
    for (const char* file : {"fsm.c", "fsm_recur.c"}) {
        auto p = td_root() / file;
        for (auto& f : prism::extract_functions(p, p.string()))
            if (f.name == name) return f;
    }
    FAIL("missing plant function " << name);
    return {};
}

prism::LtlFsm fsm_of(const char* name) {
    auto fsm = prism::extract_ltl_fsm(plant(name).body);
    REQUIRE(fsm.has_value());
    return *fsm;
}

std::set<Trans> trans_set(const prism::LtlFsm& fsm) {
    return {fsm.transitions.begin(), fsm.transitions.end()};
}

std::string xget(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

prism::Finding check(const std::string& formula, const prism::LtlFsm& fsm) {
    auto f = prism::check_ltl_safety(formula, fsm);
    REQUIRE(f.has_value());
    return *f;
}

struct TmpDir {
    fs::path dir;
    explicit TmpDir(const char* tag) {
        static int seq = 0;
        dir = fs::temp_directory_path() / (std::string("prism_ltl_") + tag + "_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path put(const char* name, const std::string& text) const {
        auto p = dir / name;
        std::ofstream(p) << text;
        return p;
    }
};

std::vector<prism::Finding> run_spec(const prism::FunctionInfo& fn, const std::string& text,
                                     const prism::Config& cfg = prism::Config{}) {
    TmpDir t("spec");
    auto spec = t.put("spec.ltl", text);
    return prism::run_ltl({fn}, {spec}, cfg);
}

void require_not_proof(const prism::Finding& f) {
    CHECK(f.status != std::string(laws::PROVED));
    CHECK(f.status != std::string(laws::PROVED_UNBOUNDED));
    CHECK(f.status != std::string(laws::CLEAN));
    CHECK_FALSE(laws::is_proof(f.status));
}

}  // namespace

// ---- machine extraction ----

TEST_CASE("ltl fsm: plants extract, switch(ev) does not") {
    fsm_of("fsm_recur");
    fsm_of("fsm_step");
    fsm_of("fsm_settle");
    // switch (ev) is not the plant even though the body assigns state.
    CHECK_FALSE(prism::extract_ltl_fsm(plant("fsm_ev_only").body).has_value());
}

TEST_CASE("ltl fsm: switch on p->state and obj.state is the plant, braces bound it") {
    auto arrow = prism::extract_ltl_fsm(
        "switch (p->state) { case A: p->state = B; break; case B: p->state = A; break; }");
    REQUIRE(arrow.has_value());
    CHECK(trans_set(*arrow) == std::set<Trans>{{"A", "B"}, {"B", "A"}});
    auto dot = prism::extract_ltl_fsm(
        "switch (m.state) { case A: m.state = B; break; case B: break; }");
    REQUIRE(dot.has_value());
    CHECK(trans_set(*dot) == std::set<Trans>{{"A", "B"}, {"B", "B"}});
    // A later switch (ev) outside the switch (state) braces adds no cases.
    auto scoped = prism::extract_ltl_fsm(
        "switch (state) { case A: state = B; break; case B: state = A; break; }\n"
        "switch (ev) { case X: log_ev(ev); break; }");
    REQUIRE(scoped.has_value());
    CHECK(std::find(scoped->cases.begin(), scoped->cases.end(), "X") == scoped->cases.end());
    for (auto& [s, d] : scoped->transitions) CHECK(s != "X");
    // If that switch (ev) writes state, the step does more than the
    // switch (state) shows: no machine rather than a wrong one.
    CHECK_FALSE(prism::extract_ltl_fsm(
                    "switch (state) { case A: state = B; break; case B: state = A; break; }\n"
                    "switch (ev) { case X: state = C; break; }")
                    .has_value());
    // No brace after switch (state) (a `;` first): nothing to extract.
    CHECK_FALSE(prism::extract_ltl_fsm("switch (state); { case A: state = B; }").has_value());
    // One state only is not a machine.
    CHECK_FALSE(prism::extract_ltl_fsm("switch (state) { case A: break; }").has_value());
}

TEST_CASE("ltl fsm: stacked case labels share destinations, no spurious self-loop") {
    auto fsm = fsm_of("fsm_fall");
    auto t = trans_set(fsm);
    CHECK(t.contains({"FT_REQ", "FT_ACK"}));
    CHECK(t.contains({"FT_RETRY", "FT_ACK"}));
    CHECK_FALSE(t.contains({"FT_REQ", "FT_REQ"}));
    auto f = check("G (state == FT_REQ -> X (state == FT_ACK))", fsm);
    CHECK(f.status == std::string(laws::PROVED));
    // False twin: REQ does not go back to REQ, so the X claim is refuted.
    auto bad = check("G (state == FT_REQ -> X (state == FT_REQ))", fsm);
    CHECK(bad.status == std::string(laws::FAILED));
}

TEST_CASE("ltl fsm: fall-through without break collects the next arm's dests") {
    auto fsm = prism::extract_ltl_fsm(
        "switch (state) { case A: if (ev) state = B; case B: state = C; break; case C: return 0; }");
    REQUIRE(fsm.has_value());
    auto t = trans_set(*fsm);
    CHECK(t.contains({"A", "B"}));
    CHECK(t.contains({"A", "C"}));
    // Both ev paths reach `state = C` in the next arm: A never stays A.
    CHECK_FALSE(t.contains({"A", "A"}));
    CHECK(t.contains({"B", "C"}));
    CHECK_FALSE(t.contains({"B", "B"}));
    CHECK(t.contains({"C", "C"}));
}

TEST_CASE("ltl fsm: parenthesised destination is a transition, not a self-loop") {
    auto fsm = fsm_of("fsm_paren");
    auto t = trans_set(fsm);
    CHECK(t.contains({"LIVE_IDLE", "LIVE_ACK"}));
    CHECK(t.contains({"LIVE_ACK", "LIVE_IDLE"}));
    CHECK_FALSE(t.contains({"LIVE_IDLE", "LIVE_IDLE"}));
    auto f = check("G (state == LIVE_IDLE -> X (state == LIVE_ACK))", fsm);
    CHECK(f.status == std::string(laws::PROVED));
    auto bad = check("G (state == LIVE_IDLE -> X (state == LIVE_IDLE))", fsm);
    CHECK(bad.status == std::string(laws::FAILED));
}

TEST_CASE("ltl fsm: comparisons are not destinations; compound assignment refuses the machine") {
    auto fsm = prism::extract_ltl_fsm(
        "switch (state) { case A: if (state == C) {} state = B; break; case B: if (state != A) {} break; }");
    REQUIRE(fsm.has_value());
    CHECK(std::find(fsm->states.begin(), fsm->states.end(), "C") == fsm->states.end());
    auto t = trans_set(*fsm);
    CHECK(t.contains({"A", "B"}));
    CHECK(t.contains({"B", "B"}));
    // `state += 1` / `state++` move to a state the reader cannot name: a
    // B->B self-loop would be a machine the code does not have.
    CHECK_FALSE(prism::extract_ltl_fsm(
                    "switch (state) { case A: state = B; break; case B: state += 1; break; }")
                    .has_value());
    CHECK_FALSE(prism::extract_ltl_fsm(
                    "switch (state) { case A: state = B; break; case B: state++; break; }")
                    .has_value());
}

TEST_CASE("ltl fsm: one named case plus default is a machine; default feeds unmatched states") {
    auto fsm = fsm_of("fsm_default");
    auto t = trans_set(fsm);
    CHECK(t.contains({"LIVE_IDLE", "LIVE_ACK"}));
    CHECK(t.contains({"LIVE_ACK", "LIVE_IDLE"}));
    CHECK_FALSE(t.contains({"LIVE_ACK", "LIVE_ACK"}));
    auto f = check("G (state == LIVE_ACK -> X (state == LIVE_IDLE))", fsm);
    CHECK(f.status == std::string(laws::PROVED));
    auto bad = check("G (state == LIVE_ACK -> X (state == LIVE_ACK))", fsm);
    CHECK(bad.status == std::string(laws::FAILED));
}

TEST_CASE("ltl fsm: a nested switch (ev) in an arm keeps the state; its labels are not states") {
    const char* src =
        "switch (state) {\n"
        "case IDLE:\n"
        "    switch (ev) { case GO: state = RUN; break; case STOP: break; }\n"
        "    break;\n"
        "case RUN: state = IDLE; break;\n"
        "}";
    auto fsm = prism::extract_ltl_fsm(src);
    REQUIRE(fsm.has_value());
    auto t = trans_set(*fsm);
    CHECK(t.contains({"IDLE", "RUN"}));
    CHECK(t.contains({"IDLE", "IDLE"}));  // STOP, or an unmatched ev
    CHECK(t.contains({"RUN", "IDLE"}));
    for (const char* bogus : {"GO", "STOP"})
        CHECK(std::find(fsm->states.begin(), fsm->states.end(), bogus) == fsm->states.end());
    // The X claim is false: IDLE can stay IDLE.
    auto bad = check("G (state == IDLE -> X (state == RUN))", *fsm);
    CHECK(bad.status == std::string(laws::FAILED));
    // Its true twin still proves.
    auto ok = check("G (state == IDLE -> X (state == RUN || state == IDLE))", *fsm);
    CHECK(ok.status == std::string(laws::PROVED));
    // A loop body may run zero times: its write is not certain.
    auto loop = prism::extract_ltl_fsm(
        "switch (state) { case A: while (more()) { state = B; } break; case B: state = A; break; }");
    REQUIRE(loop.has_value());
    CHECK(trans_set(*loop).contains({"A", "A"}));
}

TEST_CASE("ltl fsm: a default with `if` and no `else` keeps unmatched states") {
    auto fsm = prism::extract_ltl_fsm(
        "switch (state) { case A: state = B; break; default: if (go) state = A; break; }");
    REQUIRE(fsm.has_value());
    auto t = trans_set(*fsm);
    CHECK(t.contains({"A", "B"}));
    CHECK(t.contains({"B", "A"}));
    CHECK(t.contains({"B", "B"}));
    CHECK(check("G (state == B -> X (state == A))", *fsm).status == std::string(laws::FAILED));
    // True twin: an unconditional default write does not keep the state.
    auto sure = prism::extract_ltl_fsm(
        "switch (state) { case A: state = B; break; default: state = A; break; }");
    REQUIRE(sure.has_value());
    CHECK_FALSE(trans_set(*sure).contains({"B", "B"}));
    CHECK(check("G (state == B -> X (state == A))", *sure).status == std::string(laws::PROVED));
}

TEST_CASE("ltl fsm: only an unconditional top-level stop ends an arm") {
    // A conditional break leaves A in A; the fall-through still reaches C.
    auto cond = prism::extract_ltl_fsm(
        "switch (state) { case A: if (x) break; state = B; case B: state = C; break; case C: break; }");
    REQUIRE(cond.has_value());
    auto t = trans_set(*cond);
    CHECK(t.contains({"A", "A"}));
    CHECK(t.contains({"A", "C"}));
    CHECK(check("G (state == A -> X (state == C))", *cond).status == std::string(laws::FAILED));
    // A break inside a nested loop does not end the arm: A falls into B.
    auto loop = prism::extract_ltl_fsm(
        "switch (state) { case A: state = B; for (;;) { break; } case B: state = C; break; case C: break; }");
    REQUIRE(loop.has_value());
    auto lt = trans_set(*loop);
    CHECK(lt.contains({"A", "C"}));
    CHECK_FALSE(lt.contains({"A", "A"}));
    // A conditional return before the write keeps the state.
    auto ret = prism::extract_ltl_fsm(
        "switch (state) { case A: if (err) { return -1; } state = B; break; case B: state = A; break; }");
    REQUIRE(ret.has_value());
    CHECK(trans_set(*ret).contains({"A", "A"}));
    CHECK(check("G (state == A -> X (state == B))", *ret).status == std::string(laws::FAILED));
    // if / else that both write: no self-loop, and the X claim proves.
    auto both = prism::extract_ltl_fsm(
        "switch (state) { case A: if (x) state = B; else state = C; break; case B: state = A; break;"
        " case C: state = A; break; }");
    REQUIRE(both.has_value());
    CHECK_FALSE(trans_set(*both).contains({"A", "A"}));
    CHECK(check("G (state == A -> X (state == B || state == C))", *both).status ==
          std::string(laws::PROVED));
}

TEST_CASE("ltl fsm: a write the reader cannot name refuses the machine (NOTRUN, not a proof)") {
    for (const char* src : {
             "switch (state) { case A: state = (c) ? C : D; break; case B: state = A; break; }",
             "switch (state) { case A: state = next(s); break; case B: state = A; break; }",
             "switch (state) { case A: state = B + 1; break; case B: state = A; break; }",
             // A write outside the switch is a step the switch does not show.
             "if (err) state = BAD;\n"
             "switch (state) { case A: state = B; break; case B: state = A; break; }",
             "switch (state) { case A: state = B; break; case B: state = A; break; }\nstate = A;",
             // A qualified case label is not a plain name.
             "switch (state) { case S::A: state = B; break; case B: state = A; break; }",
         })
        CHECK_MESSAGE(!prism::extract_ltl_fsm(src).has_value(), src);
    // An initialiser is the start state, not a step.
    auto init = prism::extract_ltl_fsm(
        "int state = A;\nswitch (state) { case A: state = B; break; case B: state = A; break; }");
    REQUIRE(init.has_value());
    CHECK(trans_set(*init) == std::set<Trans>{{"A", "B"}, {"B", "A"}});
    // The same machine through run_ltl: a refused machine is NOTRUN.
    prism::FunctionInfo fn;
    fn.name = "tern";
    fn.body = "switch (state) { case A: state = (c) ? C : D; break; case B: state = A; break; }";
    auto recs = run_spec(fn, "G (state == A -> X (state == B))\n");
    REQUIRE_FALSE(recs.empty());
    for (auto& r : recs) require_not_proof(r);
}

TEST_CASE("ltl fsm: labels and writes inside comments and strings are not code") {
    auto fsm = prism::extract_ltl_fsm(
        "switch (state) {\n"
        "case A: /* case Z: state = Z; */ state = B; break; // state = Y;\n"
        "case B: puts(\"state = W; case V:\"); state = A; break;\n"
        "}");
    REQUIRE(fsm.has_value());
    CHECK(trans_set(*fsm) == std::set<Trans>{{"A", "B"}, {"B", "A"}});
}

// ---- safety fragment on fsm_step (ST_IDLE / ST_WORK / ST_BAD) ----

TEST_CASE("ltl G p: protocol.ltl assigns an error state and is FAILED with the machine") {
    auto recs = prism::run_ltl({plant("fsm_step")}, {td_root() / "protocol.ltl"});
    std::vector<prism::Finding> failed;
    for (auto& r : recs)
        if (r.status == laws::FAILED) failed.push_back(r);
    REQUIRE_FALSE(failed.empty());
    CHECK(failed[0].cls == "LTL-SAFETY");
    CHECK(failed[0].function == std::optional<std::string>("fsm_step"));
    CHECK(has(failed[0].message, "error state"));
    auto shape = xget(failed[0], "fsm");
    CHECK(has(shape, "\"states\""));
    CHECK(has(shape, "\"cases\""));
    CHECK(has(shape, "\"assigns\""));
    CHECK(has(shape, "ST_BAD"));
}

TEST_CASE("ltl G (p -> X q) holds and fails on fsm_step") {
    auto fsm = fsm_of("fsm_step");
    CHECK(check("G (state == ST_IDLE -> X (state == ST_IDLE || state == ST_WORK))", fsm).status ==
          std::string(laws::PROVED));
    auto bad = check("G (state == ST_IDLE -> X (state == ST_WORK))", fsm);
    CHECK(bad.status == std::string(laws::FAILED));
    CHECK(has(bad.message, "ST_IDLE->ST_IDLE"));
    CHECK(has(xget(bad, "violations"), "ST_IDLE"));
}

TEST_CASE("ltl G (p -> X q) violation stays FAILED, never HYPOTHESIS") {
    prism::LtlFsm fsm{{"S_REQ", "S_BAD", "S_OK"}, {"S_REQ"}, {}, {{"S_REQ", "S_BAD"}}};
    auto f = check("G (state == S_REQ -> X (state == S_OK))", fsm);
    CHECK(f.status == std::string(laws::FAILED));
    CHECK(f.status != std::string(laws::HYPOTHESIS));
}

TEST_CASE("ltl G (p -> X q) missing transition is a HYPOTHESIS synthesis") {
    prism::LtlFsm fsm{{"WAIT", "DONE"}, {"WAIT"}, {}, {}};
    auto f = check("G (state == WAIT -> X (state == DONE))", fsm);
    CHECK(f.status == std::string(laws::HYPOTHESIS));
    CHECK(f.strength == std::string(laws::STRENGTH_READS));
    CHECK(xget(f, "synthesis") == R"([["WAIT","DONE"]])");
    CHECK(has(f.message, "HYPOTHESIS"));
}

TEST_CASE("ltl G (req -> F_k ack) holds now and fails into a bad sink") {
    auto fsm = fsm_of("fsm_step");
    auto ok = check("G (state == ST_IDLE -> F_8 (state == ST_IDLE))", fsm);
    CHECK(ok.status == std::string(laws::PROVED));
    CHECK(xget(ok, "k") == "8");
    auto bad = check("G (state == ST_WORK -> F_8 (state == ST_IDLE))", fsm);
    CHECK(bad.status == std::string(laws::FAILED));
}

TEST_CASE("ltl unbounded G (req -> F ack) is the F approximation, BOUNDED never PROVED") {
    auto f = check("G (state == ST_IDLE -> F (state == ST_IDLE))", fsm_of("fsm_step"));
    CHECK(f.status == std::string(laws::BOUNDED));
    require_not_proof(f);
    CHECK(xget(f, "approx_kind") == "F");
    CHECK(xget(f, "safety_approx") == "G ((state == ST_IDLE) -> F_8 (state == ST_IDLE))");
    CHECK_THROWS(laws::refuse_merge(f.status, laws::PROVED));
    // Into a sink the approximation is refuted.
    auto bad = check("G (state == ST_WORK -> F (state == ST_IDLE))", fsm_of("fsm_step"));
    CHECK(bad.status == std::string(laws::FAILED));
    CHECK(xget(bad, "approx_kind") == "F");
    require_not_proof(bad);
    // The explicit bound is the fragment itself: PROVED, no approximation.
    auto exact = check("G (state == LIVE_IDLE -> F_8 (state == LIVE_ACK))", fsm_of("fsm_recur"));
    CHECK(exact.status == std::string(laws::PROVED));
    CHECK_FALSE(exact.extra.contains("approx_kind"));
}

// ---- liveness approximations on fsm_recur / fsm_settle / fsm_step ----

TEST_CASE("ltl GF approx holds: BOUNDED with approx extras, never PROVED") {
    auto f = check("GF (state == LIVE_IDLE)", fsm_of("fsm_recur"));
    CHECK(f.status == std::string(laws::BOUNDED));
    require_not_proof(f);
    CHECK(xget(f, "approx_kind") == "GF");
    CHECK(xget(f, "safety_approx") == "G (F_8 (state == LIVE_IDLE))");
    CHECK(xget(f, "strix_not_proved") == "true");
    CHECK(has(xget(f, "strix_note"), "safety fragment"));
    CHECK(has(f.message, "not a proof"));
    CHECK_THROWS(laws::refuse_merge(laws::PROVED, f.status));
    CHECK_THROWS(laws::refuse_merge(laws::PROVED, laws::BOUNDED));
    // The same spec from testdata/gf_recur.ltl through run_ltl.
    auto recs = prism::run_ltl({plant("fsm_recur")}, {td_root() / "gf_recur.ltl"});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(laws::BOUNDED));
    CHECK(xget(recs[0], "approx_kind") == "GF");
    CHECK(xget(recs[0], "strix_not_proved") == "true");
    CHECK(recs[0].extra.contains("safety_approx"));
    require_not_proof(recs[0]);
}

TEST_CASE("ltl G F p and G (F p) are the same GF approximation") {
    auto fsm = fsm_of("fsm_recur");
    for (auto* formula : {"G F (state == LIVE_IDLE)", "G (F (state == LIVE_IDLE))"}) {
        CAPTURE(formula);
        auto f = check(formula, fsm);
        CHECK(f.status == std::string(laws::BOUNDED));
        CHECK(xget(f, "approx_kind") == "GF");
        require_not_proof(f);
    }
}

TEST_CASE("ltl explicit G (F_k p) is in the fragment and PROVED; its false twin FAILS") {
    auto f = check("G (F_8 (state == LIVE_IDLE))", fsm_of("fsm_recur"));
    CHECK(f.status == std::string(laws::PROVED));
    CHECK_FALSE(f.extra.contains("approx_kind"));
    auto bad = check("G (F_8 (state == LIVE_BUSY))", fsm_of("fsm_recur"));
    CHECK(bad.status == std::string(laws::FAILED));
}

TEST_CASE("ltl GF approx into a sink is FAILED") {
    auto f = check("GF (state == ST_IDLE)", fsm_of("fsm_step"));
    CHECK(f.status == std::string(laws::FAILED));
    CHECK(xget(f, "approx_kind") == "GF");
    require_not_proof(f);
}

TEST_CASE("ltl FG approx: settles is BOUNDED, a cycle is FAILED") {
    auto ok = check("FG (state == LIVE_IDLE)", fsm_of("fsm_settle"));
    CHECK(ok.status == std::string(laws::BOUNDED));
    CHECK(xget(ok, "approx_kind") == "FG");
    require_not_proof(ok);
    auto cyc = check("F G (state == LIVE_IDLE)", fsm_of("fsm_recur"));
    CHECK(cyc.status == std::string(laws::FAILED));
    require_not_proof(cyc);
}

TEST_CASE("ltl top-level until: holds is BOUNDED, a real violation is FAILED") {
    auto ok = check("state == LIVE_ACK U state == LIVE_IDLE", fsm_of("fsm_recur"));
    CHECK(ok.status == std::string(laws::BOUNDED));
    CHECK(xget(ok, "approx_kind") == "UNTIL");
    require_not_proof(ok);
    auto bad = check("state == ST_WORK U state == ST_IDLE", fsm_of("fsm_step"));
    CHECK(bad.status == std::string(laws::FAILED));
    CHECK(has(bad.message, "\xC2\xACp \xE2\x88\xA7 \xC2\xACq"));
    CHECK(has(bad.message, "before q"));
}

TEST_CASE("ltl G (p U q), nested GF-until and nested until stay NOTRUN") {
    auto step = plant("fsm_step");
    for (auto* formula : {"G (state == ST_WORK U state == ST_IDLE)",
                          "G F (state == ST_IDLE U state == ST_WORK)",
                          "state == ST_IDLE U (state == ST_WORK U state == ST_BAD)"}) {
        CAPTURE(formula);
        auto recs = run_spec(step, std::string(formula) + "\n");
        REQUIRE_FALSE(recs.empty());
        CHECK(recs[0].status == std::string(laws::NOTRUN));
        CHECK(recs[0].status != std::string(laws::BOUNDED));
        require_not_proof(recs[0]);
    }
    CHECK_FALSE(prism::check_ltl_safety("state == ST_IDLE U (state == ST_WORK U state == ST_BAD)",
                                        fsm_of("fsm_step"))
                    .has_value());
}

TEST_CASE("ltl bare F stays NOTRUN and names Strix") {
    auto recs = run_spec(plant("fsm_step"), "F (state == ST_IDLE)\n");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(laws::NOTRUN));
    CHECK(has(recs[0].message, "Strix"));
    CHECK(xget(recs[0], "strix_not_proved") == "true");
    require_not_proof(recs[0]);
}

// ---- strix: located and recorded, never run for a verdict ----

TEST_CASE("ltl strix path is recorded and its absence is empty; still NOTRUN") {
    prism::Config cfg;
    auto exe = cfg.which_adapter("strix", {"strix", "strix.exe"});
    auto recs = run_spec(plant("fsm_step"), "F (state == ST_IDLE)\n", cfg);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(laws::NOTRUN));
    CHECK(xget(recs[0], "strix_not_proved") == "true");
    CHECK(recs[0].extra.contains("strix_note"));
    CHECK(xget(recs[0], "strix") == (exe ? exe->string() : std::string()));
    require_not_proof(recs[0]);
}

#ifndef _WIN32
TEST_CASE("ltl a present strix binary is never invoked for a proof") {
    TmpDir t("strix");
    auto marker = t.dir / "ran";
    auto fake = t.put("strix", "#!/bin/sh\necho REALIZABLE\ntouch '" + marker.string() + "'\n");
    fs::permissions(fake, fs::perms::owner_all, fs::perm_options::add);
    prism::Config cfg;
    cfg.tools["strix"] = fake;
    auto recs = run_spec(plant("fsm_step"), "F (state == ST_IDLE)\n", cfg);
    REQUIRE_FALSE(recs.empty());
    CHECK_FALSE(fs::exists(marker));
    CHECK(recs[0].status == std::string(laws::NOTRUN));
    CHECK(xget(recs[0], "strix_not_proved") == "true");
    CHECK(xget(recs[0], "strix") == fake.string());
    CHECK(has(recs[0].message, "does not treat strix output as PROVED"));
    require_not_proof(recs[0]);
}
#endif

// ---- spec files ----

TEST_CASE("ltl no spec, or a comment-only spec, is NOTRUN never CLEAN") {
    auto step = plant("fsm_step");
    auto none = prism::run_ltl({step}, {});
    REQUIRE_FALSE(none.empty());
    CHECK(none[0].status == std::string(laws::NOTRUN));
    require_not_proof(none[0]);
    for (auto* text : {"# G (state != BAD)\n\n", "// G (state != BAD)\n\n"}) {
        CAPTURE(text);
        auto recs = run_spec(step, text);
        REQUIRE(recs.size() == 1);
        CHECK(recs[0].status == std::string(laws::NOTRUN));
        CHECK(has(recs[0].message, "no .ltl spec"));
        require_not_proof(recs[0]);
    }
}

TEST_CASE("ltl testdata/comments_only.ltl has only # and // lines and is NOTRUN") {
    auto spec = td_root() / "comments_only.ltl";
    REQUIRE(fs::is_regular_file(spec));
    std::ifstream in(spec);
    std::stringstream ss;
    ss << in.rdbuf();
    auto text = ss.str();
    CHECK(has(text, "#"));
    CHECK(has(text, "//"));
    std::istringstream lines(text);
    std::string ln;
    while (std::getline(lines, ln)) {
        auto a = ln.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        auto s = ln.substr(a);
        CHECK((s.starts_with("#") || s.starts_with("//")));
    }
    auto recs = prism::run_ltl({plant("fsm_step")}, {spec});
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == std::string(laws::NOTRUN));
    CHECK(has(recs[0].message, "no .ltl spec"));
    require_not_proof(recs[0]);
}

TEST_CASE("ltl safety formula without a switch(state) machine is NOTRUN") {
    auto recs = run_spec(plant("fsm_ev_only"), "G (state != BAD)\n");
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == std::string(laws::NOTRUN));
    CHECK(has(recs[0].message, "switch(state)"));
    require_not_proof(recs[0]);
}

TEST_CASE("ltl G (p -> boolean) is an invariant: holds and fails") {
    auto fsm = fsm_of("fsm_step");
    CHECK(check("G (state == ST_IDLE -> (state == ST_IDLE || state == ST_WORK))", fsm).status ==
          std::string(laws::PROVED));
    auto bad = check("G (state == ST_IDLE -> state == ST_WORK)", fsm);
    CHECK(bad.status == std::string(laws::FAILED));
    CHECK(has(bad.message, "ST_IDLE"));
}
