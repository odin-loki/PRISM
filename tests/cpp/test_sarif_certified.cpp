// PROVED-CERTIFIED is not a SARIF result. to_sarif counts it on the run
// and still emits FAILED. docs/finish-notes/sarif-certified.md

#include <doctest/doctest.h>

#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace {

prism::Finding finding(std::string_view status, std::string message, std::string cls) {
    prism::Finding f;
    f.stage = status == prism::laws::PROVED_CERTIFIED ? "pir" : "lints";
    f.status = std::string(status);
    f.file = status == prism::laws::PROVED_CERTIFIED ? "ok.c" : "a.c";
    f.function = status == prism::laws::PROVED_CERTIFIED ? "main" : "f";
    f.line = status == prism::laws::PROVED_CERTIFIED ? 1 : 3;
    f.cls = std::move(cls);
    f.message = std::move(message);
    f.strength = status == prism::laws::PROVED_CERTIFIED ? std::string(prism::laws::STRENGTH_PROVES)
                                                         : std::string(prism::laws::STRENGTH_FINDS);
    return f;
}

}  // namespace

TEST_CASE("sarif: PROVED-CERTIFIED is counted and FAILED stays a result") {
    prism::StageResult stage;
    stage.name = "pir";
    stage.status = "ok";
    stage.findings = {
        finding(prism::laws::PROVED_CERTIFIED, "checked certificate", ""),
        finding(prism::laws::FAILED, "use after free", "MEM-UAF"),
    };
    prism::RunReport report;
    report.stages.push_back(std::move(stage));

    const auto doc = nlohmann::json::parse(prism::to_sarif(report));
    const auto& run = doc.at("runs").at(0);
    const auto& results = run.at("results");
    REQUIRE(results.is_array());

    const std::string certified(prism::laws::PROVED_CERTIFIED);
    bool saw_failed = false;
    for (const auto& res : results) {
        const std::string level = res.at("level").get<std::string>();
        const std::string text = res.at("message").at("text").get<std::string>();
        std::string status;
        if (res.contains("properties") && res["properties"].contains("status") &&
            res["properties"]["status"].is_string())
            status = res["properties"]["status"].get<std::string>();

        const bool about_certified = status == certified || text.find(certified) != std::string::npos ||
                                      text.find("checked certificate") != std::string::npos;
        const bool defect_level = level == "error" || level == "warning";
        // Message or level must not present the certificate-checked proof as a defect.
        CHECK_FALSE((about_certified && defect_level));
        CHECK_FALSE(about_certified);

        if (status == std::string(prism::laws::FAILED)) {
            saw_failed = true;
            CHECK(level == "error");
            CHECK(text.find("use after free") != std::string::npos);
        }
    }
    CHECK(saw_failed);

    if (run.contains("properties") && run["properties"].contains("certified"))
        CHECK(run["properties"]["certified"].get<int>() == 1);
}
