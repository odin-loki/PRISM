// Roadmap 9.2 / 9.3 / 4.2 (src/prism/ai/proof_search.cpp, proof_repair.cpp,
// contracts.cpp, assumption_audit.cpp, review.cpp). Part of prism_tests.
// No model on the test machine: every model half runs against a
// deterministic fake backend; Lean checks use the real kernel when lake is
// installed (skipped with a message otherwise).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/ai.hpp"
#include "prism/ai_proof.hpp"
#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai9 {

// Replies chosen by a substring of the prompt (first match wins), else a
// queue, else "" (rejected by every validator).
struct Fake final : prism::ai::ModelBackend {
    std::vector<std::pair<std::string, std::vector<std::string>>> by_prompt;
    std::map<std::string, std::size_t> used;
    std::vector<std::string> queue;
    std::size_t next = 0;
    std::vector<prism::ai::ModelRequest> seen;
    std::string name() const override { return "fake:ai9"; }
    std::string model_sha256() const override { return "unknown"; }
    prism::ai::ModelReply complete(const prism::ai::ModelRequest& r) override {
        seen.push_back(r);
        for (auto& [key, replies] : by_prompt) {
            if (r.user.find(key) == std::string::npos || replies.empty()) continue;
            auto& n = used[key];
            auto& t = replies[std::min(n, replies.size() - 1)];
            ++n;
            return {t, ""};
        }
        if (next < queue.size()) return {queue[next++], ""};
        return {"", ""};
    }
};

fs::path tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-ai9-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<nlohmann::json> audit_lines(const fs::path& out) {
    std::vector<nlohmann::json> v;
    std::ifstream in(out / "ai_audit.jsonl");
    std::string l;
    while (std::getline(in, l))
        if (!l.empty()) v.push_back(nlohmann::json::parse(l));
    return v;
}

prism::FunctionInfo fn_of(const fs::path& file, const std::string& name) {
    for (auto& f : prism::extract_functions(file, file.string()))
        if (f.name == name) return f;
    FAIL("missing function " << name);
    return {};
}

prism::FunctionInfo scalar(const std::string& name, const std::string& body,
                           std::vector<std::pair<std::string, std::string>> params = {{"int", "n"}}) {
    prism::FunctionInfo fn;
    fn.file = "mem.c";
    fn.name = name;
    fn.kind = "SCALAR";
    fn.params = std::move(params);
    fn.signature = "int " + name + "(...)";
    fn.body = body;
    return fn;
}

}  // namespace ai9

using namespace ai9;
namespace ai = prism::ai;
namespace laws = prism::laws;

// ============================================================ Lean proof search
TEST_CASE("ai9 lean: validator admits tactics and rejects commands and escape hatches") {
    CHECK(ai::validate_lean_tactics(R"({"tactics": ["omega"]})").ok);
    CHECK(ai::validate_lean_tactics(R"({"tactics": ["constructor", "· omega", "· exact Nat.le_refl _"]})").ok);
    for (auto* bad : {R"({"tactics": ["sorry"]})", R"({"tactics": ["admit"]})", R"({"tactics": ["native_decide"]})",
                      R"({"tactics": ["decide!"]})", R"({"tactics": ["#eval IO.println 1"]})",
                      R"({"tactics": ["set_option debug.skipKernelTC true in omega"]})",
                      R"J({"tactics": ["exact (IO.println \"x\")"]})J", R"({"tactics": ["run_tac Lean.Elab.Tactic.done"]})",
                      R"({"tactics": ["omega /- hide"]})", R"({"tactics": ["end Foo"]})", R"({"tactics": []})",
                      R"({"tactics": ["axiom x : False"]})", R"({"tactics": ["ok"], "verdict": "PROVED"})",
                      R"(["omega"])", "PROVED"}) {
        auto v = ai::validate_lean_tactics(bad);
        CHECK_MESSAGE(!v.ok, bad);
    }
}

