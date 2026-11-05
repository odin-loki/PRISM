// report.md (src/prism/pipeline.cpp write_report_md). Law 2: PROVED and
// BOUNDED are printed as themselves. TIMEOUT and UNKNOWN stay. Law 7: a
// NOTRUN finding is written down, except the inventory/classify/unify
// NOTRUN and CLEAN lines, which are dropped on purpose.

#include <doctest/doctest.h>

#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

struct TmpDir {
    fs::path dir;
    TmpDir() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("prism_report_md_" + std::to_string(rd()));
        fs::create_directories(dir);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

prism::Finding finding(std::string_view status, std::string message) {
    prism::Finding f;
    f.status = std::string(status);
    f.message = std::move(message);
    return f;
}

}  // namespace

TEST_CASE("write_report_md keeps PROVED and BOUNDED and drops inventory NOTRUN") {
    TmpDir tmp;
    prism::RunReport r;
    r.root = "x";

    prism::StageResult bmc;
    bmc.name = "bmc";
    bmc.status = "ok";
    bmc.findings = {
        finding(prism::laws::PROVED, "holds"),
        finding(prism::laws::BOUNDED, "within k"),
        finding(prism::laws::TIMEOUT, "solver timed out"),
        finding(prism::laws::UNKNOWN, "no answer"),
        finding(prism::laws::NOTRUN, "bmc did not run"),
    };

    prism::StageResult inventory;
    inventory.name = "inventory";
    inventory.status = "ok";
    inventory.findings = {finding(prism::laws::NOTRUN, "inventory did not run")};

    r.stages = {bmc, inventory};

    auto md = tmp.dir / "report.md";
    prism::write_report_md(r, md);
    std::ifstream in(md, std::ios::binary);
    REQUIRE(in);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    CHECK(body.find("- `PROVED` **bmc**") != std::string::npos);
    CHECK(body.find("- `BOUNDED` **bmc**") != std::string::npos);
    CHECK(body.find("- `TIMEOUT` **bmc**") != std::string::npos);
    CHECK(body.find("- `UNKNOWN` **bmc**") != std::string::npos);
    CHECK(body.find("- `NOTRUN` **bmc**") != std::string::npos);
    CHECK(body.find("bmc did not run") != std::string::npos);
    CHECK(body.find("- `NOTRUN` **inventory**") == std::string::npos);
    CHECK(body.find("inventory did not run") == std::string::npos);
    CHECK(body.find("0 means no data, not clean") != std::string::npos);
}

TEST_CASE("write_report_md leaves classify kind rows out of Findings; report.json keeps them") {
    // cJSON: 153 classify rows (SCALAR/POINTER/VOID/OTHER) dominated the list.
    TmpDir tmp;
    prism::RunReport r;
    r.root = "x";
    prism::StageResult classify;
    classify.name = "classify";
    classify.status = "ok";
    auto kind = [](const char* k, const char* fn) {
        prism::Finding f = finding(k, std::string("int ") + fn + "(int *p)");
        f.stage = "classify";
        f.cls = k;
        f.function = fn;
        f.file = "a.c";
        return f;
    };
    classify.findings = {kind("POINTER", "walk"), kind("POINTER", "put"), kind("SCALAR", "add"),
                         finding(prism::laws::ERROR, "b.c: parse failed")};
    prism::StageResult pir;
    pir.name = "pir";
    pir.status = "ok";
    pir.findings = {finding(prism::laws::PROVED, "holds")};
    r.stages = {classify, pir};

    auto md = tmp.dir / "report.md";
    prism::write_report_md(r, md);
    std::ifstream in(md, std::ios::binary);
    REQUIRE(in);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto findings = body.substr(body.find("## Findings"));
    CHECK(findings.find("`POINTER`") == std::string::npos);
    CHECK(findings.find("`SCALAR`") == std::string::npos);
    CHECK(findings.find("int walk(int *p)") == std::string::npos);
    CHECK(findings.find("Function classification: 3 functions (2 POINTER, 1 SCALAR)") != std::string::npos);
    // a classify row with a verdict is a finding
    CHECK(findings.find("- `ERROR` **classify** — b.c: parse failed") != std::string::npos);
    CHECK(findings.find("- `PROVED` **pir**") != std::string::npos);

    // report.json keeps every classify row
    auto js = tmp.dir / "report.json";
    r.save(js);
    std::ifstream jin(js, std::ios::binary);
    std::string jbody((std::istreambuf_iterator<char>(jin)), std::istreambuf_iterator<char>());
    CHECK(jbody.find("\"POINTER\"") != std::string::npos);
    CHECK(jbody.find("int walk(int *p)") != std::string::npos);
    CHECK(jbody.find("int add(int *p)") != std::string::npos);
}
