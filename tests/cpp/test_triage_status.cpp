// Triage orders findings. It writes triage.json and a Clusters section of
// report.md (src/prism/ai/triage.cpp). Finding::status stays as recorded:
// HYPOTHESIS stays HYPOTHESIS, NOTRUN stays NOTRUN, and every finding stays
// on the report. PROVED and NOTRUN are outside the defect set that triage
// clusters; the cluster text prints HYPOTHESIS and BOUNDED as themselves.
// The deterministic TF-IDF path needs no embedding model.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/ai_assist.hpp"
#include "prism/laws.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

prism::Finding row(std::string stage, std::string_view status, std::string file, std::string function,
                   std::string cls, std::string message) {
    prism::Finding f;
    f.stage = std::move(stage);
    f.status = std::string(status);
    f.file = std::move(file);
    f.function = std::move(function);
    f.line = 1;
    f.cls = std::move(cls);
    f.message = std::move(message);
    return f;
}

struct OutDir {
    fs::path path;
    OutDir() {
        path = fs::temp_directory_path() / "prism-triage-status";
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~OutDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<std::string> backtick_tokens(const std::string& line) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < line.size();) {
        const auto a = line.find('`', i);
        if (a == std::string::npos) break;
        const auto b = line.find('`', a + 1);
        if (b == std::string::npos) break;
        out.push_back(line.substr(a + 1, b - a - 1));
        i = b + 1;
    }
    return out;
}

}  // namespace

TEST_CASE("triage: PROVED, HYPOTHESIS, NOTRUN, and BOUNDED keep their statuses") {
    prism::RunReport report;
    report.root = "src";
    prism::StageResult bmc;
    bmc.name = "bmc";
    bmc.status = "ok";
    bmc.findings.push_back(row("bmc", prism::laws::PROVED, "proved.c", "proved_fn", "OVF", "addition stays in range"));
    bmc.findings.push_back(row("bmc", prism::laws::HYPOTHESIS, "hyp.c", "hyp_fn", "MEM-UAF", "maybe freed"));
    bmc.findings.push_back(row("bmc", prism::laws::NOTRUN, "notrun.c", "notrun_fn", "CRASH", "fuzzer absent"));
    bmc.findings.push_back(row("bmc", prism::laws::BOUNDED, "bounded.c", "bounded_fn", "MEM-OOB", "index checked to a bound"));
    report.stages.push_back(std::move(bmc));

    const auto before = report.dumps();
    REQUIRE(report.stages.size() == 1);
    REQUIRE(report.stages[0].findings.size() == 4);
    const auto& findings = report.stages[0].findings;
    REQUIRE(findings[0].status == prism::laws::PROVED);
    REQUIRE(findings[1].status == prism::laws::HYPOTHESIS);
    REQUIRE(findings[2].status == prism::laws::NOTRUN);
    REQUIRE(findings[3].status == prism::laws::BOUNDED);

    OutDir out;
    const std::string head =
        "# Report\n\n## Findings\n\n"
        "- bmc#0 status PROVED\n"
        "- bmc#1 status HYPOTHESIS\n"
        "- bmc#2 status NOTRUN\n"
        "- bmc#3 status BOUNDED\n";
    {
        std::ofstream md(out.path / "report.md", std::ios::binary);
        md << head;
    }

    // Public entry. use_embedder false is the TF-IDF core: clusters are real,
    // and a missing model is not turned into a finding status.
    prism::ai::TriageOptions opt;
    opt.use_embedder = false;
    prism::ai::write_triage(report, out.path, opt);

    CHECK(report.dumps() == before);
    REQUIRE(report.stages.size() == 1);
    REQUIRE(report.stages[0].findings.size() == 4);
    CHECK(report.stages[0].findings[0].status == prism::laws::PROVED);
    CHECK(report.stages[0].findings[1].status == prism::laws::HYPOTHESIS);
    CHECK(report.stages[0].findings[2].status == prism::laws::NOTRUN);
    CHECK(report.stages[0].findings[3].status == prism::laws::BOUNDED);

    const auto md = slurp(out.path / "report.md");
    REQUIRE(md.starts_with(head));
    CHECK(md.find("## Clusters") != std::string::npos);
    CHECK(md.find("no status is changed") != std::string::npos);

    int cluster_lines = 0;
    bool saw_hypothesis = false;
    bool saw_bounded = false;
    std::size_t pos = 0;
    while (pos < md.size()) {
        auto nl = md.find('\n', pos);
        if (nl == std::string::npos) nl = md.size();
        const auto line = md.substr(pos, nl - pos);
        pos = nl < md.size() ? nl + 1 : md.size();
        if (line.find("**C") == std::string::npos) continue;
        ++cluster_lines;
        const auto tokens = backtick_tokens(line);
        REQUIRE(tokens.size() >= 2);
        const auto& shown = tokens[0];
        const auto& id = tokens.back();
        CHECK(id != "bmc#0");
        CHECK(id != "bmc#2");
        if (id == "bmc#1") {
            CHECK(shown == std::string(prism::laws::HYPOTHESIS));
            saw_hypothesis = true;
        } else if (id == "bmc#3") {
            CHECK(shown == std::string(prism::laws::BOUNDED));
            saw_bounded = true;
        } else {
            FAIL("unexpected cluster member " << id);
        }
    }
    CHECK(cluster_lines == 2);
    CHECK(saw_hypothesis);
    CHECK(saw_bounded);
    // The seeded Findings section names PROVED and NOTRUN in prose. The
    // cluster lines above are the only status tokens in backticks.
    CHECK(md.find("`PROVED`") == std::string::npos);
    CHECK(md.find("`CLEAN`") == std::string::npos);
    CHECK(md.find("`NOTRUN`") == std::string::npos);
    CHECK(md.find("`bmc#0`") == std::string::npos);
    CHECK(md.find("`bmc#2`") == std::string::npos);

    const auto j = nlohmann::json::parse(slurp(out.path / "triage.json"));
    CHECK(j["kind"] == "prism-triage");
    CHECK(j["considered"] == 2);
    REQUIRE(j["clusters"].size() == 2);
    bool json_hypothesis = false;
    bool json_bounded = false;
    for (const auto& c : j["clusters"]) {
        const auto top = c["top_status"].get<std::string>();
        REQUIRE(c["members"].size() == 1);
        const auto id = c["members"][0].get<std::string>();
        CHECK(c["representative"] == id);
        if (id == "bmc#1") {
            CHECK(top == std::string(prism::laws::HYPOTHESIS));
            json_hypothesis = true;
        } else if (id == "bmc#3") {
            CHECK(top == std::string(prism::laws::BOUNDED));
            json_bounded = true;
        } else {
            FAIL("unexpected cluster member " << id);
        }
    }
    CHECK(json_hypothesis);
    CHECK(json_bounded);
    CHECK(j["embedder_note"].get<std::string>().find("TF-IDF") != std::string::npos);
}
