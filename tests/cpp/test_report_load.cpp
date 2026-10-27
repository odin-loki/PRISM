// report.json load tolerance (src/prism/models.cpp). A numeric string in an
// int field and a non-UTF-8 byte in a message still produce a report.
// finding_from_json_object / stage_from_json_object / function_from_json_object
// stay strict: a non-object, or a byte the JSON parser rejects, throws.
// Resume does not use those loaders for functions.json; a wrong-typed
// function there still drops the whole list (journal.cpp, Law 7).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/models.hpp"
#include "prism/models_json.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

fs::path write_bytes(const std::string& name, const std::string& bytes) {
    auto dir = fs::temp_directory_path() / "prism_report_load";
    fs::create_directories(dir);
    auto p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out << bytes;
    return p;
}

}  // namespace

TEST_CASE("report json: load keeps a string line and a non-UTF-8 message") {
    // "line":"12" is an int field written as a string. 0xFF is not UTF-8.
    // file:7 is a string field written as a number. "nope" is not an int.
    // records:"1" must stay 1, not fall through to the finding count.
    std::string body = R"({"root":"r","notes":["caf)";
    body += "\xc3\xa9";
    body += R"("],"functions":[{"file":"a.c","name":"f","line":"12"}],)"
            R"("stages":[{"name":"bmc","status":"ok","records":"1","findings":[)"
            R"({"stage":"bmc","status":"FAILED","file":7,"line":"12","cls":"X","message":"bad )";
    body.push_back(static_cast<char>(0xFF));
    body += R"( byte"},{"stage":"bmc","line":"nope","message":1}]}]})";

    std::optional<prism::RunReport> rep;
    CHECK_NOTHROW(rep = prism::RunReport::load(write_bytes("badbyte.json", body)));
    REQUIRE(rep);
    CHECK(rep->notes == std::vector<std::string>{"caf\xc3\xa9"});
    REQUIRE(rep->functions.size() == 1);
    CHECK(rep->functions[0].line == 12);
    REQUIRE(rep->stages.size() == 1);
    CHECK(rep->stages[0].records == 1);
    REQUIRE(rep->stages[0].findings.size() == 2);
    CHECK(rep->stages[0].findings[0].line == std::optional<int>(12));
    CHECK(rep->stages[0].findings[0].file == "7");
    const auto& msg = rep->stages[0].findings[0].message;
    CHECK(msg.find("bad ") != std::string::npos);
    CHECK(msg.find(" byte") != std::string::npos);
    CHECK(msg.find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(msg.find('\xff') == std::string::npos);
    CHECK_FALSE(rep->stages[0].findings[1].line);
    CHECK(rep->stages[0].findings[1].message == "1");

    // A byte that was never valid UTF-8 must not make dumps throw either.
    prism::RunReport raw;
    prism::StageResult st;
    prism::Finding f;
    f.message = "bad ";
    f.message.push_back(static_cast<char>(0xFF));
    st.findings.push_back(f);
    raw.stages.push_back(st);
    std::string dumped;
    CHECK_NOTHROW(dumped = raw.dumps());
    CHECK(dumped.find('\xff') == std::string::npos);
    CHECK(dumped.find("\xEF\xBF\xBD") != std::string::npos);

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "prism_report_load", ec);
}

TEST_CASE("report json: object parsers stay strict outside a record") {
    auto finding = prism::finding_from_json_object(R"({"line":"12","file":7,"message":"ok"})");
    CHECK(finding.line == std::optional<int>(12));
    CHECK(finding.file == "7");
    CHECK(finding.message == "ok");

    auto stage = prism::stage_from_json_object(
        R"({"name":"bmc","records":"4","findings":[{"line":"12","file":3}]})");
    CHECK(stage.name == "bmc");
    CHECK(stage.records == 4);
    REQUIRE(stage.findings.size() == 1);
    CHECK(stage.findings[0].line == std::optional<int>(12));
    CHECK(stage.findings[0].file == "3");

    auto fn = prism::function_from_json_object(R"({"name":"f","file":"a.c","line":"12","span":["1","2"]})");
    CHECK(fn.name == "f");
    CHECK(fn.line == 12);
    CHECK(fn.span == std::pair<int, int>(1, 2));

    CHECK_THROWS(prism::finding_from_json_object("[]"));
    CHECK_THROWS(prism::stage_from_json_object("1"));
    CHECK_THROWS(prism::function_from_json_object("\"x\""));

    std::string bad = R"({"message":"bad )";
    bad.push_back(static_cast<char>(0xFF));
    bad += "\"}";
    CHECK_THROWS(prism::finding_from_json_object(bad));
    CHECK_THROWS(prism::stage_from_json_object(bad));
    CHECK_THROWS(prism::function_from_json_object(bad));
}