TEST_CASE("ai9 lean: target location, splice and output parsing") {
    auto dir = tmp("lean-target");
    auto file = dir / "T.lean";
    write(file,
          "namespace Foo\n"
          "-- theorem ghost : True := sorry   (a comment, not a target)\n"
          "theorem t (a b : Nat) : a + b = b + a := by\n  sorry\n\n"
          "theorem two (n : Nat) : n = n := sorry\n"
          "theorem done' (n : Nat) : n = n := rfl\n"
          "theorem partial (n : Nat) : n = n ∧ True := by\n  constructor\n  sorry\n"
          "end Foo\n");
    auto names = ai::lean_sorry_theorems(file);
    CHECK(names == std::vector<std::string>{"t", "two", "partial"});
    std::string why;
    auto t = ai::find_lean_target(file, "Foo.t", &why);
    REQUIRE_MESSAGE(t, why);
    CHECK(t->theorem == "t");
    CHECK(t->statement == "theorem t (a b : Nat) : a + b = b + a");
    auto spliced = ai::splice_proof(slurp(file), *t, {"omega"}, true);
    CHECK(spliced.find("a + b = b + a := by\n  omega\n") != std::string::npos);
    CHECK(spliced.find("#print axioms t\n") != std::string::npos);
    CHECK(spliced.find("#print axioms t\ntheorem two") != std::string::npos);
    auto t2 = ai::find_lean_target(file, "two", &why);
    REQUIRE(t2);
    CHECK(ai::splice_proof(slurp(file), *t2, {"rfl"}, false).find("n = n := by\n  rfl\n") != std::string::npos);
    // A sorry that is only part of a proof is not searched.
    CHECK_FALSE(ai::find_lean_target(file, "partial", &why));
    CHECK(why.find("not the whole proof") != std::string::npos);
    CHECK_FALSE(ai::find_lean_target(file, "done'", &why));
    CHECK_FALSE(ai::find_lean_target(file, "ghost", &why));

    auto ok = ai::parse_lean_output("'Foo.t' depends on axioms: [propext, Quot.sound]\n", "t", 0);
    CHECK(ok.complete);
    CHECK(ok.axioms_ok);
    auto bad_ax = ai::parse_lean_output("'t' depends on axioms: [propext, sorryAx]\n", "t", 0);
    CHECK(bad_ax.complete);
    CHECK_FALSE(bad_ax.axioms_ok);
    auto none = ai::parse_lean_output("'t' does not depend on any axioms\n", "t", 0);
    CHECK(none.axioms_ok);
    auto unsolved = ai::parse_lean_output("T.lean:3:41: error: unsolved goals\ncase left\nn : Nat\n⊢ n = n\n\ncase right\n⊢ True\n", "t", 1);
    CHECK(unsolved.only_unsolved);
    CHECK(unsolved.goal_count == 2);
    auto err = ai::parse_lean_output("T.lean:4:2: error: The rfl tactic failed.\na b : Nat\n⊢ a + b = b + a\n", "t", 1);
    CHECK_FALSE(err.only_unsolved);
    CHECK_FALSE(err.complete);
    CHECK(err.errors.find("rfl") != std::string::npos);
    CHECK_FALSE(ai::parse_lean_output("", "t", 0).axioms_ok);  // no report = not audited = not accepted
}

