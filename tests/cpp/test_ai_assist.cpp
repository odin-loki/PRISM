// Roadmap 9.1 / 9.3 / 9.4 assistant features: regression tests, triage,
// questions over findings, report drafting, solver/bound prediction.
// Every model-dependent part is tested with a deterministic fake backend;
// every deterministic core is tested without one.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/ai.hpp"
#include "prism/ai_assist.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/solver_predict.hpp"
#ifdef PRISM_HAS_Z3
#include "prism/pir.hpp"
#include "prism/solver.hpp"
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct AssistFake final : prism::ai::ModelBackend {
    std::vector<std::string> replies;
    std::vector<prism::ai::ModelRequest> seen;
    std::size_t next = 0;
    std::string name() const override { return "fake:assist-double"; }
    std::string model_sha256() const override { return "unknown"; }
    prism::ai::ModelReply complete(const prism::ai::ModelRequest& r) override {
        seen.push_back(r);
        if (replies.empty()) return {"", ""};
        auto& t = replies[std::min(next, replies.size() - 1)];
        ++next;
        return {t, ""};
    }
};

struct FakeEmbedder final : prism::ai::Embedder {
    int calls = 0;
    std::string name() const override { return "fake-embed"; }
    std::optional<std::vector<double>> embed(const std::string& text) override {
        ++calls;
        // Two directions: texts mentioning "overflow" vs everything else.
        if (text.find("ovf") != std::string::npos || text.find("overflow") != std::string::npos)
            return std::vector<double>{1.0, 0.0};
        return std::vector<double>{0.0, 1.0};
    }
};

fs::path assist_tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-assist-" + tag);
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

std::vector<std::string> lines_of(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    for (std::string l; std::getline(in, l);)
        if (!l.empty()) out.push_back(l);
    return out;
}

prism::Finding mk(const std::string& stage, const std::string& status, const std::string& file,
                  const std::string& fn, int line, const std::string& cls, const std::string& msg,
                  const std::string& cex = "") {
    prism::Finding f;
    f.stage = stage;
    f.status = status;
    f.file = file;
    if (!fn.empty()) f.function = fn;
    if (line) f.line = line;
    f.cls = cls;
    f.message = msg;
    f.strength = std::string(prism::laws::STRENGTH_PROVES);
    f.counterexample = cex;
    return f;
}

// A small report over a temp tree: a signed overflow found by three stages, a
// division by zero, a proof, a bounded result and a NOTRUN stage.
prism::RunReport sample_report(const fs::path& root) {
    write(root / "calc.c",
          "int add_big(int x) {\n    return x + 100;\n}\n\n"
          "int divide(int a, int b) {\n    return a / b;\n}\n\n"
          "int ok(int x) {\n    return x & 1;\n}\n");
    prism::RunReport r;
    r.root = root.string();
    for (auto [name, line] : {std::pair{"add_big", 1}, {"divide", 5}, {"ok", 9}}) {
        prism::FunctionInfo fn;
        fn.file = "calc.c";
        fn.name = name;
        fn.kind = "SCALAR";
        fn.line = line;
        fn.params = std::string(name) == "divide" ? std::vector<std::pair<std::string, std::string>>{{"int", "a"}, {"int", "b"}}
                                                  : std::vector<std::pair<std::string, std::string>>{{"int", "x"}};
        r.functions.push_back(fn);
    }
    prism::StageResult inv{"inventory", "ok", "", 0, 0, {}, 0, ""};
    prism::StageResult bmc{"bmc", "ok", "", 0, 0, {}, 0, ""};
    bmc.findings.push_back(mk("bmc", "FAILED", "calc.c", "add_big", 1, "INT-SIGNED-OVF",
                              "ovf+: INT-SIGNED-OVF", "x=#x7fffffff"));
    bmc.findings.push_back(mk("bmc", "FAILED", "calc.c", "divide", 5, "INT-DIV-ZERO", "div0: INT-DIV-ZERO",
                              "a=#x00000007, b=#x00000000"));
    bmc.findings.push_back(mk("bmc", "PROVED-UNBOUNDED", "calc.c", "ok", 9, "", "no UB"));
    prism::StageResult pir{"pir", "ok", "", 0, 0, {}, 0, ""};
    pir.findings.push_back(mk("pir", "FAILED", "calc.c", "add_big", 2, "INT-SIGNED-OVF",
                              "signed add overflow (ovf+)", "x=2147483647"));
    pir.findings.push_back(mk("pir", "BOUNDED", "calc.c", "ok", 9, "", "no violation within unwind 8"));
    prism::StageResult san{"sanitize", "ok", "", 0, 0, {}, 0, ""};
    san.findings.push_back(mk("sanitize", "SANFAIL", "calc.c", "add_big", 2, "INT-SIGNED-OVF",
                              "runtime error: signed integer overflow: 2147483647 + 100"));
    prism::StageResult fuzz{"fuzz", "NOTRUN", "libFuzzer not found", 0, 0, {}, 0, "install clang"};
    fuzz.findings.push_back(mk("fuzz", "NOTRUN", "", "", 0, "", "libFuzzer not found"));
    prism::StageResult unify{"unify", "ok", "", 0, 0, {}, 0, ""};
    r.stages = {inv, bmc, pir, san, fuzz, unify};
    return r;
}

