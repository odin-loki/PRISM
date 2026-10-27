// Finding and RunReport JSON (src/prism/models.cpp). Status words are copied
// as text: BOUNDED stays BOUNDED, NOTRUN stays NOTRUN, HYPOTHESIS stays
// HYPOTHESIS, PROVED stays PROVED. A byte that is not UTF-8 inside a message
// is still a report; load replaces that byte with U+FFFD.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/models.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

prism::Finding row(std::string_view status, std::string message) {
    prism::Finding f;
    f.stage = "bmc";
    f.status = std::string(status);
    f.file = "a.c";
    f.function = "f";
    f.line = 1;
    f.cls = "X";
    f.message = std::move(message);
    return f;
}

struct OutDir {
    fs::path path;
    OutDir() {
        path = fs::temp_directory_path() / "prism-finding-roundtrip";
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~OutDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

void check_statuses(const prism::RunReport& rep) {
    REQUIRE(rep.stages.size() == 1);
    const auto& stage = rep.stages[0];
    CHECK(stage.status == prism::laws::CLEAN);
    REQUIRE(stage.findings.size() == 4);
    CHECK(stage.findings[0].status == prism::laws::BOUNDED);
    CHECK(stage.findings[0].status != prism::laws::PROVED);
    CHECK(stage.findings[1].status == prism::laws::NOTRUN);
    CHECK(stage.findings[1].status != prism::laws::CLEAN);
    CHECK(stage.findings[2].status == prism::laws::HYPOTHESIS);
    CHECK(stage.findings[2].status != prism::laws::PROVED);
    CHECK(stage.findings[3].status == prism::laws::PROVED);
    CHECK(stage.findings[3].status != prism::laws::BOUNDED);
    CHECK(stage.findings[3].status != prism::laws::HYPOTHESIS);
}

}  // namespace

TEST_CASE("finding roundtrip: BOUNDED, NOTRUN, HYPOTHESIS, and PROVED stay distinct") {
    prism::RunReport report;
    report.root = "src";
    prism::StageResult stage;
    stage.name = "bmc";
    // CLEAN on the stage, NOTRUN on a finding: the two words must not trade places.
    stage.status = std::string(prism::laws::CLEAN);
    stage.findings.push_back(row(prism::laws::BOUNDED, "bounded row"));
    stage.findings.push_back(row(prism::laws::NOTRUN, "notrun row"));
    stage.findings.push_back(row(prism::laws::HYPOTHESIS, "hypothesis row"));
    stage.findings.push_back(row(prism::laws::PROVED, "proved row"));
    report.stages.push_back(std::move(stage));

    const std::string dumped = report.dumps();
    OutDir out;
    const auto clean_path = out.path / "report.json";
    write_bytes(clean_path, dumped);
    std::optional<prism::RunReport> clean;
    CHECK_NOTHROW(clean = prism::RunReport::load(clean_path));
    REQUIRE(clean);
    check_statuses(*clean);

    // 0xFF is not UTF-8. load replaces it with U+FFFD and still returns the report.
    const auto at = dumped.find("bounded row");
    REQUIRE(at != std::string::npos);
    std::string raw = dumped;
    raw.insert(at + std::string("bounded row").size(), 1, static_cast<char>(0xFF));
    const auto bad_path = out.path / "bad.json";
    write_bytes(bad_path, raw);
    std::optional<prism::RunReport> loaded;
    CHECK_NOTHROW(loaded = prism::RunReport::load(bad_path));
    REQUIRE(loaded);
    check_statuses(*loaded);
    const auto& msg = loaded->stages[0].findings[0].message;
    CHECK(msg.find("bounded row") != std::string::npos);
    CHECK(msg.find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(msg.find('\xff') == std::string::npos);
}
