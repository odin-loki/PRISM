// SARIF 2.1.0 export and --fail-on exit policy (src/prism/sarif.cpp).
// Port of tests/test_sarif.py: only defects (and HYPOTHESIS) are results;
// NOTRUN/failed stages are tool execution notifications.

#include <doctest/doctest.h>

#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

prism::Finding sample_f(std::string_view status, std::initializer_list<std::pair<const char*, const char*>> kw = {}) {
    prism::Finding f;
    f.stage = "lints";
    f.status = std::string(status);
    f.file = "a.c";
    f.function = "f";
    f.line = 3;
    f.cls = "MEM-UAF";
    f.message = "use after free";
    f.strength = prism::laws::STRENGTH_FINDS;
    for (auto& [k, v] : kw) {
        if (std::string(k) == "stage") f.stage = v;
        else if (std::string(k) == "cls") f.cls = v;
        else if (std::string(k) == "strength") f.strength = v;
        else if (std::string(k) == "counterexample") f.counterexample = v;
        else f.extra[k] = v;
    }
    return f;
}

prism::RunReport report_with(std::initializer_list<prism::StageResult> stages) {
    prism::RunReport r;
    r.root = (fs::path(PRISM_SOURCE_DIR) / "testdata").string();
    for (auto& s : stages) r.stages.push_back(s);
    return r;
}

nlohmann::json parse_sarif(const prism::RunReport& rep) { return nlohmann::json::parse(prism::to_sarif(rep)); }

