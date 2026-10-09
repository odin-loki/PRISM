// Honest taxonomy coverage (prism/taxonomy.cpp). Port of tests/test_taxonomy.py.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/pipeline.hpp"
#include "prism/taxonomy.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

prism::RunReport rep(std::initializer_list<prism::StageResult> stages) {
    prism::RunReport r;
    r.root = "x";
    for (auto s : stages) r.stages.push_back(s);
    return r;
}

prism::StageResult stage(const std::string& name, const std::string& status,
                         std::initializer_list<prism::Finding> findings = {}) {
    prism::StageResult s;
    s.name = name;
    s.status = status;
    s.findings.assign(findings.begin(), findings.end());
    s.records = static_cast<int>(s.findings.size());
    return s;
}

prism::Finding f(const std::string& stg, const char* status, const char* cls, const char* strength,
                 const char* stg_name_override = nullptr) {
    prism::Finding x;
    x.stage = stg_name_override ? stg_name_override : stg;
    x.status = status;
    x.file = "a.c";
    x.function = "fn";
    x.line = 1;
    x.cls = cls;
    x.message = "m";
    x.strength = strength;
    return x;
}

std::map<std::string, prism::TaxonomyRow> rows_map(const prism::RunReport& r) {
    std::map<std::string, prism::TaxonomyRow> m;
    for (auto& row : prism::coverage_from_report(r)) m[row.id] = row;
    return m;
}

}  // namespace

TEST_CASE("taxonomy: CRASH cls is COVERED; HYPOTHESIS and READS are not") {
    auto m = rows_map(rep({stage("fuzz", "ok", {f("fuzz", prism::laws::CRASH, "INT-SIGNED-OVF", prism::laws::STRENGTH_FINDS)})}));
    CHECK(m["INT-SIGNED-OVF"].verdict == "COVERED");
    m = rows_map(rep({stage("llm", "ok", {f("llm", prism::laws::HYPOTHESIS, "INTENT", prism::laws::STRENGTH_READS)})}));
    CHECK(m["INTENT"].verdict == "PARTIAL");
    CHECK(m["INTENT"].verdict != "COVERED");
    m = rows_map(rep({stage("ltl", "ok",
                            {f("ltl", prism::laws::FAILED, "LTL-SAFETY", prism::laws::STRENGTH_READS)})}));
    CHECK(m["LTL-SAFETY"].best == prism::laws::STRENGTH_READS);
    CHECK(m["LTL-SAFETY"].verdict == "PARTIAL");
}

TEST_CASE("taxonomy: wp PROVED-ASSUMING covers FUNC-CONTRACT; empty cls on wp too") {
    auto m = rows_map(rep({stage("wp", "ok", {f("wp", prism::laws::PROVED_ASSUMING, "", prism::laws::STRENGTH_PROVES)})}));
    CHECK(m["FUNC-CONTRACT"].verdict == "COVERED");
    CHECK(m["INT-SIGNED-OVF"].verdict != "COVERED");
    m = rows_map(rep({stage("wp", "ok",
                            {f("wp", prism::laws::PROVED_ASSUMING, "FUNC-CONTRACT", prism::laws::STRENGTH_PROVES)})}));
    CHECK(m["FUNC-CONTRACT"].verdict == "COVERED");
    CHECK(m["FUNC-CONTRACT"].verdict != "GAP");
}

TEST_CASE("taxonomy: C++ does not promote READS to FINDS") {
    const auto cpp = slurp(repo() / "src" / "prism" / "taxonomy.cpp");
    const auto gen = slurp(repo() / "tools" / "gen_prism.py");
    CHECK(cpp.find("if (st == READS) st = FINDS") == std::string::npos);
    CHECK(gen.find("if (st == READS) st = FINDS") == std::string::npos);
    CHECK(cpp.find(R"(cls.empty() && (s.name == "wp" || s.name == "contracts"))") != std::string::npos);
}

TEST_CASE("taxonomy: lints INTENT FINDS is COVERED; skipped stage is GAP") {
    auto m = rows_map(rep({stage("lints", "ok",
                                {f("lints", prism::laws::FAILED, "INTENT", prism::laws::STRENGTH_FINDS, "lints")})}));
    CHECK(m["INTENT"].verdict == "COVERED");
    CHECK(m["INTENT"].best == prism::laws::STRENGTH_FINDS);
    m = rows_map(rep({stage("taint", "skipped")}));
    CHECK(m["TAINT-SINK"].verdict == "GAP");
}