TEST_CASE("ai9 lean: best-first search rejects a wrong proof with the real kernel, accepts the right one") {
    auto lake = ai::find_lake();
    if (!lake) {
        MESSAGE("SKIP: lake not installed (the search itself is NOTRUN then)");
        ai::ProveOptions o;
        o.file = "/nonexistent.lean";
        return;
    }
    auto dir = tmp("lean-search");
    auto proj = dir / "proj";
    auto repo_toolchain = fs::path(__FILE__).parent_path().parent_path().parent_path() / "proofs" / "lean-toolchain";
    write(proj / "lean-toolchain", slurp(repo_toolchain));
    write(proj / "lakefile.toml", "name = \"t\"\nversion = \"0.1.0\"\ndefaultTargets = [\"T\"]\n\n[[lean_lib]]\nname = \"T\"\n");
    write(proj / "T.lean",
          "theorem t (a b : Nat) : a + b = b + a := by\n  sorry\n\n"
          "theorem u (a b : Nat) : a + b = b + a ∧ True := by\n  sorry\n\n"
          "axiom bad : False\n"
          "theorem w : (1 : Nat) = 2 := by\n  sorry\n");
    auto out = dir / "out";
    prism::Config cfg = prism::default_config();
    cfg.out = out;
    ai::Session session(cfg);

    // t: a wrong proof (kernel error), an escape hatch (validator), then the right one.
    auto fake = std::make_shared<Fake>();
    fake->queue = {R"({"tactics": ["rfl"]})", R"({"tactics": ["sorry"]})", R"({"tactics": ["omega"]})"};
    ai::ProveOptions o;
    o.file = proj / "T.lean";
    o.theorem = "t";
    o.backend = fake;
    o.write = true;
    o.lemmas = dir / "lemmas.jsonl";
    o.out = out;
    o.budget = 5;
    auto r = ai::prove_theorem(o);
    REQUIRE_MESSAGE(r.status == std::string(laws::PROVED), r.reason, " | ", nlohmann::json(r.attempts).dump());
    CHECK(r.proof == std::vector<std::string>{"omega"});
    CHECK(r.rejected_kernel == 1);
    CHECK(r.rejected_invalid == 1);
    CHECK(r.model_calls == 3);
    CHECK(r.written);
    for (auto& a : r.axioms) CHECK((a == "propext" || a == "Quot.sound" || a == "Classical.choice"));
    auto text = slurp(proj / "T.lean");
    CHECK(text.find("a + b = b + a := by\n  omega\n") != std::string::npos);
    CHECK(text.find("#print axioms") == std::string::npos);
    auto lemmas = ai::read_lemmas(dir / "lemmas.jsonl");
    REQUIRE(lemmas.size() == 1);
    CHECK(lemmas[0].theorem == "t");
    CHECK(lemmas[0].proof == "omega");
    CHECK(lemmas[0].audit_id == r.audit_id);
    // The model saw the Lean error of the wrong attempt, fenced as untrusted.
    REQUIRE(fake->seen.size() == 3);
    CHECK(fake->seen[1].user.find("rfl") != std::string::npos);
    CHECK(fake->seen[1].user.find("<<<UNTRUSTED LEAN ERROR") != std::string::npos);
    CHECK(fake->seen[0].grammar == "lean_proof");

    // u: a partial step (two goals left) is kept in the tree and extended.
    auto fake2 = std::make_shared<Fake>();
    fake2->queue = {R"({"tactics": ["constructor"]})", R"({"tactics": ["· omega", "· trivial"]})"};
    o.theorem = "u";
    o.backend = fake2;
    o.write = false;
    auto r2 = ai::prove_theorem(o);
    REQUIRE_MESSAGE(r2.status == std::string(laws::PROVED), r2.reason, " | ", nlohmann::json(r2.attempts).dump());
    CHECK(r2.proof == std::vector<std::string>{"constructor", "· omega", "· trivial"});
    CHECK_FALSE(r2.written);
    CHECK(slurp(proj / "T.lean").find("∧ True := by\n  sorry") != std::string::npos);  // not written
    CHECK(fake2->seen[1].user.find("ACCEPTED PREFIX") != std::string::npos);
    CHECK(fake2->seen[0].user.find("omega") != std::string::npos);  // lemma library in the prompt

    // w: the kernel accepts a proof from a user axiom; the axiom audit does not.
    auto fake3 = std::make_shared<Fake>();
    fake3->queue = {R"({"tactics": ["exact bad.elim"]})"};
    o.theorem = "w";
    o.backend = fake3;
    o.budget = 1;
    auto r3 = ai::prove_theorem(o);
    CHECK(r3.status == std::string(laws::UNKNOWN));
    CHECK(r3.rejected_axioms == 1);
    CHECK(r3.proof.empty());

    auto lines = audit_lines(out);
    REQUIRE(lines.size() == 3 + 2 + 1);
    CHECK(lines[0]["feature"] == "lean-proof");
    CHECK(lines[0]["checker_result"].get<std::string>().rfind("rejected:", 0) == 0);
    CHECK(lines[1]["checker"] == "grammar-validator");
    CHECK(lines[2]["checker_result"] == "accepted");
    CHECK(lines[2]["verdict_effect"] == "PROVED");
    CHECK(lines[3]["checker_result"].get<std::string>().rfind("partial:", 0) == 0);
    CHECK(lines[5]["checker_result"].get<std::string>().find("bad") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("ai9 lean: no prover model is NOTRUN, never a proof") {
    auto dir = tmp("lean-notrun");
    write(dir / "lakefile.toml", "name = \"t\"\n[[lean_lib]]\nname = \"T\"\n");
    write(dir / "T.lean", "theorem t : True := by\n  sorry\n");
    ai::ProveOptions o;
    o.file = dir / "T.lean";
    o.theorem = "t";
    o.out = dir / "out";
    o.prover_gguf = dir / "missing-prover.gguf";
    auto r = ai::prove_theorem(o);
    CHECK(r.status == std::string(laws::NOTRUN));
    CHECK_FALSE(r.reason.empty());
    CHECK(r.proof.empty());
}

// ============================================================ assumption audit
TEST_CASE("ai9 vacuity: Z3 flags a requires no input satisfies, and only that") {
    auto fn = scalar("vac", "return n;");
    CHECK(ai::assumptions_satisfiable(fn, {"n > 5", "n < 3"}) == "unsat");
    CHECK(ai::assumptions_satisfiable(fn, {"n > 5"}) == "sat");
    CHECK(ai::assumptions_satisfiable(fn, {"n > 2147483647"}) == "unsat");  // int width
    CHECK(ai::assumptions_satisfiable(fn, {"p != NULL"}).rfind("unknown", 0) == 0);
    CHECK(ai::split_conjuncts("(n > 0) && (m < 3 || m > 5) && k") ==
          std::vector<std::string>{"n > 0", "m < 3 || m > 5", "k"});

    prism::Finding f;
    f.stage = "contracts";
    f.status = std::string(laws::PROVED_ASSUMING);
    f.file = fn.file;
    f.function = fn.name;
    f.extra["requires"] = "n > 5 && n < 3";
    prism::Finding g = f;
    g.extra["requires"] = "n >= 0";
    prism::Finding h = f;
    h.stage = "harness";
    h.extra.erase("requires");
    h.extra["assumptions"] = R"J(["n != NULL", "5 <= n <= 4 (checked size range)"])J";
    auto out = ai::vacuity_audit({fn}, {f, g, h}, "review");
    REQUIRE(out.size() == 2);
    CHECK(out[0].status == std::string(laws::FAILED));
    CHECK(out[0].cls == "VACUOUS-ASSUMPTION");
    CHECK(out[0].extra["audited_status"] == std::string(laws::PROVED_ASSUMING));
    CHECK(out[1].extra["culprit"] == "n >= 5 && n <= 4");
}

TEST_CASE("ai9 assumption audit: model flags are READS, never a verdict change") {
    auto out_dir = tmp("audit-model");
    prism::Config cfg = prism::default_config();
    cfg.out = out_dir;
    ai::Session session(cfg);
    auto fn = scalar("sat_add", "return n + 1;");
    prism::Finding f;
    f.stage = "contracts";
    f.status = std::string(laws::PROVED_ASSUMING);
    f.file = fn.file;
    f.function = fn.name;
    f.extra["requires"] = "n >= 0 && n < 100";
    auto fake = std::make_shared<Fake>();
    fake->queue = {R"({"flags": [{"index": 1, "input": "n = 5000", "reason": "callers pass buffer sizes above 100"}]})"};
    ai::set_session_backend_for_testing(fake);
    auto flags = ai::model_assumption_audit({fn}, {f}, "review");
    REQUIRE(flags.size() == 1);
    CHECK(flags[0].status == std::string(laws::READS));
    CHECK(flags[0].strength == std::string(laws::STRENGTH_READS));
    CHECK(flags[0].extra["assumption"] == "requires n < 100");
    CHECK(f.status == std::string(laws::PROVED_ASSUMING));  // the audited verdict is untouched
    // An index out of range or extra keys are rejected by the validator.
    CHECK_FALSE(ai::validate_assumption_audit(R"({"flags": [{"index": 7, "input": "", "reason": "x"}]})", 2).ok);
    CHECK_FALSE(ai::validate_assumption_audit(R"({"flags": [], "verdict": "FAILED"})", 2).ok);
    auto lines = audit_lines(out_dir);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0]["feature"] == "assumption-audit");
    CHECK(lines[0]["verdict_effect"] == "none");
    ai::set_session_backend_for_testing(nullptr);
    CHECK(ai::model_assumption_audit({fn}, {f}, "review").empty());
}