struct PolyTree {
    fs::path dir;
    PolyTree() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("prism_sarif_" + std::to_string(rd()));
        fs::create_directories(dir);
    }
    void put(const std::string& rel, const std::string& text) {
        auto p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
    ~PolyTree() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("sarif: defects are results; CLEAN/PROVED/BOUNDED are omitted") {
    prism::StageResult lints;
    lints.name = "lints";
    lints.status = "ok";
    lints.findings = {
        sample_f(prism::laws::FAILED),
        sample_f(prism::laws::CRASH, {{"stage", "fuzz"}, {"cls", ""}, {"counterexample", "n=0"}}),
        sample_f(prism::laws::CLEAN, {{"cls", ""}}),
        sample_f(prism::laws::PROVED, {{"stage", "bmc"}, {"cls", ""}}),
        sample_f(prism::laws::BOUNDED, {{"stage", "bmc"}, {"cls", ""}}),
        sample_f(prism::laws::HYPOTHESIS,
                 {{"stage", "llm"}, {"cls", "INTENT"}, {"strength", "READS"}}),
    };
    auto doc = parse_sarif(report_with({lints}));
    CHECK(doc["version"] == "2.1.0");
    auto& run = doc["runs"][0];
    CHECK(run["tool"]["driver"]["name"] == "PRISM");
    CHECK(run["tool"]["driver"]["version"] == prism::PRISM_VERSION);
    auto& res = run["results"];
    REQUIRE(res.size() == 3);
    CHECK(res[0]["properties"]["status"] == prism::laws::FAILED);
    CHECK(res[1]["properties"]["status"] == prism::laws::CRASH);
    CHECK(res[2]["properties"]["status"] == prism::laws::HYPOTHESIS);
    CHECK(res[0]["level"] == "error");
    CHECK(res[1]["level"] == "error");
    CHECK(res[2]["level"] == "note");
    CHECK(res[1]["ruleId"] == "fuzz/CRASH");
    CHECK(res[1]["message"]["text"].get<std::string>().find("counterexample: n=0") != std::string::npos);
    CHECK(res[2]["properties"]["hypothesis"] == true);
    auto& loc = res[0]["locations"][0]["physicalLocation"];
    CHECK(loc["artifactLocation"]["uri"] == "a.c");
    CHECK(loc["artifactLocation"]["uriBaseId"] == "SRCROOT");
    CHECK(loc["region"]["startLine"] == 3);
    CHECK(run["originalUriBaseIds"]["SRCROOT"]["uri"].get<std::string>().rfind("file://", 0) == 0);
    std::vector<std::string> rule_ids;
    for (auto& r : run["tool"]["driver"]["rules"]) rule_ids.push_back(r["id"].get<std::string>());
    std::sort(rule_ids.begin(), rule_ids.end());
    CHECK(rule_ids == std::vector<std::string>{"INTENT", "MEM-UAF", "fuzz/CRASH"});
}

TEST_CASE("sarif: warning severity and STRENGTH_SOME map to warning level") {
    prism::StageResult s;
    s.name = "polyglot";
    s.status = "ok";
    s.findings = {sample_f(prism::laws::FAILED, {{"severity", "warning"}}),
                  sample_f(prism::laws::FAILED, {{"strength", "SOME"}})};
    auto doc = parse_sarif(report_with({s}));
    auto& res = doc["runs"][0]["results"];
    REQUIRE(res.size() == 2);
    CHECK(res[0]["level"] == "warning");
    CHECK(res[1]["level"] == "warning");
}

TEST_CASE("sarif: NOTRUN and failed stages become tool execution notifications") {
    prism::StageResult esbmc;
    esbmc.name = "esbmc";
    esbmc.status = "NOTRUN";
    esbmc.detail = "esbmc not found";
    esbmc.install = "apt install esbmc";
    prism::StageResult bmc;
    bmc.name = "bmc";
    bmc.status = "failed";
    bmc.detail = "boom";
    prism::StageResult lints;
    lints.name = "lints";
    lints.status = "ok";
    auto doc = parse_sarif(report_with({esbmc, bmc, lints}));
    auto& inv = doc["runs"][0]["invocations"][0];
    CHECK(inv["executionSuccessful"] == false);
    auto& notes = inv["toolExecutionNotifications"];
    REQUIRE(notes.size() == 2);
    CHECK(notes[0]["level"] == "warning");
    CHECK(notes[1]["level"] == "error");
    CHECK(notes[0]["message"]["text"] == "esbmc NOTRUN: esbmc not found");
}

TEST_CASE("sarif: exit_code matrix for never, defect, and gap") {
    prism::StageResult clean_s;
    clean_s.name = "lints";
    clean_s.status = "ok";
    clean_s.findings = {sample_f(prism::laws::CLEAN)};
    auto clean = report_with({clean_s});
    prism::StageResult defect_s;
    defect_s.name = "lints";
    defect_s.status = "ok";
    defect_s.findings = {sample_f(prism::laws::FAILED)};
    auto defect = report_with({defect_s});
    prism::StageResult gap_s;
    gap_s.name = "esbmc";
    gap_s.status = "NOTRUN";
    gap_s.findings = {sample_f(prism::laws::NOTRUN)};
    auto gap = report_with({gap_s});
    prism::StageResult crashed;
    crashed.name = "bmc";
    crashed.status = "failed";
    auto crashed_rep = report_with({crashed});

    struct Row {
        prism::RunReport rep;
        int never, defect, gap;
    };
    for (auto& row : std::vector<Row>{{clean, 0, 0, 0},
                                      {defect, 0, 1, 1},
                                      {gap, 0, 0, 1},
                                      {crashed_rep, 2, 2, 2}}) {
        CHECK(prism::exit_code(row.rep, "never") == row.never);
        CHECK(prism::exit_code(row.rep, "defect") == row.defect);
        CHECK(prism::exit_code(row.rep, "gap") == row.gap);
    }
}

TEST_CASE("sarif: warning/note/style severity does not trip --fail-on") {
    for (auto* sev : {"warning", "note", "style", "WARNING"}) {
        prism::StageResult s;
        s.name = "warnings";
        s.status = "ok";
        s.findings = {sample_f(prism::laws::FAILED, {{"severity", sev}})};
        auto rep = report_with({s});
        CHECK(prism::exit_code(rep, "defect") == 0);
        CHECK(prism::exit_code(rep, "gap") == 0);
    }
    prism::StageResult err;
    err.name = "warnings";
    err.status = "ok";
    err.findings = {sample_f(prism::laws::FAILED, {{"severity", "error"}})};
    CHECK(prism::exit_code(report_with({err}), "defect") == 1);
}

TEST_CASE("sarif: polyglot pipeline emits expected defect results") {
    PolyTree t;
    t.put("bad.py", "def f(:\n    pass\n");
    t.put("bad.json", "{\"a\": 1,}\n");
    t.put("a.c", "<<<<<<< HEAD\nint x;\n>>>>>>> b\n");
    t.put("k.ini", "aws = AKIAABCDEFGHIJKLMNOP\n");  // prism:allow
    t.put("s.sh", "if then\nfi\n");
    t.put("id_rsa", "-----BEGIN OPENSSH PRIVATE KEY-----\n");  // prism:allow
    t.put("key.pem", "-----BEGIN PRIVATE KEY-----\n");         // prism:allow
    t.put("lint.py", "import os\n");

    auto cfg = prism::default_config();
    cfg.root = t.dir;
    cfg.out = t.dir / "out";
    cfg.llm = false;
    cfg.stages = std::vector<std::string>{"polyglot"};
    auto rep = prism::run_pipeline(cfg);
    prism::write_sarif(rep, cfg.out / "report.sarif");

    auto doc = nlohmann::json::parse(std::ifstream(cfg.out / "report.sarif"));
    auto& run = doc["runs"][0];
    struct Row {
        std::string rule;
        std::string level;
        std::string msg;
        std::string uri;
        int line;
    };
    std::vector<Row> rows;
    for (auto& r : run["results"]) {
        std::string msg = r["message"]["text"];
        if (msg.find("prism-syntax:") != std::string::npos || msg.find("prism-json:") != std::string::npos ||
            msg.find("python:") != std::string::npos) {
            auto uri = r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"].get<std::string>();
            CHECK((uri == "bad.json" || uri == "bad.py"));
            continue;
        }
        rows.push_back({r["ruleId"],
                        r["level"],
                        msg,
                        r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"].get<std::string>(),
                        r["locations"][0]["physicalLocation"]["region"].value("startLine", 0)});
    }
    CHECK_FALSE(rows.empty());
    std::set<std::string> rule_ids;
    std::set<std::string> uris;
    for (auto& row : rows) {
        rule_ids.insert(row.rule);
        uris.insert(row.uri);
    }
    CHECK(rule_ids.contains("VCS-CONFLICT-MARKER"));
    CHECK(uris.contains("id_rsa"));
    CHECK(uris.contains("key.pem"));
    for (auto& n : run["invocations"][0]["toolExecutionNotifications"])
        CHECK(n["message"]["text"].get<std::string>().find("NOTRUN") == std::string::npos);
}