TEST_CASE("taxonomy: breadth classes exist and empty report marks them GAP") {
    static const char* kSample[] = {"MEM-MEMSET-SWAP", "INT-TAUTOLOGY", "EMPTY-TU", "CTRL-FALLTHROUGH",
                                    "CXX-USE-AFTER-MOVE", "LOCK-ORDER", "CRYPTO-MISUSE", "API-GETDENTS",
                                    "CXX-CONTRACTS", "API-PTHREAD-JOIN", "STR-STRNCPY-NUL"};
    auto all = rows_map(prism::RunReport{});
    CHECK(prism::taxonomy_classes().size() >= 500);
    for (const char* cid : kSample) {
        CHECK(all.count(cid) == 1);
        CHECK(all[cid].verdict == "GAP");
    }
}

TEST_CASE("taxonomy: inventory EMPTY-TU ERROR is COVERED") {
    prism::Finding inv;
    inv.stage = "inventory";
    inv.status = prism::laws::ERROR;
    inv.file = "empty_tu.c";
    inv.cls = "EMPTY-TU";
    inv.message = "no functions parsed (not a clean unit)";
    inv.strength = prism::laws::STRENGTH_FINDS;
    auto m = rows_map(rep({stage("inventory", "ok", {inv})}));
    CHECK(m["EMPTY-TU"].verdict == "COVERED");
}

TEST_CASE("taxonomy: llm-only hypothesis never COVERED") {
    auto rows = prism::coverage_from_report(
        rep({stage("llm", "ok", {f("llm", prism::laws::HYPOTHESIS, "FUNC-CONTRACT", prism::laws::STRENGTH_READS)})}));
    std::vector<std::string> covered;
    for (auto& r : rows) {
        if (r.id == "FUNC-CONTRACT" || r.id == "INTENT")
            CHECK((r.verdict == "GAP" || r.verdict == "PARTIAL"));
        if (r.verdict == "COVERED") covered.push_back(r.id);
    }
    CHECK(covered.empty());
}

TEST_CASE("taxonomy: empty scope confidence is zero") {
    prism::RunReport r;
    prism::apply_confidence(r);
    CHECK(r.visibility == 0.0);
    CHECK(r.answer == 0.0);
    CHECK(r.resolution == 0.0);
    CHECK(r.confidence == 0.0);
}

TEST_CASE("taxonomy: unify CLEAN is not a proof and spoofed cls does not COVER") {
    const auto unify_cpp = slurp(repo() / "src" / "prism" / "pipeline.cpp");
    auto u = unify_cpp.substr(unify_cpp.find(R"(stage("unify")"));
    u = u.substr(0, u.find("apply_confidence"));
    CHECK(u.find("not a proof") != std::string::npos);
    CHECK(u.find(R"(f.extra["not_a_proof"] = "true")") != std::string::npos);

    prism::Finding clean;
    clean.stage = "unify";
    clean.status = prism::laws::CLEAN;
    clean.cls = "INT-SIGNED-OVF";
    clean.message = "taxonomy 1/1 COVERED, 0 GAP (not a proof)";
    clean.strength = prism::laws::STRENGTH_FINDS;
    clean.extra["not_a_proof"] = "true";
    auto m = rows_map(rep({stage("unify", "ok", {clean})}));
    CHECK(m["INT-SIGNED-OVF"].verdict != "COVERED");
    CHECK_FALSE(prism::laws::is_proof(clean.status));
}

TEST_CASE("taxonomy: unify stage end-to-end carries not_a_proof") {
    std::random_device rd;
    auto dir = fs::temp_directory_path() / ("prism_tax_unify_" + std::to_string(rd()));
    fs::create_directories(dir);
    auto out = dir / "out";
    prism::Config cfg = prism::default_config();
    cfg.root = dir;
    cfg.out = out;
    cfg.llm = false;
    cfg.stages = {"unify"};
    cfg.skip = {};
    auto report = prism::run_pipeline(cfg);
    const prism::StageResult* uni = nullptr;
    for (auto& s : report.stages)
        if (s.name == "unify") uni = &s;
    REQUIRE(uni != nullptr);
    REQUIRE_FALSE(uni->findings.empty());
    CHECK(uni->findings[0].message.find("not a proof") != std::string::npos);
    CHECK(uni->findings[0].status == std::string(prism::laws::CLEAN));
    CHECK(uni->findings[0].extra["not_a_proof"] == "true");
    std::error_code ec;
    fs::remove_all(dir, ec);
}
