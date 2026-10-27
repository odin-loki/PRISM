// stages.jsonl: a line that is not a stage object is skipped. The next
// line is still read, and its status is the status that was written
// (PROVED stays PROVED, NOTRUN stays NOTRUN). A file of only bad lines
// is an empty stage list, not a CLEAN row.
//
// functions.json is a different reader: one bad element drops the whole
// function list so resume cannot skip classify. That rule is locked in
// test_journal_confidence.cpp and is not covered here.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/journal.hpp"
#include "prism/laws.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        static int seq = 0;
        path = fs::temp_directory_path() / ("prism_jl_" + tag + "_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

prism::StageResult stage(const std::string& name, const std::string& status, int records) {
    prism::StageResult s;
    s.name = name;
    s.status = status;
    s.records = records;
    s.detail = name + ":" + status;
    return s;
}

// Replace the first line with text that is not a JSON object. The newline
// and every later line stay byte for byte.
void corrupt_first_line(const fs::path& file) {
    auto text = slurp(file);
    const auto nl = text.find('\n');
    REQUIRE(nl != std::string::npos);
    REQUIRE(nl + 1 < text.size());
    text.replace(0, nl, "{not json");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << text;
}

void expect_second_status(const std::string& status) {
    TempDir dir("keep_" + status);
    prism::journal_append_stage(dir.path, stage("torn", "failed", 4));
    prism::journal_append_stage(dir.path, stage("kept", status, 2));
    corrupt_first_line(dir.path / prism::STAGES_JSONL);

    const auto text = slurp(dir.path / prism::STAGES_JSONL);
    CHECK(text.find("{not json\n") == 0);
    CHECK(text.find("\"name\":\"kept\"") != std::string::npos);
    CHECK(text.find("\"status\":\"" + status + "\"") != std::string::npos);

    auto recs = prism::journal_read_stages(dir.path);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].name == "kept");
    CHECK(recs[0].status == status);
    CHECK(recs[0].records == 2);
    CHECK(recs[0].detail == "kept:" + status);
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
}

}  // namespace

TEST_CASE("journal line: a corrupt first line keeps the second status") {
    expect_second_status(std::string(prism::laws::PROVED));
    expect_second_status(std::string(prism::laws::NOTRUN));
}

TEST_CASE("journal line: only corrupt lines are an empty stage list") {
    TempDir dir("allcorrupt");
    {
        std::ofstream out(dir.path / prism::STAGES_JSONL, std::ios::binary);
        REQUIRE(out.is_open());
        out << "{not json\n"
               "1\n"
               "[]\n"
               "\"CLEAN\"\n"
               "{\"name\":\"bmc\",\"status\":\"CLEAN\"\n";
    }
    auto recs = prism::journal_read_stages(dir.path);
    CHECK(recs.empty());
    for (const auto& s : recs) CHECK(s.status != std::string(prism::laws::CLEAN));
}