// ============================================================ contracts
TEST_CASE("ai9 contracts: syntax helpers, hashes and requirements") {
    CHECK(ai::contract_to_c("n > 0 ==> \\result > 0") == "(!(n > 0) || (result > 0))");
    CHECK(ai::contract_to_c("(a ==> b) && c") == "((!(a) || (b))) && c");
    CHECK(ai::clause_hash("f", "requires  n >= 0;") == ai::clause_hash("f", "requires n >= 0;"));
    CHECK(ai::clause_hash("f", "requires n >= 0;") != ai::clause_hash("g", "requires n >= 0;"));
    auto dir = tmp("reqs");
    write(dir / "req.md", "# Requirements\n\n- The half function shall only be called with a non-negative n. It "
                          "returns at most n.\n\n```\ncode block is not a requirement.\n```\nok\n");
    auto reqs = ai::load_requirements({dir});
    REQUIRE(reqs.size() == 2);
    CHECK(reqs[0].id == "R1");
    CHECK(reqs[0].line == 3);
    CHECK(reqs[0].text == "The half function shall only be called with a non-negative n.");
    CHECK(reqs[1].text == "It returns at most n.");
    auto v = ai::validate_contract("requires n >= 0; // from R1\nensures \\result <= n; // from code\n", {"n"});
    REQUIRE(v.ok);
    CHECK(v.traces == std::vector<std::string>{"R1", "code"});
    CHECK_FALSE(ai::validate_contract("requires n >= 0; // from somewhere\n", {"n"}).ok);
}

