// Polyglot Law 1: a missing python3 is NOTRUN (strength FINDS), never CLEAN.
// JSON syntax is built in: a broken file is FAILED SYNTAX-ERROR, a valid
// file is UNKNOWN ("not a proof"), never PROVED.
//
// The lookup seam is run_polyglot(root, cfg, which). It does not change PATH.
// Windows Config::which searches the application directory, the current
// directory, and the system directories before PATH, so clearing PATH does
// not hide python3.

#include <doctest/doctest.h>

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/polyglot.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace pg = prism::polyglot;

namespace {

struct Tree {
    fs::path dir;
    Tree(const std::string& name, const std::string& text) {
        std::random_device rd;
        dir = fs::temp_directory_path() /
              ("prism_pg_miss_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
        std::ofstream(dir / name, std::ios::binary) << text;
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    prism::Config cfg() const {
        auto c = prism::default_config();
        c.root = dir;
        c.jobs = 1;
        c.allow_exec = false;
        c.tools.clear();
        return c;
    }
};

std::optional<fs::path> no_exe(std::string_view) { return std::nullopt; }

std::string extra(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

std::vector<prism::Finding> rows(const std::vector<prism::Finding>& out, const char* group) {
    std::vector<prism::Finding> r;
    for (auto& f : out)
        if (extra(f, "check") == group) r.push_back(f);
    return r;
}

const pg::Tool& tool_named(const std::string& name) {
    for (auto& c : pg::checks())
        for (auto& t : c.tools)
            if (t.name == name) return t;
    FAIL("no tool " << name);
    throw 0;
}

}  // namespace

TEST_CASE("polyglot missing: python3 hidden from lookup is NOTRUN and not a proof") {
    Tree t("a.py", "x = 1\n");
    auto cfg = t.cfg();
    // --tool is empty, and which() never returns a path, so PATH is unread.
    CHECK_FALSE(pg::resolve(tool_named("python"), cfg, &no_exe));
    auto out = prism::run_polyglot(t.dir, cfg, &no_exe);
    auto py = rows(out, "python-syntax");
    REQUIRE(py.size() == 1);
    CHECK(py[0].stage == "polyglot");
    CHECK(py[0].status == prism::laws::NOTRUN);
    CHECK(py[0].status != prism::laws::CLEAN);
    CHECK(py[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK(py[0].strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(py[0].status));
    CHECK(py[0].message == "python-syntax: python not found for 1 python file(s)");
    CHECK(extra(py[0], "install") == "install Python 3 (python3 on PATH)");
    CHECK(extra(py[0], "check") == "python-syntax");
    CHECK(py[0].message.find("python") != std::string::npos);
    CHECK(extra(py[0], "install").find("python3") != std::string::npos);
}

TEST_CASE("polyglot missing: broken JSON is FAILED SYNTAX-ERROR, not NOTRUN") {
    Tree t("bad.json", "{\"a\": 1,}\n");
    auto out = prism::run_polyglot(t.dir, t.cfg());
    auto got = rows(out, "json-syntax");
    REQUIRE(got.size() == 1);
    CHECK(got[0].stage == "polyglot");
    CHECK(got[0].status == prism::laws::FAILED);
    CHECK(got[0].status != prism::laws::NOTRUN);
    CHECK(got[0].cls == "SYNTAX-ERROR");
    CHECK(got[0].file == "bad.json");
    CHECK(got[0].line == 1);
    CHECK(got[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK(got[0].strength != prism::laws::STRENGTH_PROVES);
    CHECK(extra(got[0], "tool") == "prism-json");
    CHECK(extra(got[0], "check") == "json-syntax");
    CHECK(got[0].message.rfind("prism-json: JSON: ", 0) == 0);
}

TEST_CASE("polyglot missing: valid JSON is UNKNOWN, not a proof of the program") {
    Tree t("ok.json", "{\"a\": [1, 2]}\n");
    auto out = prism::run_polyglot(t.dir, t.cfg());
    auto got = rows(out, "json-syntax");
    REQUIRE(got.size() == 1);
    CHECK(got[0].status == prism::laws::UNKNOWN);
    CHECK(got[0].message == "prism-json: 1 json file(s), no diagnostics (not a proof)");
    CHECK(got[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK(got[0].strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(got[0].status));
    for (auto& f : out) {
        CHECK(f.status != prism::laws::PROVED);
        CHECK(f.status != prism::laws::PROVED_UNBOUNDED);
        CHECK(f.status != prism::laws::PROVED_CERTIFIED);
        CHECK(f.status != prism::laws::PROVED_ASSUMING);
        CHECK_FALSE(prism::laws::is_proof(f.status));
        CHECK(f.strength != prism::laws::STRENGTH_PROVES);
    }
}
