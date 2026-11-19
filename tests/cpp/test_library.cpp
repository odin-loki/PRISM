// Proof library L1 (docs/PROOF_LIBRARY.md): import is idempotent, every row
// goes through the verdict audit, vetting sets tiers, lookups by each key.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/clock.hpp"
#include "prism/laws.hpp"
#include "prism/library.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace lib = prism::library;

json row(const std::string& stage, const std::string& fn, const std::string& status, json extra = json::object(),
         const std::string& msg = "") {
    return json{{"stage", stage},   {"file", "drivers/x.c"}, {"function", fn},  {"line", 10},
                {"status", status}, {"cls", ""},             {"message", msg}, {"counterexample", ""},
                {"extra", extra},   {"strength", "PROVES"},  {"evidence", ""}};
}

fs::path write_run(const fs::path& dir, const json& findings) {
    fs::create_directories(dir);
    json rep{{"root", "/src/linux"}, {"started", "2026-11-01T00:00:00Z"},
             {"stages", json::array({json{{"name", "pir"}, {"findings", findings}}})}};
    std::ofstream(dir / "report.json") << rep.dump();
    return dir;
}
}  // namespace

TEST_CASE("library: import, audit, idempotence, vetting, lookup, stats") {
    const auto base = fs::temp_directory_path() / ("prism-library-test-" + std::to_string(prism::clock::now_ns()));
    const auto run = write_run(
        base / "run1",
        json::array({row("pir", "ok", std::string(prism::laws::PROVED),
                         {{"pir_fingerprint", "aa"}, {"pir_shape", "s1"}, {"unwind", "8"},
                          {"pir_invariants", "[\"i <= n\"]"}, {"features", "{\"blocks\":3}"}}),
                     row("pir", "twin", std::string(prism::laws::PROVED), {{"pir_fingerprint", "bb"}, {"pir_shape", "s1"}}),
                     row("pir", "blocked", std::string(prism::laws::NEEDS_HARNESS), json::object(),
                         "UNENCODED: call @kmalloc (no model)"),
                     row("pir", "blocked2", std::string(prism::laws::NEEDS_HARNESS), json::object(),
                         "UNENCODED: call @kmalloc (no model)"),
                     // a stage that may not prove: the audit demotes it on import
                     row("fuzz", "fz", std::string(prism::laws::PROVED))}));
    lib::ImportOptions o;
    o.library = base / "lib";
    o.stages = {"pir", "fuzz"};
    auto st = lib::import_runs({run}, o);
    CHECK(st.errors.empty());
    CHECK(st.rows == 5);
    CHECK(st.imported == 5);
    CHECK(st.demoted == 1);
    // the same run again: nothing new
    auto again = lib::import_runs({run / "report.json"}, o);
    CHECK(again.imported == 0);
    CHECK(again.duplicates == 5);

    auto ok = lib::lookup(o.library, "exact", "aa");
    REQUIRE(ok.size() == 1);
    CHECK(ok[0]["function"] == "ok");
    CHECK(ok[0]["level"] == "L4");
    CHECK(ok[0]["tier"] == "T0");
    CHECK(ok[0]["artefacts"]["unwind"] == "8");
    CHECK(ok[0]["features"]["blocks"] == 3);
    CHECK(ok[0]["provenance"]["root"] == "/src/linux");
    CHECK(lib::lookup(o.library, "shape", "s1").size() == 2);  // a near match by shape
    auto fz = lib::lookup(o.library, "function", "drivers/x.c::fz");
    REQUIRE(fz.size() == 1);
    CHECK(fz[0]["audit_demoted_from"] == "PROVED");
    CHECK(fz[0]["status"] != "PROVED");
    CHECK(lib::lookup(o.library, "exact", "nope").empty());

    auto s = lib::summary(o.library);
    CHECK(s["entries"] == 5);
    CHECK(s["by_tier"]["T0"] == 5);
    CHECK(s["needs_harness"]["UNENCODED: call @kmalloc"] == 2);

    // vetting after import: a T1 and a quarantine
    {
        std::ofstream v(base / "vet.jsonl");
        v << R"({"file":"drivers/x.c","function":"ok","tier":"T1","note":"replayed"})" << "\n"
          << R"({"file":"drivers/x.c","function":"twin","tier":"Q","note":"disagrees with bmc"})" << "\n"
          << R"({"file":"drivers/x.c","function":"blocked","tier":"T9"})" << "\n";
    }
    auto [changed, errors] = lib::vet(o.library, base / "vet.jsonl");
    CHECK(changed == 2);
    CHECK(errors.size() == 1);  // T9 is not a tier
    CHECK(lib::lookup(o.library, "exact", "aa")[0]["tier"] == "T1");
    CHECK(lib::lookup(o.library, "exact", "bb")[0]["vetting"] == "disagrees with bmc");
    auto s2 = lib::summary(o.library);
    CHECK(s2["by_tier"]["T1"] == 1);
    CHECK(s2["by_tier"]["Q"] == 1);
    CHECK(s2["by_tier"]["T0"] == 3);
    // ids do not change with the tier: a re-import is still all duplicates
    CHECK(lib::import_runs({run}, o).duplicates == 5);

    // a broken report is an error, not a silent skip (Law 7)
    fs::create_directories(base / "bad");
    std::ofstream(base / "bad" / "report.json") << "{not json";
    auto bad = lib::import_runs({base / "bad"}, o);
    CHECK(bad.errors.size() == 1);
    fs::remove_all(base);
}