TEST_CASE("ai9 contracts: callers are checked against a requires, modularly") {
    auto callee = scalar("half", "return n / 2;");
    auto ok = scalar("use_half", "if (x < 0) return 0; return half(x);", {{"int", "x"}});
    auto bad = scalar("bad_half", "int y = x - 10; return half(y);", {{"int", "x"}});
    auto none = scalar("nothing", "return x;", {{"int", "x"}});
    auto cond = scalar("cond_half", "return x > 0 && half(x) > 1;", {{"int", "x"}});
    CHECK(ai::check_callers_requires(ok, callee, "n >= 0", 8) == "satisfies");
    CHECK(ai::check_callers_requires(bad, callee, "n >= 0", 8).rfind("violates", 0) == 0);
    CHECK(ai::check_callers_requires(none, callee, "n >= 0", 8) == "no-calls");
    CHECK(ai::check_callers_requires(cond, callee, "n >= 0", 8).rfind("unknown", 0) == 0);
}

TEST_CASE("ai9 contracts: drafted contract is HYPOTHESIS with trace links; approved one is proved") {
    auto dir = tmp("draft");
    auto src = dir / "src";
    write(src / "half.c",
          "int half(int n) { return n / 2; }\n"
          "int use_half(int x) { if (x < 0) return 0; return half(x); }\n"
          "int bad_half(int x) { int y = x - 10; return half(y); }\n");
    write(dir / "req" / "spec.md", "The half function shall only be called with a non-negative n.\n"
                                   "The result of half never exceeds its argument.\n");
    auto fns = prism::extract_functions(src / "half.c", "half.c");
    REQUIRE(fns.size() == 3);
    prism::Config cfg = prism::default_config();
    cfg.root = src;
    cfg.out = dir / "out";
    cfg.requirements = {dir / "req"};
    ai::Session session(cfg);
    auto fake = std::make_shared<Fake>();
    fake->by_prompt = {{"int half(int n)", {"requires n >= 0; // from R1\nensures \\result <= n; // from R2\n"}},
                       {"int use_half", {"requires x >= 0; // from R9\n"}},  // R9 was never offered
                       {"int bad_half", {"requires x > 5; // from code\nrequires x < 3; // from code\n"}}};
    ai::set_session_backend_for_testing(fake);
    auto out = ai::draft_contracts(fns, cfg);
    const prism::Finding* half = nullptr;
    const prism::Finding* vac = nullptr;
    for (auto& f : out) {
        if (f.function == std::string("half")) half = &f;
        if (f.function == std::string("bad_half")) vac = &f;
        CHECK_FALSE(laws::is_proof(f.status));  // nothing drafted is a verdict
    }
    REQUIRE(half);
    CHECK(half->status == std::string(laws::HYPOTHESIS));
    CHECK(half->extra.at("contract_state") == "drafted");
    CHECK(half->extra.at("proof_status") == std::string(laws::PROVED_ASSUMING));
    auto trace = nlohmann::json::parse(half->extra.at("trace"));
    REQUIRE(trace.size() == 2);
    CHECK(trace[0]["source"] == "R1");
    CHECK(trace[0]["text"] == "The half function shall only be called with a non-negative n.");
    CHECK(trace[0]["line"] == 1);
    auto callers = nlohmann::json::parse(half->extra.at("callers"));
    CHECK(callers["half.c:use_half"] == "satisfies");
    CHECK(callers["half.c:bad_half"].get<std::string>().rfind("violates", 0) == 0);
    REQUIRE(vac);  // the vacuous draft is rejected by Z3, never proved
    CHECK(vac->cls == "VACUOUS-ASSUMPTION");
    CHECK(vac->status == std::string(laws::HYPOTHESIS));
    // use_half's draft named a requirement that was not offered: rejected, no row.
    for (auto& f : out) CHECK(f.function != std::string("use_half"));
    auto lines = audit_lines(cfg.out);
    REQUIRE(lines.size() == 3);
    int rejected = 0;
    for (auto& l : lines) {
        CHECK(l["feature"] == "contract");
        if (l["output_valid"] == false) ++rejected;
        CHECK(l["verdict_effect"] == "none");
    }
    CHECK(rejected == 1);

    // A human approves the drafted clauses: the next run proves them, without a model.
    nlohmann::json approved{{"approved", nlohmann::json::parse(half->extra.at("approve_with"))}};
    write(src / "contracts.approved.json", approved.dump(2));
    ai::set_session_backend_for_testing(nullptr);
    auto out2 = ai::draft_contracts(fns, cfg);
    const prism::Finding* proved = nullptr;
    const prism::Finding* caller_fail = nullptr;
    for (auto& f : out2) {
        if (f.function == std::string("half")) proved = &f;
        if (f.function == std::string("bad_half")) caller_fail = &f;
    }
    REQUIRE(proved);
    CHECK(proved->status == std::string(laws::PROVED_ASSUMING));
    CHECK(proved->extra.at("contract_state") == "approved");
    REQUIRE(caller_fail);
    CHECK(caller_fail->status == std::string(laws::FAILED));
    CHECK(caller_fail->cls == "FUNC-CONTRACT");
    // A tampered approval (hash of another clause) approves nothing.
    auto tampered = approved;
    tampered["approved"][0]["clause"] = "requires n >= -5;";
    write(src / "contracts.approved.json", tampered.dump(2));
    CHECK(ai::load_approvals(src / "contracts.approved.json").size() == 1);
}