bool have_clang_sanitizers(const fs::path& dir) {
    write(dir / "probe.c", "int main(void) { return 0; }\n");
    std::string cmd = "clang -fsanitize=undefined,address " + (dir / "probe.c").string() + " -o " +
                      (dir / "probe").string() + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

// ------------------------------------------------------------------ regress
TEST_CASE("ai-assist regress: counterexample parsing and C literals") {
    auto v = prism::ai::parse_counterexample("x=#x7fffffff, y=-5, flag=true, z = 0x10, bad=zz");
    REQUIRE(v.size() == 4);
    CHECK(v[0] == std::pair<std::string, std::string>{"x", "#x7fffffff"});
    CHECK(v[1].second == "-5");
    CHECK(v[2].second == "true");
    CHECK(v[3].second == "0x10");
    CHECK(prism::ai::c_literal_for("int", "#x7fffffff") == "(int)2147483647LL");
    CHECK(prism::ai::c_literal_for("int", "#xffffffff") == "(int)(-1LL)");
    CHECK(prism::ai::c_literal_for("const int", "-5") == "(int)(-5LL)");
    CHECK(prism::ai::c_literal_for("unsigned int", "#xffffffff") == "(unsigned int)4294967295LL");
    CHECK(prism::ai::c_literal_for("long", "#x8000000000000000") == "(long)(-9223372036854775807LL - 1)");
    CHECK(prism::ai::c_literal_for("unsigned long long", "#xffffffffffffffff") ==
          "(unsigned long long)18446744073709551615ULL");
    CHECK(prism::ai::c_literal_for("char", "#xff") == "(char)(-1LL)");
    CHECK(prism::ai::c_literal_for("_Bool", "true") == "1");
    CHECK_FALSE(prism::ai::c_literal_for("int *", "0").has_value());
    CHECK_FALSE(prism::ai::c_literal_for("struct s", "0").has_value());
}

TEST_CASE("ai-assist regress: framework detection") {
    auto root = assist_tmp("fw");
    std::string ev;
    write(root / "a.c", "int f(int x){return x;}\n");
    CHECK(prism::ai::detect_framework(root, &ev) == "plain");
    write(root / "pyproject.toml", "[project]\nname='x'\n");
    CHECK(prism::ai::detect_framework(root, &ev) == "pytest");
    write(root / "CMakeLists.txt", "project(x C)\nenable_testing()\nadd_test(NAME t COMMAND t)\n");
    CHECK(prism::ai::detect_framework(root, &ev) == "ctest");
    CHECK(ev == "CMakeLists.txt");
    write(root / "tests" / "t.cpp", "#include <catch2/catch_test_macros.hpp>\n");
    CHECK(prism::ai::detect_framework(root, &ev) == "catch2");
    write(root / "tests" / "g.cpp", "#include <gtest/gtest.h>\n");
    CHECK(prism::ai::detect_framework(root, &ev) == "gtest");
    CHECK(ev == "tests/g.cpp");
}

TEST_CASE("ai-assist regress: tests are generated per framework, never into the tree by default") {
    auto root = assist_tmp("regress-gen");
    auto report = sample_report(root / "src");
    auto before = report.dumps();
    for (std::string fw : {"plain", "ctest", "gtest", "catch2", "pytest"}) {
        prism::ai::RegressOptions opt;
        opt.out_dir = root / "out" / fw;
        opt.framework = fw;
        auto res = prism::ai::generate_regression_tests(report, opt);
        CHECK(res.framework == fw);
        int written = 0;
        for (auto& t : res.tests) written += t.status == "written";
        // add_big (bmc), divide (bmc); pir's add_big is the same call -> duplicate
        CHECK(written == 2);
        bool dup = false;
        for (auto& t : res.tests) dup = dup || (t.status == "duplicate" && t.finding_id == "pir#0");
        CHECK(dup);
        CHECK(fs::exists(opt.out_dir / "manifest.json"));
        CHECK(fs::exists(opt.out_dir / "run_tests.sh"));
        auto c = slurp(opt.out_dir / "calc_add_big.c");
        CHECK(c.find("add_big((int)2147483647LL)") != std::string::npos);
        CHECK(c.find("#define main prism_regress_user_main_") != std::string::npos);
        auto d = slurp(opt.out_dir / "calc_divide.c");
        CHECK(d.find("divide((int)7LL, (int)0LL)") != std::string::npos);
        if (fw == "ctest" || fw == "gtest" || fw == "catch2") {
            auto cm = slurp(opt.out_dir / "CMakeLists.txt");
            CHECK(cm.find("add_test(NAME calc_add_big") != std::string::npos);
            CHECK(cm.find("-fsanitize=undefined,address") != std::string::npos);
        }
        if (fw == "gtest") CHECK(slurp(opt.out_dir / "calc_add_big_test.cpp").find("EXPECT_EXIT") != std::string::npos);
        if (fw == "catch2") CHECK(fs::exists(opt.out_dir / "calc_divide_test.cpp"));
        if (fw == "pytest") CHECK(slurp(opt.out_dir / "test_prism_regression.py").find("calc_divide") != std::string::npos);
    }
    // Nothing was written next to the user's sources, and no status changed.
    CHECK_FALSE(fs::exists(root / "src" / "regression_tests"));
    CHECK(report.dumps() == before);
}

TEST_CASE("ai-assist regress: unsupported findings are listed, not dropped") {
    auto root = assist_tmp("regress-unsup");
    write(root / "p.c", "int deref(int *p) { return *p; }\n");
    prism::RunReport r;
    r.root = root.string();
    prism::FunctionInfo fn;
    fn.file = "p.c";
    fn.name = "deref";
    fn.params = {{"int *", "p"}};
    r.functions.push_back(fn);
    prism::StageResult s{"bmc", "ok", "", 0, 0, {}, 0, ""};
    s.findings.push_back(mk("bmc", "FAILED", "p.c", "deref", 1, "PTR-NULL-DEREF", "null", "p=#x00000000"));
    s.findings.push_back(mk("bmc", "FAILED", "p.c", "deref", 1, "PTR-NULL-DEREF", "no cex"));
    r.stages = {s};
    prism::ai::RegressOptions opt;
    opt.out_dir = root / "out";
    auto res = prism::ai::generate_regression_tests(r, opt);
    REQUIRE(res.tests.size() == 2);
    CHECK(res.tests[0].status == "unsupported");
    CHECK(res.tests[0].detail.find("not a scalar") != std::string::npos);
    CHECK(res.tests[1].detail == "no concrete counterexample");
    auto m = nlohmann::json::parse(slurp(res.manifest));
    CHECK(m["tests"].size() == 2);
}

TEST_CASE("ai-assist regress: --run needs --allow-exec (Law 9)") {
    auto root = assist_tmp("regress-law9");
    auto report = sample_report(root / "src");
    prism::ai::RegressOptions opt;
    opt.out_dir = root / "out";
    opt.run = true;
    opt.allow_exec = false;
    auto res = prism::ai::generate_regression_tests(report, opt);
    for (auto& t : res.tests)
        if (!t.name.empty()) CHECK(t.status == "NOTRUN");
    CHECK_FALSE(fs::exists(root / "out" / "bin"));
}

TEST_CASE("ai-assist regress: generated tests fail on the bug and pass on the fix") {
    auto root = assist_tmp("regress-run");
    if (!have_clang_sanitizers(root)) {
        MESSAGE("NOTRUN: clang with UBSan/ASan not available");
        return;
    }
    auto report = sample_report(root / "src");
    prism::ai::RegressOptions opt;
    opt.out_dir = root / "out";
    opt.run = true;
    opt.allow_exec = true;
    auto res = prism::ai::generate_regression_tests(report, opt);
    int repro = 0;
    for (auto& t : res.tests)
        if (!t.name.empty()) {
            CHECK_MESSAGE(t.status == "reproduces", t.name, ": ", t.detail);
            repro += t.status == "reproduces";
        }
    CHECK(repro == 2);
    // The framework-free runner fails too (exit 1).
    CHECK(std::system(("sh " + (opt.out_dir / "run_tests.sh").string() + " >/dev/null 2>&1").c_str()) != 0);
    // Fix the bugs: the same tests now pass.
    write(root / "src" / "calc.c",
          "int add_big(int x) {\n    return x > 2147483547 ? 2147483647 : x + 100;\n}\n\n"
          "int divide(int a, int b) {\n    return b == 0 ? 0 : a / b;\n}\n\n"
          "int ok(int x) {\n    return x & 1;\n}\n");
    auto fixed = prism::ai::generate_regression_tests(report, opt);
    for (auto& t : fixed.tests)
        if (!t.name.empty()) CHECK_MESSAGE(t.status == "does-not-reproduce", t.name, ": ", t.detail);
    CHECK(std::system(("sh " + (opt.out_dir / "run_tests.sh").string() + " >/dev/null 2>&1").c_str()) == 0);
    // ctest project: build and run with cmake when available.
    if (std::system("cmake --version >/dev/null 2>&1") == 0) {
        prism::ai::RegressOptions c = opt;
        c.run = false;
        c.framework = "ctest";
        c.out_dir = root / "ctest";
        prism::ai::generate_regression_tests(report, c);
        auto b = root / "ctest-build";
        std::string cmd = "cmake -S " + c.out_dir.string() + " -B " + b.string() +
                          " -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null 2>&1 && cmake --build " +
                          b.string() + " >/dev/null 2>&1 && ctest --test-dir " + b.string() + " >/dev/null 2>&1";
        CHECK(std::system(cmd.c_str()) == 0);  // fixed code: ctest passes
        write(root / "src" / "calc.c", "int add_big(int x) {\n    return x + 100;\n}\n\n"
                                       "int divide(int a, int b) {\n    return b == 0 ? 0 : a / b;\n}\n");
        CHECK(std::system(cmd.c_str()) != 0);  // bug back: ctest fails
    }
}

// ------------------------------------------------------------------ triage
TEST_CASE("ai-assist triage: duplicates cluster, ordering only, statuses unchanged") {
    auto root = assist_tmp("triage");
    auto report = sample_report(root / "src");
    auto before = report.dumps();
    prism::ai::set_embedder_for_testing(nullptr);
    auto t = prism::ai::triage(report);
    CHECK(t.considered == 5);  // 3x overflow, div0, BOUNDED (proof / NOTRUN excluded)
    REQUIRE(!t.clusters.empty());
    // The overflow reported by bmc, pir and sanitize is one root cause, ranked first.
    const auto& c1 = t.clusters[0];
    CHECK(c1.id == "C1");
    CHECK(c1.top_status == "FAILED");
    CHECK(c1.members.size() == 3);
    CHECK(c1.stages.size() == 3);
    CHECK(c1.representative == "bmc#0");
    // The division by zero is a different root cause.
    bool div_alone = false;
    for (auto& c : t.clusters)
        if (c.representative == "bmc#1") div_alone = c.members.size() == 1;
    CHECK(div_alone);
    CHECK(t.embedder_note.find("NOTRUN") != std::string::npos);
    // write_triage: triage.json + Clusters section; report untouched.
    auto out = root / "out";
    prism::write_report_md(report, out / "report.md");
    prism::ai::write_triage(report, out);
    CHECK(report.dumps() == before);
    auto md = slurp(out / "report.md");
    CHECK(md.find("## Findings") != std::string::npos);
    CHECK(md.find("## Clusters") != std::string::npos);
    CHECK(md.find("no status is changed") != std::string::npos);
    auto j = nlohmann::json::parse(slurp(out / "triage.json"));
    CHECK(j["kind"] == "prism-triage");
    CHECK(j["clusters"][0]["members"].size() == 3);
}

TEST_CASE("ai-assist triage: optional embedding model (fake) and TF-IDF similarity") {
    auto root = assist_tmp("triage-embed");
    auto report = sample_report(root / "src");
    auto fake = std::make_shared<FakeEmbedder>();
    prism::ai::set_embedder_for_testing(fake);
    auto t = prism::ai::triage(report);
    prism::ai::set_embedder_for_testing(nullptr);
    CHECK(fake->calls == 5);
    CHECK(t.embedder.find("fake-embed") != std::string::npos);
    CHECK(t.clusters[0].members.size() == 3);
    // TF-IDF: same class + function is closer than a different class.
    auto a = prism::ai::triage_text(report.stages[1].findings[0], "return x + 100;");
    auto b = prism::ai::triage_text(report.stages[2].findings[0], "return x + 100;");
    auto c = prism::ai::triage_text(report.stages[1].findings[1], "return a / b;");
    std::vector<std::string> corpus{a, b, c};
    CHECK(prism::ai::cosine_tfidf(a, b, corpus) > prism::ai::cosine_tfidf(a, c, corpus));
}

TEST_CASE("ai-assist triage: the pipeline writes triage.json and never changes statuses") {
    auto root = assist_tmp("triage-pipe");
    write(root / "src" / "a.c", "int f(int x) {\n    return x + 1;\n}\nint g(int y) {\n    return 10 / y;\n}\n");
    prism::Config cfg = prism::default_config();
    cfg.root = root / "src";
    cfg.out = root / "out";
    cfg.llm = false;
    cfg.stages = std::vector<std::string>{"inventory", "classify", "bmc"};
    auto rep = prism::run_pipeline(cfg);
    REQUIRE(fs::exists(cfg.out / "triage.json"));
    auto md = slurp(cfg.out / "report.md");
    CHECK(md.find("## Clusters") != std::string::npos);
    // Re-running triage over the saved report leaves report.json byte-identical.
    auto saved = slurp(cfg.out / "report.json");
    auto loaded = prism::RunReport::load(cfg.out / "report.json");
    REQUIRE(loaded);
    prism::ai::write_triage(*loaded, cfg.out);
    CHECK(slurp(cfg.out / "report.json") == saved);
    CHECK(loaded->dumps() == rep.dumps());
}

// ------------------------------------------------------------------ ask
TEST_CASE("ai-assist ask: keyword grammar") {
    std::string unused;
    auto q = prism::ai::parse_question("unproved memory safety in module net", &unused);
    CHECK(q.file_glob == "*net*");
    CHECK(std::find(q.cls.begin(), q.cls.end(), "MEM-") != q.cls.end());
    CHECK(std::find(q.statuses.begin(), q.statuses.end(), "BOUNDED") != q.statuses.end());
    CHECK(std::find(q.statuses.begin(), q.statuses.end(), "PROVED") == q.statuses.end());
    CHECK(unused.empty());
    q = prism::ai::parse_question("how many FAILED by stage");
    CHECK(q.count_only);
    CHECK(q.group_by == "stage");
    CHECK(q.statuses == std::vector<std::string>{"FAILED"});
    q = prism::ai::parse_question("overflows found by bmc in calc.c for function add_*");
    CHECK(q.stages == std::vector<std::string>{"bmc"});
    CHECK(q.file_glob == "*calc.c*");
    CHECK(q.function_glob == "add_*");
    CHECK(std::find(q.cls.begin(), q.cls.end(), "OVF") != q.cls.end());
    q = prism::ai::parse_question("proofs from the model checker", &unused);
    CHECK(q.stages == std::vector<std::string>{"bmc"});
    CHECK(q.statuses.size() == 4);
    q = prism::ai::parse_question("findings mentioning 'runtime error' zorblax", &unused);
    CHECK(q.text == "runtime error");
    CHECK(unused == "mentioning zorblax");
    CHECK(prism::ai::glob_match("*net/*.c", "src/net/tcp.c"));
    CHECK_FALSE(prism::ai::glob_match("*.h", "src/a.c"));
}

TEST_CASE("ai-assist ask: answers show the structured query; JSON round trip and validation") {
    auto root = assist_tmp("ask");
    auto report = sample_report(root / "src");
    auto r = prism::ai::ask_question(report, "unproved overflows", false);
    CHECK(r.translator == "grammar");
    CHECK(r.matches.size() == 3);
    CHECK(r.answer.find("query (grammar): {") != std::string::npos);
    CHECK(r.answer.find("bmc#0 FAILED calc.c:1 `add_big`") != std::string::npos);
    auto q2 = prism::ai::query_from_json(prism::ai::query_to_json(r.query));
    REQUIRE(q2);
    CHECK(prism::ai::query_to_json(*q2) == prism::ai::query_to_json(r.query));
    std::string why;
    CHECK_FALSE(prism::ai::query_from_json(R"({"stages":["nosuch"]})", &why));
    CHECK(why.find("unknown stage") != std::string::npos);
    CHECK_FALSE(prism::ai::query_from_json(R"({"statuses":["PROVED-BY-AI"]})", &why));
    CHECK_FALSE(prism::ai::query_from_json(R"({"verdict":"PROVED"})", &why));
    auto c = prism::ai::ask_question(report, "how many findings by status", false);
    CHECK(c.answer.find("  3  FAILED") != std::string::npos);
    // explain / trusted base / help
    auto e = prism::ai::assistant_reply(report, "explain pir#1", false);
    CHECK(e.find("BOUNDED") != std::string::npos);
    CHECK(e.find("never a proof") != std::string::npos);
    CHECK(prism::ai::assistant_reply(report, "explain nope#9", false).find("no finding") != std::string::npos);
    CHECK(prism::ai::assistant_reply(report, "what is the trusted base?", false).find("cake_lpr") != std::string::npos);
}

TEST_CASE("ai-assist ask: model translation is grammar-constrained, validated and audited") {
    auto root = assist_tmp("ask-model");
    auto report = sample_report(root / "src");
    prism::Config cfg = prism::default_config();
    cfg.out = root / "out";
    prism::ai::Session session(cfg);
    auto fake = std::make_shared<AssistFake>();
    fake->replies = {
        R"({"stages":["bmc"],"statuses":["FAILED"],"cls":["DIV"],"file_glob":"","function_glob":"","text":"","group_by":"","count_only":false})",
        R"({"stages":["bmc"],"statuses":["PROVED-BY-AI"]})"};
    prism::ai::set_session_backend_for_testing(fake);
    auto before = report.dumps();
    auto r = prism::ai::ask_question(report, "which division problems did model checking find", true);
    CHECK(r.translator == "llm:fake:assist-double");
    REQUIRE(r.matches.size() == 1);
    CHECK(r.matches[0].id == "bmc#1");
    CHECK(r.answer.find("query (llm:fake:assist-double)") != std::string::npos);
    REQUIRE(fake->seen.size() == 1);
    CHECK(fake->seen[0].grammar == "ask");
    CHECK(fake->seen[0].grammar_text.find("\"\\\"bmc\\\"\"") != std::string::npos);
    CHECK(fake->seen[0].user.find("<<<UNTRUSTED QUESTION") != std::string::npos);
    // Invalid model output: rejected, grammar fallback, still answered.
    auto r2 = prism::ai::ask_question(report, "failed bmc", true);
    CHECK(r2.translator == "grammar");
    CHECK(r2.model_note.find("rejected") != std::string::npos);
    CHECK(r2.matches.size() == 2);
    CHECK(report.dumps() == before);
    auto lines = lines_of(cfg.out / "ai_audit.jsonl");
    REQUIRE(lines.size() == 2);
    auto j0 = nlohmann::json::parse(lines[0]);
    auto j1 = nlohmann::json::parse(lines[1]);
    CHECK(j0["feature"] == "ask");
    CHECK(j0["checker"] == "query-validator");
    CHECK(j0["checker_result"] == "accepted");
    CHECK(j0["verdict_effect"] == "none");
    CHECK(j1["output_valid"] == false);
    CHECK(j1["checker_result"] == "rejected");
}

TEST_CASE("ai-assist ask: no model -> NOTRUN note, grammar answer") {
    auto root = assist_tmp("ask-nomodel");
    auto report = sample_report(root / "src");
    prism::Config cfg = prism::default_config();
    cfg.out = root / "out";
    cfg.llm = false;
    prism::ai::Session session(cfg);
    auto r = prism::ai::ask_question(report, "bounded results", true);
    CHECK(r.translator == "grammar");
    CHECK(r.model_note.starts_with("NOTRUN"));
    CHECK(r.matches.size() == 1);
}

// ------------------------------------------------------------------ draft
TEST_CASE("ai-assist draft: theorem index from proofs/") {
    auto root = assist_tmp("lean");
    write(root / "P.lean", "namespace Prism\ntheorem a : True := trivial\nnamespace Verdict\ntheorem b : True := "
                           "trivial\nend Verdict\nsection S\nprivate theorem c : True := trivial\nend S\nend Prism\n");
    auto t = prism::ai::lean_theorems(root);
    CHECK(t == std::vector<std::string>{"Prism.Verdict.b", "Prism.a", "Prism.c"});
    auto proofs = fs::path(__FILE__).parent_path().parent_path().parent_path() / "proofs";
    auto real = prism::ai::lean_theorems(proofs);
    CHECK(std::find(real.begin(), real.end(), "Prism.proved_bounded_never_merge") != real.end());
}

TEST_CASE("ai-assist draft: template claims all link; unlinked or unsupported claims are rejected") {
    auto root = assist_tmp("draft");
    auto report = sample_report(root / "src");
    auto proofs = fs::path(__FILE__).parent_path().parent_path().parent_path() / "proofs";
    auto thms = prism::ai::lean_theorems(proofs);
    for (std::string kind : {"report", "assurance"}) {
        auto d = prism::ai::draft_template(report, kind, thms);
        auto chk = prism::ai::validate_draft(d, report, thms);
        CHECK_MESSAGE(chk.ok, kind, ": ", (chk.rejected.empty() ? "" : chk.rejected[0]));
        CHECK(chk.claims >= 5);
        auto md = prism::ai::render_draft(d, chk);
        CHECK(md.find("[verdict:bmc#0]") != std::string::npos);
        if (kind == "assurance") CHECK(md.find("[theorem:Prism.proved_bounded_never_merge]") != std::string::npos);
    }
    // Without the theorem index the argument steps have no support: rejected, not rendered.
    auto d = prism::ai::draft_template(report, "assurance", {});
    auto chk = prism::ai::validate_draft(d, report, {});
    CHECK_FALSE(chk.ok);
    CHECK(chk.rejected.size() == 5);
    auto md = prism::ai::render_draft(d, chk);
    auto rej = md.find("## Rejected claims");
    REQUIRE(rej != std::string::npos);
    CHECK(md.find("never merged with a proof") > rej);
    // Hand-made bad claims.
    prism::ai::Draft bad;
    bad.kind = "report";
    bad.sections.push_back({"S",
                            {{"Everything is fine.", {}},
                             {"add_big is proved safe.", {"finding:pir#1"}},
                             {"ok is proved.", {"verdict:bmc#2"}},
                             {"bogus link", {"finding:bmc#99"}},
                             {"bounded is a verdict", {"verdict:fuzz#0"}},
                             {"certified!", {"verdict:bmc#2"}},
                             {"a theorem", {"theorem:Prism.no_such"}}}});
    auto c2 = prism::ai::validate_draft(bad, report, thms);
    CHECK(c2.rejected.size() == 6);  // only "ok is proved." (a PROVED-UNBOUNDED verdict) stands
}

TEST_CASE("ai-assist draft: model rewording is validated like any draft") {
    auto root = assist_tmp("draft-model");
    auto report = sample_report(root / "src");
    prism::Config cfg = prism::default_config();
    cfg.out = root / "out";
    prism::ai::Session session(cfg);
    auto fake = std::make_shared<AssistFake>();
    fake->replies = {
        R"([{"section":"Defects","text":"Adding 100 to x overflows for x = INT_MAX.","links":["verdict:bmc#0"]},
            {"section":"Defects","text":"All functions are proved correct.","links":["stage:bmc"]}])"};
    prism::ai::set_session_backend_for_testing(fake);
    std::string note;
    auto d = prism::ai::draft_with_model(report, "report", {}, &note);
    CHECK(d.author == "llm:fake:assist-double");
    auto chk = prism::ai::validate_draft(d, report, {});
    CHECK(chk.rejected.size() == 1);  // the unsupported "proved" claim
    CHECK(chk.rejected[0].find("claims a proof") != std::string::npos);
    auto lines = lines_of(cfg.out / "ai_audit.jsonl");
    REQUIRE(lines.size() == 1);
    CHECK(nlohmann::json::parse(lines[0])["checker_result"] == "partially-rejected");
}