// ============================================================ proof store / regression
TEST_CASE("ai9 proof store: a changed function whose stored contract no longer checks is a PROOF-REGRESSION") {
    auto dir = tmp("regress");
    prism::Config cfg = prism::default_config();
    cfg.root = dir;
    cfg.out = dir / "out";
    cfg.unwind = 8;
    auto v1 = scalar("inc", "if (n > 100) return 0; return n + 1;");
    prism::StageResult contracts;
    contracts.name = "contracts";
    auto proved = prism::prove_with_contract(v1, 8, std::string("n >= 0"), std::string("result >= 0"));
    REQUIRE(proved.status == std::string(laws::PROVED_ASSUMING));
    contracts.findings = {proved};
    // Run 1: the proof is recorded.
    CHECK(ai::run_proof_regression({v1}, {contracts}, cfg).empty());
    auto store = ai::load_proof_store(cfg.out / "proof_store.json");
    REQUIRE(store.size() == 1);
    CHECK(store[0].fingerprint == ai::function_fingerprint(v1));
    CHECK(store[0].artefacts[0].requires_ == "n >= 0");
    // Comments and whitespace do not change the fingerprint.
    auto v1b = v1;
    v1b.body = "if (n > 100)   return 0; // cap\n return n + 1;";
    CHECK(ai::function_fingerprint(v1b) == ai::function_fingerprint(v1));

    // Run 2: the function changed, the current run no longer proves it, and the
    // stored contract fails on the new code.
    auto v2 = scalar("inc", "if (n > 100) return 0; return n - 1;");
    auto out = ai::run_proof_regression({v2}, {}, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == std::string(laws::UNKNOWN));
    CHECK(out[0].cls == "PROOF-REGRESSION");
    CHECK(out[0].extra["regression"] == "true");
    CHECK(out[0].extra["previous_status"] == std::string(laws::PROVED_ASSUMING));
    CHECK(out[0].extra["code_changed"] == "true");
    CHECK(out[0].extra["repair"].rfind("NOTRUN", 0) == 0);
    // The regression is kept in the store and reported again until fixed.
    CHECK(ai::run_proof_regression({v2}, {}, cfg).size() == 1);

    // Run 3: a refactor that still satisfies the stored contract is re-proved
    // from the store by the checker.
    auto v3 = scalar("inc", "int r = 0; if (n <= 100) r = n + 1; return r;");
    auto out3 = ai::run_proof_regression({v3}, {}, cfg);
    REQUIRE(out3.size() == 1);
    CHECK(out3[0].status == std::string(laws::PROVED_ASSUMING));
    CHECK(out3[0].extra["proof_store"] == "reused");
}