// ------------------------------------------------------------------ predict
TEST_CASE("ai-assist predict: GBDT evaluation, off by default, enabled only by the model file") {
    auto dir = assist_tmp("predict");
    const std::string tree = R"({"base":1.0,"lr":0.5,"trees":[{"f":0,"t":2.0,"l":{"v":-2.0},"r":{"f":1,"t":0.5,"l":{"v":4.0},"r":{"v":6.0}}}]})";
    CHECK(prism::solver::predict::gbdt_eval(tree, {1.0, 0.0}) == doctest::Approx(0.0));
    CHECK(prism::solver::predict::gbdt_eval(tree, {3.0, 0.0}) == doctest::Approx(3.0));
    CHECK(prism::solver::predict::gbdt_eval(tree, {3.0, 1.0}) == doctest::Approx(4.0));
    CHECK_FALSE(prism::solver::predict::enabled(dir));
    CHECK_FALSE(prism::solver::predict::seconds(dir, "z3", {0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(prism::solver::predict::unwind_for(dir, {}, 8) == 8);
    nlohmann::json m;
    m["schema"] = 1;
    m["kind"] = "prism-gbdt";
    m["enabled"] = false;
    m["query_features"] = prism::solver::predict::query_feature_names();
    m["function_features"] = prism::solver::predict::function_feature_names();
    m["solvers"]["z3"] = nlohmann::json::parse(R"({"base":0.0,"lr":1.0,"trees":[]})");
    m["bound"] = nlohmann::json::parse(R"({"base":2.0,"lr":1.0,"trees":[]})");
    write(dir / "predict_model.json", m.dump());
    CHECK_FALSE(prism::solver::predict::enabled(dir));  // a trained model that did not beat the baseline
    m["enabled"] = true;
    write(dir / "predict_model.json", m.dump());
    // mtime granularity: force a different timestamp
    fs::last_write_time(dir / "predict_model.json", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    CHECK(prism::solver::predict::enabled(dir));
    CHECK(prism::solver::predict::seconds(dir, "z3", {0, 0, 0, 0, 0, 0, 0, 0, 0}) == doctest::Approx(1.0 - 1e-3));
    CHECK_FALSE(prism::solver::predict::seconds(dir, "kissat", {0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(prism::solver::predict::unwind_for(dir, {}, 8) == 4);
    ::setenv("PRISM_SOLVER_PREDICT", "0", 1);
    CHECK_FALSE(prism::solver::predict::enabled(dir));
    ::unsetenv("PRISM_SOLVER_PREDICT");
    // Feature names drift -> the model is refused.
    m["query_features"] = std::vector<std::string>{"other"};
    write(dir / "predict_model.json", m.dump());
    fs::last_write_time(dir / "predict_model.json", fs::file_time_type::clock::now() + std::chrono::seconds(10));
    CHECK_FALSE(prism::solver::predict::enabled(dir));
}

#ifdef PRISM_HAS_Z3
TEST_CASE("ai-assist predict: every solve logs a training line; an enabled model keeps answers") {
    auto dir = assist_tmp("predict-log");
    z3::context c;
    auto x = c.bv_const("x", 32);
    prism::solver::SolveOptions o;
    o.cache_dir = dir.string();
    o.use_cache = false;
    o.timeout_s = 10;
    auto r1 = prism::solver::solve(c, x + 1 == x, o);  // unsat
    CHECK(r1.kind == prism::solver::SolveResult::Unsat);
    auto lines = lines_of(dir / "solve_log.jsonl");
    REQUIRE(lines.size() == 1);
    auto j = nlohmann::json::parse(lines[0]);
    CHECK(j["features"].size() == prism::solver::predict::query_feature_names().size());
    CHECK(j["result"] == "unsat");
    CHECK(j["times"].contains(j["winner"].get<std::string>()));
    // A model that predicts sls to be fastest changes only the lead, not the answer.
    nlohmann::json m;
    m["schema"] = 1;
    m["kind"] = "prism-gbdt";
    m["enabled"] = true;
    m["query_features"] = prism::solver::predict::query_feature_names();
    m["solvers"]["z3"] = nlohmann::json::parse(R"({"base":3.0,"lr":1.0,"trees":[]})");
    m["solvers"]["sls"] = nlohmann::json::parse(R"({"base":-5.0,"lr":1.0,"trees":[]})");
    write(dir / "predict_model.json", m.dump());
    auto r2 = prism::solver::solve(c, x * 3 == 7, o);  // sat
    CHECK(r2.kind == prism::solver::SolveResult::Sat);
    auto r3 = prism::solver::solve(c, (x & 1) == 2, o);
    CHECK(r3.kind == prism::solver::SolveResult::Unsat);
    CHECK(lines_of(dir / "solve_log.jsonl").size() == 3);
}

// Data collection for tools/prism_ai/predict.py (roadmap 9.7 measurement).
// PRISM_PREDICT_COLLECT=<dir> ./prism_tests -tc="ai-assist predict collect*"
// Runs the conformance suite through the PIR front end: per function the
// verdict and time at unwind 1,2,4,8,16 (bound data), and per verification
// condition the time of Z3, CaDiCaL and Kissat each ALONE (solver data).
TEST_CASE("ai-assist predict collect conformance logs") {
    const char* outp = std::getenv("PRISM_PREDICT_COLLECT");
    if (!outp || !*outp) return;
    fs::path out = outp;
    fs::create_directories(out);
    auto suite = fs::path(__FILE__).parent_path().parent_path() / "conformance";
    auto cfg = prism::default_config();
    auto fe = prism::pir::find_frontend(cfg);
    REQUIRE(fe.clang);
    std::ofstream bound_log(out / "bound_runs.jsonl", std::ios::app);
    std::ofstream solve_log(out / "solve_runs.jsonl", std::ios::app);
    prism::solver::SolveOptions base;
    base.use_cache = false;
    base.timeout_s = 8.0;
    base.cache_dir = (out / "cache").string();
    auto cad = prism::solver::find_tool("cadical", base);
    auto kis = prism::solver::find_tool("kissat", base);
    double budget = std::getenv("PRISM_PREDICT_BUDGET") ? std::atof(std::getenv("PRISM_PREDICT_BUDGET")) : 1800.0;
    auto t_start = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    };
    std::vector<fs::path> srcs;
    for (auto& e : fs::recursive_directory_iterator(suite)) {
        auto ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".c" || ext == ".cpp")) srcs.push_back(e.path());
    }
    std::sort(srcs.begin(), srcs.end());
    int nfn = 0, nvc = 0;
    for (auto& src : srcs) {
        if (elapsed() > budget) break;
        std::string err;
        auto ir = prism::pir::lower_to_ir(fe, src, 30.0, err);
        if (!ir) continue;
        auto mod = prism::pir::ir::parse_module(*ir);
        for (auto& irf : mod.functions) {
            if (elapsed() > budget) break;
            auto tr = prism::pir::translate(mod, irf);
            if (!tr.fn) continue;
            auto& fn = *tr.fn;
            fn.name = irf.name;
            ++nfn;
            nlohmann::json b;
            b["file"] = fs::relative(src, suite).generic_string();
            b["function"] = irf.name;
            b["features"] = prism::solver::predict::function_features(fn);
            for (int u : {1, 2, 4, 8, 16}) {
                auto t0 = std::chrono::steady_clock::now();
                auto v = prism::pir::check_function(fn, u, 10.0);
                double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                b["runs"].push_back({{"unwind", u}, {"status", v.status}, {"seconds", s}, {"prop", v.prop}});
            }
            bound_log << b.dump() << "\n";
            auto vcs = prism::pir::pir_vcs(fn, 8);
            int k = 0;
            for (auto& vc : vcs) {
                if (++k > 6 || elapsed() > budget) break;
                z3::context c;
                z3::expr f(c);
                try {
                    auto v = c.parse_string(vc.smt2.c_str());
                    f = z3::mk_and(v);
                } catch (...) {
                    continue;
                }
                ++nvc;
                auto feats = prism::solver::features(f);
                nlohmann::json s;
                s["file"] = b["file"];
                s["function"] = irf.name;
                s["vc"] = vc.kind + ":" + vc.prop;
                s["bucket"] = feats.bucket();
                s["features"] = prism::solver::predict::query_features(feats);
                auto run = [&](const std::string& name, prism::solver::SolveOptions o) {
                    auto r = prism::solver::solve(c, f, o);
                    s["runs"][name] = {{"kind", std::string(prism::solver::kind_name(r.kind))}, {"wall_s", r.wall_s}};
                };
                auto z = base;
                z.portfolio = false;
                run("z3", z);
                for (auto* t : {&cad, &kis}) {
                    if (!*t) continue;
                    auto o = base;
                    o.z3_in_process = false;
                    o.sls = false;
                    o.search_default_tools = false;
                    std::vector<std::string> argv{(*t)->path.string()};
                    if ((*t)->name == "kissat") argv.push_back("--quiet");
                    argv.push_back("{input}");
                    o.extra_solvers = {{(*t)->name, argv, prism::solver::ExternalSolver::Input::Dimacs}};
                    run((*t)->name, o);
                }
                solve_log << s.dump() << "\n";
            }
        }
    }
    MESSAGE("collected ", nfn, " functions, ", nvc, " VCs in ", elapsed(), " s");
}
#endif

TEST_CASE("ai-assist regress: uninitialised reads get a MemorySanitizer build and a used result") {
    auto root = assist_tmp("regress-msan");
    write(root / "u.c", "int uninit_bad(int c) {\n    int x;\n    if (c) x = 1;\n    return x;\n}\n");
    prism::RunReport r;
    r.root = root.string();
    prism::FunctionInfo fn;
    fn.file = "u.c";
    fn.name = "uninit_bad";
    fn.params = {{"int", "c"}};
    r.functions.push_back(fn);
    prism::StageResult s{"bmc", "ok", "", 0, 0, {}, 0, ""};
    s.findings.push_back(mk("bmc", "FAILED", "u.c", "uninit_bad", 1, "UNINIT-READ", "uninit", "c=#x00000000"));
    r.stages = {s};
    prism::ai::RegressOptions opt;
    opt.out_dir = root / "out";
    opt.framework = "ctest";
    auto res = prism::ai::generate_regression_tests(r, opt);
    REQUIRE(res.tests.size() == 1);
    CHECK(res.tests[0].flags.find("-fsanitize=memory") != std::string::npos);
    CHECK(slurp(opt.out_dir / "u_uninit_bad.c").find("if (uninit_bad((int)0LL)) prism_regress_sink_ = 1;") !=
          std::string::npos);
    CHECK(slurp(opt.out_dir / "CMakeLists.txt").find("PRIVATE ${PRISM_MSAN_FLAGS}") != std::string::npos);
    if (std::system("clang -fsanitize=memory -x c /dev/null -c -o /dev/null >/dev/null 2>&1") == 0) {
        opt.run = true;
        opt.allow_exec = true;
        auto ran = prism::ai::generate_regression_tests(r, opt);
        CHECK_MESSAGE(ran.tests[0].status == "reproduces", ran.tests[0].detail);
    }
}