TEST_CASE("ai9 proof repair: model invariants for the changed loop are accepted only after Houdini") {
    auto dir = tmp("repair");
    prism::Config cfg = prism::default_config();
    cfg.root = dir;
    cfg.out = dir / "out";
    ai::Session session(cfg);
    // v1 was proved with y == 7 * x; the new code steps y by 9.
    auto v2 = scalar("grow",
                     "int x; int y; x = 0; y = 0; if (n > 300) return 0; while (x < n) { y = y + 4; y = y + 5; "
                     "x = x + 1; } return 100 / (y - x - x - x - x - x - x - x - x - x + 1);");
    ai::ProofEntry e;
    e.file = v2.file;
    e.function = v2.name;
    e.fingerprint = "0000-old";
    ai::ProofArtefact a;
    a.stage = "bmc";
    a.status = std::string(laws::PROVED_UNBOUNDED);
    a.invariants = R"([["y == 7 * x", "x >= 0", "x <= 300"]])";
    a.source = "llm:fake";
    a.unwind = 8;
    e.artefacts = {a};
    ai::save_proof_store(cfg.out / "proof_store.json", {e});
    auto fake = std::make_shared<Fake>();
    // First a wrong repair (Houdini drops it, the proof stays open), then a right one.
    fake->queue = {R"(["y == 8 * x"])", R"(["y == 9 * x", "x >= 0", "x <= 300"])"};
    ai::set_session_backend_for_testing(fake);
    auto out = ai::run_proof_regression({v2}, {}, cfg);
    REQUIRE(out.size() == 1);
    CHECK(out[0].cls == "PROOF-REGRESSION");  // one repair attempt per artefact per run: the wrong one
    auto out2 = ai::run_proof_regression({v2}, {}, cfg);
    REQUIRE(out2.size() == 1);
    CHECK_MESSAGE(out2[0].status == std::string(laws::PROVED_UNBOUNDED), out2[0].message);
    CHECK(out2[0].extra["regression"] == "repaired");
    CHECK(out2[0].extra["invariant_source"] == "llm:fake:ai9");
    auto lines = audit_lines(cfg.out);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0]["checker_result"] == "step-open");
    CHECK(lines[1]["checker_result"] == std::string(laws::PROVED_UNBOUNDED));
    CHECK(lines[1]["id"] == out2[0].extra["ai_audit_id"]);
    // The repaired invariants are stored for the next run.
    auto store = ai::load_proof_store(cfg.out / "proof_store.json");
    REQUIRE(store.size() == 1);
    CHECK(store[0].fingerprint == ai::function_fingerprint(v2));
    ai::set_session_backend_for_testing(nullptr);
    CHECK(ai::run_proof_regression({v2}, {}, cfg).at(0).extra["proof_store"] == "reused");
}

TEST_CASE("ai9 review stage in the pipeline: vacuous requires found without a model; one NOTRUN row") {
    auto dir = tmp("pipeline");
    write(dir / "src" / "v.c",
          "int never(int n) {\n"
          "    // requires: n > 10 && n < 5\n"
          "    // ensures: result >= 0\n"
          "    return n;\n"
          "}\n");
    prism::Config cfg = prism::default_config();
    cfg.root = dir / "src";
    cfg.out = dir / "out";
    cfg.llm = false;
    cfg.stages = std::vector<std::string>{"inventory", "classify", "contracts", "review"};
    auto rep = prism::run_pipeline(cfg);
    const prism::StageResult* review = nullptr;
    for (auto& s : rep.stages)
        if (s.name == "review") review = &s;
    REQUIRE(review);
    CHECK(review->status == "ok");
    bool vac = false, notrun = false;
    for (auto& f : review->findings) {
        if (f.cls == "VACUOUS-ASSUMPTION" && f.status == std::string(laws::FAILED)) vac = true;
        if (f.status == std::string(laws::NOTRUN)) notrun = true;
    }
    CHECK(vac);
    CHECK(notrun);
    CHECK(fs::exists(cfg.out / "proof_store.json"));
}
