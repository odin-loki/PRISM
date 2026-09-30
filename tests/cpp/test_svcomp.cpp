// `prism svcomp` (roadmap 6.3, docs/SVCOMP.md): the SV-COMP answer mapping
// keeps PRISM's laws: `true` only from a proof of main that covers the
// property (never PROVED-ASSUMING or BOUNDED, Law 2), `false` only from a
// refutation whose counterexample replays (Law 9: replay needs
// --allow-exec), everything else `unknown`. Witnesses are SV-COMP format
// 2.0 YAML. Also the task-definition reader, the subset scorer, the tool
// archive and the session runner the scorer uses.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/pipeline.hpp"
#include "prism/svcomp.hpp"
#include "prism/taskdef.hpp"
#include "../../src/prism/proc.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using namespace prism::svcomp;

namespace {

fs::path repo_root() {
    // __FILE__ may be relative to the build directory (ccache base_dir):
    // try it, then walk up from the working directory.
    fs::path from_file = fs::path(__FILE__).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (fs::is_directory(from_file / "tests" / "conformance" / "sv-comp", ec)) return from_file;
    for (fs::path d = fs::current_path(); !d.empty(); d = d.parent_path()) {
        if (fs::is_directory(d / "tests" / "conformance" / "sv-comp", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return from_file;
}

fs::path props_dir() { return repo_root() / "tests" / "conformance" / "sv-comp" / "properties"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("prism_svcomp_test_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        std::ofstream(path / name, std::ios::binary) << text;
        return path / name;
    }
};

json report(std::initializer_list<std::pair<std::string, std::vector<json>>> stages) {
    json rep = {{"stages", json::array()}};
    for (const auto& [name, fs_] : stages) {
        json rows = json::array();
        for (auto f : fs_) {
            f["function"] = "main";
            rows.push_back(f);
        }
        rep["stages"].push_back({{"name", name}, {"status", "ok"}, {"findings", rows}});
    }
    return rep;
}

json replayed(const json&) {
    return {{"replay", "replayed"}, {"detail", "UBSan: signed integer overflow"}, {"line", 3}, {"column", 9}};
}
json not_replayed(const json&) { return {{"replay", "not-replayed"}, {"why", "ran clean"}}; }

bool cc_with_ubsan() {
    auto cc = prism::Config{}.which({"clang", "gcc"});
    if (!cc) return false;
    TempDir d;
    auto src = d.write("t.c", "int main(void){return 0;}\n");
    auto r = prism::detail::run_process({cc->string(), "-fsanitize=signed-integer-overflow", src.string(), "-o",
                                         (d.path / "t").string()},
                                        120.0);
    return !r.failed && !r.timed_out && r.rc == 0;
}

std::vector<std::pair<long, std::optional<long>>> positions(const std::vector<NondetValue>& wps) {
    std::vector<std::pair<long, std::optional<long>>> out;
    for (const auto& w : wps) out.emplace_back(w.location.line, w.location.column);
    return out;
}

using Trace = std::vector<std::pair<std::string, Num>>;
Trace trace_of(std::initializer_list<std::pair<const char*, long long>> xs) {
    Trace t;
    for (const auto& [n, v] : xs) t.emplace_back(n, Num::of_int(v));
    return t;
}

}  // namespace

// ---------------------------------------------------------------- property / task

TEST_CASE("svcomp: property files map to the supported properties") {
    CHECK(parse_property(slurp(props_dir() / "no-overflow.prp")) == "no-overflow");
    CHECK(parse_property(slurp(props_dir() / "unreach-call.prp")) == "unreach-call");
    CHECK(parse_property(slurp(props_dir() / "valid-memsafety.prp")) == "valid-memsafety");
    CHECK(parse_property("CHECK( init(main()), LTL(F end) )") == "unsupported");
    // two properties in one file: not one answer
    CHECK(parse_property("CHECK( init(main()), LTL(G ! overflow) )\nCHECK( init(main()), LTL(G ! call(reach_error())) )") ==
          "unsupported");
}

TEST_CASE("svcomp: the pinned subset's task files resolve (117 no-overflow, 127 unreach-call)") {
    const fs::path suite = repo_root() / "tests" / "conformance" / "sv-comp";
    auto nov = subset_tasks(suite, "no-overflow.prp");
    auto reach = subset_tasks(suite, "unreach-call.prp");
    CHECK(nov.size() == 117);
    CHECK(reach.size() == 127);
    for (const auto* ts : {&nov, &reach})
        for (const auto& t : *ts) {
            INFO(t.id);
            CHECK(fs::exists(t.property_file));
            CHECK(fs::exists(t.input));
        }
}

TEST_CASE("svcomp: width-dependent code (ILP32 tasks PRISM cannot encode)") {
    CHECK(width_dependent_code("int main(){ long x = 1; return 0; }"));
    CHECK(width_dependent_code("typedef long L; typedef L M; int main(){ M x = 1; return 0; }"));
    CHECK(width_dependent_code("struct s { long a; }; int main(){ struct s v; return 0; }"));
    CHECK(width_dependent_code("int main(){ return sizeof(int); }"));
    // an unused declaration from a header does not count
    CHECK_FALSE(width_dependent_code("extern long f(void);\nint main(){ int x = 1; return x; }"));
    // glibc's assert(): `(void) sizeof (cond)` is discarded
    CHECK_FALSE(width_dependent_code("void reach_error() { ((void) sizeof ((0) ? 1 : 0)); }\nint main(){ return 0; }"));
    // comments and strings do not count
    CHECK_FALSE(width_dependent_code("int main(){ /* long */ const char* s = \"size_t\"; // long\n return 0; }"));
}

TEST_CASE("svcomp: the reach_error() call site, not its definition") {
    TempDir d;
    auto src = d.write("t.c",
                       "void reach_error() { __assert_fail(\"0\", \"t.c\", 1, \"reach_error\"); }\n"
                       "int main(void) {\n  if (1) reach_error();\n  return 0;\n}\n");
    CHECK(reach_error_call_site(src, std::nullopt) == LineCol{3, 10});
    src = d.write("t.c", "void reach_error(void) {}\nint main(void) {\n  reach_error();\n  reach_error();\n}\n");
    CHECK_FALSE(reach_error_call_site(src, std::nullopt).has_value());
    CHECK(reach_error_call_site(src, 4) == LineCol{4, 3});
}

// ---------------------------------------------------------------- decision

TEST_CASE("svcomp: true only from a covering proof of main (never PROVED-ASSUMING or BOUNDED)") {
    for (const char* st : {"PROVED", "PROVED-UNBOUNDED", "PROVED-CERTIFIED"}) {
        INFO(st);
        CHECK(decide(report({{"bmc", {{{"status", st}, {"cls", ""}}}}}), "no-overflow", replayed).answer == "true");
    }
    for (const char* st : {"PROVED-ASSUMING", "BOUNDED", "NEEDS-HARNESS", "UNKNOWN", "TIMEOUT", "ERROR", "NOTRUN", "CLEAN"}) {
        INFO(st);
        CHECK(decide(report({{"bmc", {{{"status", st}, {"cls", ""}}}}}), "no-overflow", replayed).answer == "unknown");
    }
    // a proof by a stage that is not a verdict stage covers nothing
    CHECK(decide(report({{"wp", {{{"status", "PROVED"}, {"cls", ""}}}}}), "no-overflow", replayed).answer == "unknown");
}

TEST_CASE("svcomp: false needs a replayed counterexample") {
    auto rep = report({{"bmc", {{{"status", "FAILED"}, {"cls", "INT-SIGNED-OVF"}, {"message", "ovf+"}}}}});
    auto d = decide(rep, "no-overflow", replayed);
    CHECK(d.answer == "false(no-overflow)");
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "bmc");
    CHECK(d.reason.find("counterexample replayed: UBSan: signed integer overflow") != std::string::npos);
    d = decide(rep, "no-overflow", not_replayed);
    CHECK(d.answer == "unknown");
    CHECK(d.reason == "bmc: FAILED INT-SIGNED-OVF but the counterexample did not replay (not-replayed: ran clean)");
}

TEST_CASE("svcomp: a finding of another class is not a refutation") {
    auto rep = report({{"bmc", {{{"status", "PROVED-UNBOUNDED"}, {"cls", ""}}}},
                       {"pir", {{{"status", "FAILED"}, {"cls", "FUNC-CONTRACT"}, {"extra", {{"prop", "assert"}}}}}}});
    CHECK(decide(rep, "no-overflow", replayed).answer == "true");
    rep = report({{"bmc", {{{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}}}}});
    CHECK(decide(rep, "no-overflow", replayed).answer == "unknown");
}

TEST_CASE("svcomp: a shift whose result is not representable is an overflow, a bad shift count is not") {
    // SV-COMP no-overflow: pir shift-base, bmc shift31
    for (const json& f : {json{{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}, {"message", "shift31: INT-SHIFT-UB"}},
                          json{{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}, {"extra", {{"prop", "shift-base"}}}}}) {
        CHECK(decide(report({{"bmc", {f}}}), "no-overflow", replayed).answer == "false(no-overflow)");
        CHECK(decide(report({{"bmc", {f}}}), "no-overflow", not_replayed).answer == "unknown");
    }
    for (const json& f : {json{{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}, {"message", "shift: INT-SHIFT-UB"}},
                          json{{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}, {"message", "shift-neg: INT-SHIFT-UB"}},
                          json{{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}, {"extra", {{"prop", "shift"}}}}})
        CHECK(decide(report({{"bmc", {f}}}), "no-overflow", replayed).answer == "unknown");
    // not an unreach-call refutation
    json f = {{"status", "FAILED"}, {"cls", "INT-SHIFT-UB"}, {"message", "shift31: INT-SHIFT-UB"}};
    CHECK(decide(report({{"pir", {f}}}), "unreach-call", replayed).answer == "unknown");
}

TEST_CASE("svcomp: finding_prop reads extra.prop, else the message prefix") {
    CHECK(finding_prop(json{{"message", "shift31: INT-SHIFT-UB"}}) == "shift31");
    CHECK(finding_prop(json{{"extra", {{"prop", "shift-base"}}}}) == "shift-base");
    CHECK(finding_prop(json{{"message", "no colon"}}) == "");
}

TEST_CASE("svcomp: a refutation with nondet call sites is replayed first, pir's before bmc's") {
    auto rep = report({{"bmc", {{{"status", "FAILED"}, {"cls", "INT-SIGNED-OVF"}, {"extra", {{"nondet", "f=1"}}}}}},
                       {"pir", {{{"status", "FAILED"}, {"cls", "INT-SIGNED-OVF"},
                                 {"extra", {{"nondet", "f=1"}, {"nondet_loc", "3:9"}}}}}}});
    auto d = decide(rep, "no-overflow", replayed);
    CHECK(d.answer == "false(no-overflow)");
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "pir");
    // both have call sites: pir first
    rep["stages"][0]["findings"][0]["extra"]["nondet_loc"] = "3:9";
    d = decide(rep, "no-overflow", replayed);
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "pir");
    // only bmc's has call sites: bmc first
    rep["stages"][1]["findings"][0]["extra"].erase("nondet_loc");
    d = decide(rep, "no-overflow", replayed);
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "bmc");
}

TEST_CASE("svcomp: an unreplayed refutation blocks a proof; a replayed one reports the disagreement") {
    auto rep = report({{"bmc", {{{"status", "PROVED"}, {"cls", ""}}}},
                       {"pir", {{{"status", "FAILED"}, {"cls", "INT-SIGNED-OVF"}}}}});
    CHECK(decide(rep, "no-overflow", not_replayed).answer == "unknown");
    auto d = decide(rep, "no-overflow", replayed);
    CHECK(d.answer == "false(no-overflow)");
    CHECK(d.reason.find("DISAGREEMENT: bmc claimed a proof") != std::string::npos);
}

TEST_CASE("svcomp: unreach-call proofs and refutations") {
    // bmc and pir both encode reach_error() as a property: either proof covers it
    for (const char* st : {"bmc", "pir"}) {
        CHECK(decide(report({{st, {{{"status", "PROVED"}, {"cls", ""}}}}}), "unreach-call", replayed).answer == "true");
        CHECK(decide(report({{st, {{{"status", "BOUNDED"}, {"cls", ""}}}}}), "unreach-call", replayed).answer == "unknown");
    }
    // bmc names the check in the message prefix
    auto rep = report({{"bmc", {{{"status", "FAILED"}, {"cls", "FUNC-CONTRACT"}, {"message", "reach_error: FUNC-CONTRACT"}}}}});
    CHECK(decide(rep, "unreach-call", replayed).answer == "false(unreach-call)");
    CHECK(decide(rep, "unreach-call", not_replayed).answer == "unknown");
    rep = report({{"bmc", {{{"status", "FAILED"}, {"cls", "FUNC-CONTRACT"}, {"message", "abort: FUNC-CONTRACT"}}}}});
    CHECK(decide(rep, "unreach-call", replayed).answer == "unknown");
    // an unreplayed bmc refutation blocks pir's proof
    rep = report({{"bmc", {{{"status", "FAILED"}, {"cls", "FUNC-CONTRACT"}, {"message", "reach_error: FUNC-CONTRACT"}}}},
                  {"pir", {{{"status", "PROVED"}, {"cls", ""}}}}});
    CHECK(decide(rep, "unreach-call", not_replayed).answer == "unknown");
    rep = report({{"pir", {{{"status", "FAILED"}, {"cls", "FUNC-CONTRACT"}, {"extra", {{"prop", "reach_error"}}}}}}});
    CHECK(decide(rep, "unreach-call", replayed).answer == "false(unreach-call)");
    rep = report({{"pir", {{{"status", "FAILED"}, {"cls", "FUNC-CONTRACT"}, {"extra", {{"prop", "abort"}}}}}}});
    CHECK(decide(rep, "unreach-call", replayed).answer == "unknown");
}

TEST_CASE("svcomp: valid-memsafety is never true; a replay names the subproperty") {
    auto rep = report({{"bmc", {{{"status", "PROVED-CERTIFIED"}, {"cls", ""}}}}, {"pir", {{{"status", "PROVED"}, {"cls", ""}}}}});
    CHECK(decide(rep, "valid-memsafety", replayed).answer == "unknown");
    rep = report({{"pir", {{{"status", "FAILED"}, {"cls", "MEM-OOB-READ"}}}}});
    auto free_replay = [](const json&) { return json{{"replay", "replayed"}, {"subproperty", "valid-free"}}; };
    CHECK(decide(rep, "valid-memsafety", free_replay).answer == "false(valid-free)");
}

TEST_CASE("svcomp: a crashed stage or no finding of main is unknown") {
    CHECK(decide(json{{"stages", json::array()}}, "no-overflow", replayed).answer == "unknown");
    json rep = {{"stages", {{{"name", "bmc"}, {"status", "failed"}, {"detail", "boom"}, {"findings", json::array()}}}}};
    auto d = decide(rep, "no-overflow", replayed);
    CHECK(d.answer == "unknown");
    CHECK(d.reason == "no proof covering no-overflow and no replayed refutation (bmc: STAGE-FAILED)");
    CHECK(decide(report({{"bmc", {}}}), "no-overflow", replayed).reason == "no verdict stage reported main");
    CHECK(decide(report({{"bmc", {{{"status", "PROVED"}}}}}), "termination", replayed).answer == "unknown");
}

TEST_CASE("svcomp: SV-COMP points") {
    CHECK(score(true, "true") == std::pair<std::string, int>{"correct", 2});
    CHECK(score(false, "false(no-overflow)") == std::pair<std::string, int>{"correct", 1});
    CHECK(score(false, "true") == std::pair<std::string, int>{"wrong", -32});
    CHECK(score(true, "false(no-overflow)") == std::pair<std::string, int>{"wrong", -16});
    CHECK(score(true, "unknown") == std::pair<std::string, int>{"unknown", 0});
    CHECK(score(false, "ERROR") == std::pair<std::string, int>{"unknown", 0});
    CHECK(score(false, "TIMEOUT") == std::pair<std::string, int>{"unknown", 0});
}

TEST_CASE("svcomp: score.md totals") {
    prism::svcomp::ojson rows = prism::svcomp::ojson::array();
    auto row = [](const char* task, bool exp, const char* ans) {
        auto [outcome, pts] = score(exp, ans);
        prism::svcomp::ojson r = prism::svcomp::ojson::object();
        r["task"] = task;
        r["expected"] = exp;
        r["answer"] = ans;
        r["outcome"] = outcome;
        r["points"] = pts;
        r["reason"] = "a|b";
        return r;
    };
    rows.push_back(row("a.yml", true, "true"));
    rows.push_back(row("b.yml", false, "false(no-overflow)"));
    rows.push_back(row("c.yml", true, "unknown"));
    prism::svcomp::ojson meta = {{"property", "no-overflow"}, {"via", "prism svcomp result line"}, {"prism", "/x/prism"}};
    auto md = score_markdown(rows, meta);
    CHECK(md.find("- tasks: 3 (2 expected true, 1 expected false)\n") != std::string::npos);
    CHECK(md.find("- score: **3** of a possible 5\n") != std::string::npos);
    CHECK(md.find("- correct true: 1, correct false: 1, incorrect true: 0, incorrect false: 0, unknown/error: 1\n") !=
          std::string::npos);
    CHECK(md.find("| c.yml | true | unknown | 0 | a/b |\n") != std::string::npos);
}

// ---------------------------------------------------------------- witnesses

namespace {

WitnessMeta test_meta(const fs::path& src) {
    WitnessMeta m;
    m.input_file = src;
    m.input_file_name = "t.c";
    m.specification = "CHECK( init(main()), LTL(G ! overflow) )\n";
    m.producer_version = "test";
    m.creation_time = "2026-09-23T00:00:00Z";
    m.uuid = "00000000-0000-4000-8000-000000000000";
    return m;
}

const char* kMetaYaml =
    "  metadata:\n"
    "    format_version: \"2.0\"\n"
    "    uuid: \"00000000-0000-4000-8000-000000000000\"\n"
    "    creation_time: \"2026-09-23T00:00:00Z\"\n"
    "    producer:\n"
    "      name: PRISM\n"
    "      version: test\n"
    "    task:\n"
    "      input_files:\n"
    "        - t.c\n"
    "      input_file_hashes:\n"
    "        t.c: \"628f4adf315baadb5d052edb8b2ef9affeaba6e144d0a657303e924ca4b7e843\"\n"
    "      specification: \"CHECK( init(main()), LTL(G ! overflow) )\"\n"
    "      data_model: LP64\n"
    "      language: C\n";

const char* kTaskText = "int main(void) { int x = __VERIFIER_nondet_int(); return x + 1; }\n";

}  // namespace

TEST_CASE("svcomp witness: violation_sequence structure (function_return waypoints, one target last)") {
    TempDir d;
    auto src = d.write("t.c", kTaskText);
    Counterexample cex{"main", Location{"t.c", 1, 58, "main"}, {}};
    cex.nondet.push_back(NondetValue{Location{"t.c", 1, 50, "main"}, Num::of_int(2147483647)});
    auto doc = build_violation_witness(cex, test_meta(src));
    REQUIRE(doc.size() == 1);
    const auto& e = doc[0];
    CHECK(e["entry_type"] == "violation_sequence");
    CHECK(e["metadata"]["format_version"] == "2.0");
    CHECK(e["metadata"]["task"]["specification"] == "CHECK( init(main()), LTL(G ! overflow) )");
    const auto& segs = e["content"];
    CHECK(segs.back()["segment"][0]["waypoint"]["type"] == "target");
    CHECK(segs[0]["segment"][0]["waypoint"]["type"] == "function_return");
    CHECK(segs[0]["segment"][0]["waypoint"]["constraint"]["value"] == "\\result == 2147483647");
    // format 2.0: a function_return constraint is `\result <op> <constant>` in ACSL
    CHECK(segs[0]["segment"][0]["waypoint"]["constraint"]["format"] == "acsl_expression");
    int targets = 0;
    for (const auto& s : segs) targets += s["segment"][0]["waypoint"]["type"] == "target";
    CHECK(targets == 1);
    cex.nondet = {NondetValue{Location{"t.c", 1, 50, std::nullopt}, Num::of_int(-3)}};
    auto neg = build_violation_witness(cex, test_meta(src));
    CHECK(neg[0]["content"][0]["segment"][0]["waypoint"]["constraint"]["value"] == "\\result == -3");
}

TEST_CASE("svcomp witness: a target without a column, never column 1 by default") {
    // a statement first on its line: no column ("the first statement or
    // full expression in that line")
    CHECK(Location{"t.c", 4, std::nullopt, std::nullopt}.as_dict().dump() == R"({"file_name":"t.c","line":4})");
    CHECK(Location{"t.c", 4}.as_dict()["column"] == 1);
    CHECK(Location{"t.c", 4, 0}.as_dict()["column"] == 1);
}

TEST_CASE("svcomp witness: YAML text (golden) and it reads back as the same document") {
    TempDir d;
    auto src = d.write("t.c", kTaskText);
    Counterexample cex{"main", Location{"t.c", 1, std::nullopt, std::nullopt}, {}};
    cex.nondet.push_back(NondetValue{Location{"t.c", 1, 50}, Num::of_int(-3)});
    cex.nondet.push_back(NondetValue{Location{"t.c", 2, 7}, Num::of_float(0.25)});
    auto doc = build_violation_witness(cex, test_meta(src));
    const std::string want = std::string("- entry_type: violation_sequence\n") + kMetaYaml +
                             "  content:\n"
                             "    - segment:\n"
                             "        - waypoint:\n"
                             "            type: function_return\n"
                             "            action: follow\n"
                             "            location:\n"
                             "              file_name: t.c\n"
                             "              line: 1\n"
                             "              column: 50\n"
                             "            constraint:\n"
                             "              value: \"\\\\result == -3\"\n"
                             "              format: acsl_expression\n"
                             "    - segment:\n"
                             "        - waypoint:\n"
                             "            type: function_return\n"
                             "            action: follow\n"
                             "            location:\n"
                             "              file_name: t.c\n"
                             "              line: 2\n"
                             "              column: 7\n"
                             "            constraint:\n"
                             "              value: \"\\\\result == 0.25\"\n"
                             "              format: acsl_expression\n"
                             "    - segment:\n"
                             "        - waypoint:\n"
                             "            type: target\n"
                             "            action: follow\n"
                             "            location:\n"
                             "              file_name: t.c\n"
                             "              line: 1\n";
    CHECK(to_yaml(doc) == want);
    auto back = prism::taskdef::parse_yaml(to_yaml(doc));
    REQUIRE(back.value.has_value());
    CHECK(*back.value == doc);
}

TEST_CASE("svcomp witness: correctness witness (invariant_set; empty when nothing proved is exported)") {
    TempDir d;
    auto src = d.write("t.c", kTaskText);
    auto empty = build_correctness_witness({}, test_meta(src));
    CHECK(empty[0]["entry_type"] == "invariant_set");
    CHECK(empty[0]["content"].empty());
    CHECK(to_yaml(empty) == std::string("- entry_type: invariant_set\n") + kMetaYaml + "  content: []\n");
    Invariant inv{"loop_invariant", Location{"t.c", 3, 3, "main"}, "(i >= 0) && (i <= 9)"};
    auto doc = build_correctness_witness({inv}, test_meta(src));
    CHECK(doc[0]["content"][0]["invariant"].dump() ==
          R"j({"type":"loop_invariant","location":{"file_name":"t.c","line":3,"column":3,"function":"main"},)j"
          R"j("value":"(i >= 0) && (i <= 9)","format":"c_expression"})j");
    CHECK(to_yaml(doc) == std::string("- entry_type: invariant_set\n") + kMetaYaml +
                              "  content:\n"
                              "    - invariant:\n"
                              "        type: loop_invariant\n"
                              "        location:\n"
                              "          file_name: t.c\n"
                              "          line: 3\n"
                              "          column: 3\n"
                              "          function: main\n"
                              "        value: \"(i >= 0) && (i <= 9)\"\n"
                              "        format: c_expression\n");
    auto back = prism::taskdef::parse_yaml(to_yaml(doc));
    REQUIRE(back.value.has_value());
    CHECK(*back.value == doc);
}

TEST_CASE("svcomp witness: metadata defaults (a random UUID v4, UTC creation time)") {
    TempDir d;
    auto src = d.write("t.c", kTaskText);
    WitnessMeta m = test_meta(src);
    m.uuid.reset();
    m.creation_time.reset();
    auto a = build_correctness_witness({}, m)[0]["metadata"];
    auto b = build_correctness_witness({}, m)[0]["metadata"];
    const std::string u = a["uuid"];
    CHECK(u.size() == 36);
    CHECK(u[14] == '4');
    CHECK(std::string("89ab").find(u[19]) != std::string::npos);
    CHECK(a["uuid"] != b["uuid"]);
    const std::string t = a["creation_time"];
    CHECK(t.size() == 20);
    CHECK(t.back() == 'Z');
    CHECK(t[10] == 'T');
}

TEST_CASE("svcomp witness: ACSL constants and float text") {
    CHECK(acsl_literal(Num::of_int(-5)) == "-5");
    CHECK(acsl_literal(Num::of_int(7)) == "7");
    CHECK(acsl_literal(Num::of_float(0.5)) == "0.5");
    CHECK(acsl_literal(Num::of_int(static_cast<__int128>(18446744073709551615ULL))) == "18446744073709551615");
    // the spelling of Python's repr(float), which the witnesses used so far
    CHECK(float_repr(2147483647.0) == "2147483647.0");
    CHECK(float_repr(1e16) == "1e+16");
    CHECK(float_repr(1e15) == "1000000000000000.0");
    CHECK(float_repr(123456789012345678.0) == "1.2345678901234568e+17");
    CHECK(float_repr(1.5e-5) == "1.5e-05");
    CHECK(float_repr(0.0001) == "0.0001");
    CHECK(float_repr(-3.25) == "-3.25");
    CHECK(float_repr(0.1) == "0.1");
    CHECK(float_repr(5e-324) == "5e-324");
    CHECK(float_repr(-0.0) == "-0.0");
}

// ---------------------------------------------------------------- invariants

TEST_CASE("svcomp: correctness invariants are only proved ones the C text can carry") {
    TempDir d;
    auto src = d.write("t.c",
                       "int main(void) {\n  int i = 0, n = 5;\n  while (i < n) i++;\n"
                       "  for (int k = 0; k < 3; k++) i--;\n  do { i++; } while (i < 3);\n  return 0;\n}\n");
    json base = {{"stage", "bmc"}, {"function", "main"}, {"status", "PROVED-UNBOUNDED"}};
    json f = base;
    f["extra"] = {{"k_induction", "closed-invariants"},
                  {"invariant_loops", R"([{"kind":"while","line":3,"column":3},{"kind":"for","line":4,"column":3},)"
                                      R"({"kind":"do","line":5,"column":3}])"},
                  {"invariants", R"([["i >= 0", "i <= n", "i == 2 * n"], ["k >= 0", "i <= 5"], ["i >= 0"]])"}};
    auto [invs, note] = correctness_invariants(src, f);
    // arithmetic conjuncts, names declared in a for-init and do loops are left out
    REQUIRE(invs.size() == 2);
    CHECK(invs[0].location.line == 3);
    CHECK(invs[0].value == "(i >= 0) && (i <= n)");
    CHECK(invs[1].location.line == 4);
    CHECK(invs[1].value == "(i <= 5)");
    CHECK(note == "3 Houdini loop invariant conjunct(s) from bmc");
    // plain k-induction or a bounded-unwind proof: no invariant (empty set)
    for (const json& extra : {json{{"k_induction", "closed"}}, json{{"k_induction", "not-needed"}, {"unwind_closed", "true"}}}) {
        json g = base;
        g["extra"] = extra;
        CHECK(correctness_invariants(src, g).first.empty());
    }
    json bad = base;
    bad["extra"] = {{"k_induction", "closed-invariants"}, {"invariants", "not json"}};
    CHECK(correctness_invariants(src, bad).second == "unreadable invariants (empty invariant set)");
}

TEST_CASE("svcomp: loop invariants sit on the loop keyword") {
    TempDir d;
    auto src = d.write("t.c", "int main() {\n  int i = 0;\n  while (i < 10) i++;\n}\n");
    json f = {{"function", "main"},
              {"extra", {{"k_induction", "closed-invariants"},
                         {"invariants", R"([["i >= 0", "i <= 10", "i + 1 > i", "i <= 2 * n"]])"},
                         {"invariant_loops", R"([{"kind": "while", "line": 3, "column": 3}])"}}}};
    auto [invs, _] = correctness_invariants(src, f);
    REQUIRE(invs.size() == 1);
    CHECK(invs[0].kind == "loop_invariant");
    CHECK(invs[0].location.line == 3);
    CHECK(invs[0].location.column == 3);
    CHECK(invs[0].value == "(i >= 0) && (i <= 10) && (i + 1 > i)");
    f["extra"]["invariant_loops"] = R"([{"kind": "while", "line": 2, "column": 3}])";
    CHECK(correctness_invariants(src, f).first.empty());
}

TEST_CASE("svcomp: arithmetic in an invariant only when the bounds keep every subterm in int") {
    Bounds b = {{"i", {0LL, 1000LL}}, {"s", {0LL, std::nullopt}}};
    CHECK(int_safe("2 * i", b));
    CHECK(int_safe("i + 1", b));
    CHECK_FALSE(int_safe("2 * s", b));
    CHECK_FALSE(int_safe("i / 2", b));
    CHECK_FALSE(int_safe("3000000 * i", b));
    CHECK(int_safe("-(i - 1)", b));
    CHECK(exportable_conjuncts({"i >= 0", "i <= 1000", "s == 2 * i", "s <= 2 * n", "i != s", "f(i) > 0"}) ==
          std::vector<std::string>{"i >= 0", "i <= 1000", "s == 2 * i", "i != s"});
    CHECK(exportable_conjuncts({"i >= 0", "i <= 1000", "s == 2 * i", "s == 3000000 * i", "t <= s + 1", "i - 1 < i"}) ==
          std::vector<std::string>{"i >= 0", "i <= 1000", "s == 2 * i", "i - 1 < i"});
    // a bound written constant-first counts; a huge constant bounds nothing
    CHECK(exportable_conjuncts({"0 <= x", "10 > x", "y == x * x", "z <= 99999999999999999999", "w == z + 1"}) ==
          std::vector<std::string>{"0 <= x", "10 > x", "y == x * x", "z <= 99999999999999999999"});
}

TEST_CASE("svcomp: pir conjuncts rendered in C only when C means the proved relation") {
    std::map<std::string, CVar> t = {{"x", {"x", true, 32}}, {"u", {"u", false, 32}}, {"c", {"c", true, 8}}};
    auto term = [&](const json& j) -> std::optional<CVar> {
        auto it = t.find(j.at("v").get<std::string>());
        if (it == t.end()) return std::nullopt;
        return it->second;
    };
    auto r = [&](const json& c) { return render_conjunct(c, term); };
    CHECK(r({{"rel", "sle"}, {"a", {{"v", "x"}, {"w", 32}}}, {"b", {{"c", "4294967295"}, {"w", 32}}}}) == "x <= -1");
    CHECK_FALSE(r({{"rel", "ule"}, {"a", {{"v", "x"}, {"w", 32}}}, {"b", {{"c", "5"}, {"w", 32}}}}).has_value());
    CHECK(r({{"rel", "ult"}, {"a", {{"v", "x"}, {"w", 32}}}, {"b", {{"v", "u"}, {"w", 32}}}}) == "x < u");
    CHECK(r({{"rel", "mask:1"}, {"a", {{"v", "u"}, {"w", 32}}}, {"b", {{"c", "1"}, {"w", 32}}}}) == "(u & 1) == 1");
    CHECK_FALSE(r({{"rel", "eq"}, {"a", {{"v", "c"}, {"w", 8}}}, {"b", {{"v", "u"}, {"w", 32}}}}).has_value());
    // unsigned constants above INT_MAX carry a suffix
    CHECK(r({{"rel", "ule"}, {"a", {{"v", "u"}, {"w", 32}}}, {"b", {{"c", "4294967295"}, {"w", 32}}}}) == "u <= 4294967295U");
    // only unsigned int/long arithmetic wraps like the bit-vectors
    CHECK(r({{"rel", "add"}, {"a", {{"v", "u"}, {"w", 32}}}, {"b", {{"v", "u"}, {"w", 32}}}, {"c", {{"c", "7"}, {"w", 32}}}}) ==
          "u + u == 7");
    CHECK_FALSE(r({{"rel", "add"}, {"a", {{"v", "x"}, {"w", 32}}}, {"b", {{"v", "x"}, {"w", 32}}}, {"c", {{"c", "7"}, {"w", 32}}}})
                    .has_value());
    // width mismatch or an unknown value: left out
    CHECK_FALSE(r({{"rel", "sle"}, {"a", {{"v", "x"}, {"w", 64}}}, {"b", {{"c", "1"}, {"w", 64}}}}).has_value());
    CHECK_FALSE(r({{"rel", "sle"}, {"a", {{"v", "nope"}, {"w", 32}}}, {"b", {{"c", "1"}, {"w", 32}}}}).has_value());
}

TEST_CASE("svcomp: pir invariants named with the C variables from debug information") {
    prism::Config cfg;
    if (!cfg.which({"clang-18", "clang"}) || !cfg.which({"opt-18", "opt"})) {
        MESSAGE("NOTRUN: clang/opt not on PATH (pir invariants need the debug IR)");
        return;
    }
    TempDir d;
    auto src = d.write("t.c",
                       "extern unsigned int __VERIFIER_nondet_uint(void);\n"
                       "int main(void) {\n"
                       "  unsigned int y = 1U, n = __VERIFIER_nondet_uint();\n"
                       "  int k = 0;\n"
                       "  while (y < n) {\n"
                       "    y = y + 2U;\n"
                       "    k = k + 1;\n"
                       "    for (int j = 0; j < 3; j++) k = k + j;\n"
                       "  }\n"
                       "  return 0;\n"
                       "}\n");
    auto v = [](const char* n) { return json{{"v", n}, {"w", 32}}; };
    auto c = [](long long x) { return json{{"c", std::to_string(x)}, {"w", 32}}; };
    json conj = json::array(
        {json::array({{{"rel", "mask:1"}, {"a", v("y.0")}, {"b", c(1)}},
                      {{"rel", "ule"}, {"a", v("y.0")}, {"b", v("call")}},
                      {{"rel", "sge"}, {"a", v("k.0")}, {"b", c(0)}},
                      {{"rel", "ule"}, {"a", v("k.0")}, {"b", c(5)}},     // unsigned relation on an int: left out
                      {{"rel", "eq"}, {"a", v("nope")}, {"b", c(0)}}}),   // unknown value: left out
         json::array({{{"rel", "sge"}, {"a", v("k.1")}, {"b", v("k.0")}},  // k.0 is not k's value at the inner loop
                      {{"rel", "sge"}, {"a", v("j.0")}, {"b", c(0)}},
                      {{"rel", "ule"}, {"a", v("y.0")}, {"b", v("call")}},  // y was assigned before this loop
                      {{"rel", "sge"}, {"a", v("k.1")}, {"b", c(1)}}})});
    json loops = json::array({{{"kind", ""}, {"line", 5}, {"column", 3}}, {{"kind", ""}, {"line", 8}, {"column", 5}}});
    json f = {{"stage", "pir"}, {"function", "main"}, {"status", "PROVED-UNBOUNDED"},
              {"extra", {{"k_induction", "closed-invariants"}, {"invariant_conjuncts", conj.dump()},
                         {"invariant_loops", loops.dump()}}}};
    auto [invs, note] = correctness_invariants(src, f);
    INFO(note);
    REQUIRE(invs.size() == 2);
    // j is declared in the for-init: not in scope at the keyword
    CHECK(invs[0].location.line == 5);
    CHECK(invs[0].location.column == 3);
    CHECK(invs[0].value == "((y & 1) == 1) && (y <= n) && (k >= 0)");
    CHECK(invs[1].location.line == 8);
    CHECK(invs[1].location.column == 5);
    CHECK(invs[1].value == "(k >= 1)");
    // a preprocessed task: debug lines name another file's lines
    auto marked = d.write("m.c", "# 1 \"t.c\"\n" + slurp(src));
    CHECK(correctness_invariants(marked, f).first.empty());
}

// ---------------------------------------------------------------- replay

TEST_CASE("svcomp replay: Law 9 and nondet inputs are checked before anything runs") {
    TempDir d;
    auto src = d.write("t.c", "int main(void) { int x = 2147483647; x = x + 1; return 0; }\n");
    CHECK(replay(src, "no-overflow", false, std::nullopt, d.path / "w")["replay"] == "notrun");
    CHECK_FALSE(fs::exists(d.path / "w" / "replay.bin"));
    auto nd = d.write("n.c", "extern int __VERIFIER_nondet_int(void);\nint main(void) { return __VERIFIER_nondet_int(); }\n");
    auto r = replay(nd, "no-overflow", true, std::nullopt, d.path / "w2");
    CHECK(r["replay"] == "unsupported");
    auto odd = d.write("o.c", "int main(void) { return __VERIFIER_nondet_weird(); }\n");
    r = replay(odd, "no-overflow", true, std::vector<Num>{Num::of_int(1)}, d.path / "w3");
    if (prism::Config{}.which({"clang", "gcc", "cc"})) {
        CHECK(r["replay"] == "unsupported");
        CHECK(r["why"] == "unsupported nondet type __VERIFIER_nondet_weird");
    }
}

TEST_CASE("svcomp replay: stubs for the __VERIFIER_* functions the task does not define") {
    auto s = stub_source("extern int __VERIFIER_nondet_int(void);\nunsigned char __VERIFIER_nondet_uchar();\n"
                         "void reach_error() { abort(); }\nint main(void) { __VERIFIER_assume(__VERIFIER_nondet_int());"
                         " return __VERIFIER_nondet_uchar(); }\n",
                         {Num::of_int(-5), Num::of_int(200), Num::of_float(0.5)});
    CHECK(s.find("static const double prism_nondet[] = {-5.0, 200.0, 0.5};") != std::string::npos);
    CHECK(s.find("prism_count = 3;") != std::string::npos);
    CHECK(s.find("int __VERIFIER_nondet_int(void) { return (int)prism_take(); }") != std::string::npos);
    CHECK(s.find("unsigned char __VERIFIER_nondet_uchar(void) { return (unsigned char)prism_take(); }") != std::string::npos);
    CHECK(s.find("void __VERIFIER_assume(int c)") != std::string::npos);
    CHECK(s.find("void reach_error(void)") == std::string::npos);  // the task defines it
    CHECK(stub_source("int main(void) { return 0; }", {}).find("prism_nondet[] = {0.0};") != std::string::npos);
    CHECK_THROWS_AS(stub_source("int main(void) { return __VERIFIER_nondet_x(); }", {}), std::invalid_argument);
}

TEST_CASE("svcomp replay: sanitizer runs decide replayed / not-replayed") {
    if (!cc_with_ubsan()) {
        MESSAGE("NOTRUN: no C compiler with UBSan");
        return;
    }
    TempDir d;
    SUBCASE("a deterministic overflow replays at its line") {
        auto src = d.write("t.c", "int main(void) {\n  int x = 2147483647;\n  x = x + 1;\n  return x == 0;\n}\n");
        auto rp = replay(src, "no-overflow", true, std::nullopt, d.path / "w");
        CHECK(rp["replay"] == "replayed");
        CHECK(rp["line"] == 3);
        CHECK(rp.contains("column"));
        CHECK((rp["sandbox"] == "bwrap+rlimits" || rp["sandbox"] == "rlimits only (no bwrap)"));
    }
    SUBCASE("nondet values drive the run") {
        auto src = d.write("t.c",
                           "extern int __VERIFIER_nondet_int(void);\n"
                           "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  x = x + 1;\n  return 0;\n}\n");
        CHECK(replay(src, "no-overflow", true, std::nullopt, d.path / "w")["replay"] == "unsupported");
        CHECK(replay(src, "no-overflow", true, std::vector<Num>{Num::of_int(2147483647)}, d.path / "w2")["replay"] ==
              "replayed");
        CHECK(replay(src, "no-overflow", true, std::vector<Num>{Num::of_int(5)}, d.path / "w3")["replay"] ==
              "not-replayed");
    }
    SUBCASE("only a left shift whose result is not representable is an overflow") {
        for (auto [s, want] : {std::pair<const char*, const char*>{"31", "replayed"}, {"0", "not-replayed"}}) {
            TempDir e;
            auto src = e.write("t.c", std::string("int main(void) {\n  int x = 1, s = ") + s +
                                          ";\n  x = x << s;\n  return x == 0;\n}\n");
            auto rp = replay(src, "no-overflow", true, std::nullopt, e.path / "w");
            INFO(s);
            CHECK(rp["replay"] == want);
            if (std::string(want) == "replayed") CHECK(rp["line"] == 3);
        }
        // negative base: UB, but not an overflow
        auto src = d.write("t.c", "int main(void) {\n  int x = -1, s = 1;\n  x = x << s;\n  return x == 0;\n}\n");
        CHECK(replay(src, "no-overflow", true, std::nullopt, d.path / "w")["replay"] == "not-replayed");
    }
    SUBCASE("reach_error() must be called and abort") {
        auto src = d.write("t.c",
                           "extern int __VERIFIER_nondet_int(void);\nvoid reach_error();\n"
                           "int main(void) {\n  if (__VERIFIER_nondet_int() == 7) reach_error();\n  return 0;\n}\n");
        CHECK(replay(src, "unreach-call", true, std::vector<Num>{Num::of_int(7)}, d.path / "w")["replay"] == "replayed");
        CHECK(replay(src, "unreach-call", true, std::vector<Num>{Num::of_int(6)}, d.path / "w2")["replay"] ==
              "not-replayed");
    }
}

// ---------------------------------------------------------------- nondet traces and waypoints

namespace {
const char* kSrc =
    "extern int __VERIFIER_nondet_int(void);\n"
    "unsigned char __VERIFIER_nondet_uchar();\n"
    "int main(void) {\n"
    "  int a = __VERIFIER_nondet_int();\n"
    "  unsigned char k = __VERIFIER_nondet_uchar();\n"
    "  while (k--) a += __VERIFIER_nondet_int();\n"
    "  return a;\n}\n";
}

TEST_CASE("svcomp: the nondet trace of a refutation of main") {
    auto t = nondet_trace(json{{"function", "main"},
                               {"extra", {{"nondet", "__VERIFIER_nondet_int=-5, __VERIFIER_nondet_uchar=200"}}}});
    REQUIRE(t.has_value());
    CHECK(*t == Trace{{"__VERIFIER_nondet_int", Num::of_int(-5)}, {"__VERIFIER_nondet_uchar", Num::of_int(200)}});
    t = nondet_trace(json{{"function", "main"}, {"extra", {{"nondet", "__VERIFIER_nondet_int=-5, __VERIFIER_nondet_float=0.5"}}}});
    REQUIRE(t.has_value());
    CHECK((*t)[1].second == Num::of_float(0.5));
    CHECK(nondet_trace(json{{"function", "main"}, {"extra", {{"nondet", ""}}}})->empty());
    // not reported, or not a whole-program trace: no values (no replay)
    CHECK_FALSE(nondet_trace(json{{"function", "main"}, {"extra", json::object()}}).has_value());
    CHECK_FALSE(nondet_trace(json{{"function", "helper"}, {"extra", {{"nondet", "x=1"}}}}).has_value());
    CHECK_FALSE(nondet_trace(json{{"function", "main"}, {"extra", {{"nondet", "f=zz"}}}}).has_value());
    // integer literals as int(text, 0) reads them; a 64-bit unsigned value is exact
    t = nondet_trace(json{{"function", "main"},
                          {"extra", {{"nondet", "a=0x1f, b=-0b101, c=18446744073709551615, d=010, e=1e5"}}}});
    REQUIRE(t.has_value());
    CHECK((*t)[0].second == Num::of_int(31));
    CHECK((*t)[1].second == Num::of_int(-5));
    CHECK(int_text((*t)[2].second.i) == "18446744073709551615");
    CHECK((*t)[3].second == Num::of_float(10.0));  // "010" is no int literal: read as a float
    CHECK((*t)[4].second == Num::of_float(100000.0));
    t = nondet_trace(json{{"function", "main"}, {"extra", {{"nondet", "__VERIFIER_nondet_float=0.25"}}}});
    REQUIRE(t.has_value());
    CHECK((*t)[0].second == Num::of_float(0.25));
}

TEST_CASE("svcomp: nondet call locations from extra.nondet_loc") {
    json f = {{"function", "main"}, {"extra", {{"nondet", "a=1, b=2"}, {"nondet_loc", "4:11, 0:0"}}}};
    auto l = nondet_locations(f, 2);
    REQUIRE(l.has_value());
    REQUIRE(l->size() == 2);
    CHECK((*l)[0] == LineCol{4, 11});
    CHECK_FALSE((*l)[1].has_value());
    CHECK_FALSE(nondet_locations(f, 3).has_value());  // does not match the trace
    CHECK_FALSE(nondet_locations(json{{"extra", json::object()}}, 0).has_value());
    CHECK_FALSE(nondet_locations(json{{"extra", {{"nondet_loc", "4:x"}}}}, 1).has_value());
    CHECK(nondet_locations(json{{"extra", {{"nondet_loc", "6:13, 0:0"}}}}, 2)->at(0) == LineCol{6, 13});
    CHECK_FALSE(nondet_locations(json{{"extra", {{"nondet_loc", "6:13"}}}}, 2).has_value());
}

TEST_CASE("svcomp: a call exactly at a debug location") {
    CHECK(call_at("int x;\n  y = f ( );\n", "f", 2, 7) == LineCol{2, 11});
    CHECK_FALSE(call_at("int x;\n  y = gf();\n", "f", 2, 8).has_value());
    CHECK_FALSE(call_at("int x;\n", "f", 5, 1).has_value());
}

TEST_CASE("svcomp: waypoints stop at an ambiguous call site") {
    TempDir d;
    auto src = d.write("t.c", kSrc);
    auto wps = nondet_waypoints(src, trace_of({{"__VERIFIER_nondet_uchar", 1}, {"__VERIFIER_nondet_int", 3}}));
    // declarations are not call sites; the uchar call is unique
    REQUIRE(wps.size() == 1);
    CHECK(wps[0].location.line == 5);
    CHECK(wps[0].location.column == 45);
    CHECK(wps[0].value == Num::of_int(1));
    CHECK(wps[0].location.file_name == "t.c");
    // __VERIFIER_nondet_int is called at two sites: no waypoint for it
    CHECK(nondet_waypoints(src, trace_of({{"__VERIFIER_nondet_int", 3}})).empty());
}

TEST_CASE("svcomp: exact debug locations place every call") {
    TempDir d;
    auto src = d.write("t.c", kSrc);
    auto trace = trace_of({{"__VERIFIER_nondet_int", 7}, {"__VERIFIER_nondet_uchar", 1}, {"__VERIFIER_nondet_int", -2}});
    using Locs = std::vector<std::optional<LineCol>>;
    // pir's debug locations point at the start of each call; the waypoint
    // goes on its closing parenthesis, also where a function has two sites
    auto wps = nondet_waypoints(src, trace, Locs{LineCol{4, 11}, LineCol{5, 21}, LineCol{6, 20}});
    REQUIRE(wps.size() == 3);
    CHECK(positions(wps) == std::vector<std::pair<long, std::optional<long>>>{{4, 33}, {5, 45}, {6, 42}});
    CHECK(wps[2].value == Num::of_int(-2));
    // the first location names no call of that function: the int call has
    // two sites, so the prefix ends before it
    CHECK(nondet_waypoints(src, trace, Locs{LineCol{4, 12}, LineCol{5, 21}, LineCol{6, 20}}).empty());
    // an unknown uchar location falls back to its only call site
    wps = nondet_waypoints(src, trace, Locs{LineCol{4, 11}, std::nullopt, LineCol{6, 20}});
    CHECK(positions(wps) == std::vector<std::pair<long, std::optional<long>>>{{4, 33}, {5, 45}, {6, 42}});
    // a location list of the wrong length is ignored as a whole
    CHECK(nondet_waypoints(src, trace, Locs{LineCol{4, 11}}).empty());
}

TEST_CASE("svcomp: line markers disable debug locations, not physical ones") {
    TempDir d;
    auto src = d.write("t.i", std::string("# 1 \"t.c\"\n") + kSrc);
    auto trace = trace_of({{"__VERIFIER_nondet_int", 7}, {"__VERIFIER_nondet_uchar", 1}, {"__VERIFIER_nondet_int", -2}});
    using Locs = std::vector<std::optional<LineCol>>;
    Locs locs{LineCol{5, 11}, LineCol{6, 21}, LineCol{7, 20}};
    // in a preprocessed task, debug lines name the original file's lines;
    // int has two sites: nothing placed without the locations
    CHECK(nondet_waypoints(src, trace, locs).empty());
    // bmc's positions are physical (the analysed text itself): kept
    CHECK(nondet_waypoints(src, trace, locs, true).size() == 3);
}

TEST_CASE("svcomp: the violation target column") {
    TempDir d;
    auto p = d.write("t.c",
                     "int main() {\n  y = y +2*f();\n int x = (1 + 2) - 3;\n  if (a + b > 0) g();\n"
                     "  return a * b;\n  for (i = 0; i + 1 < n; i++) ;\n  x = a +\n    b;\n  --x;\n"
                     "  a = 1; b = c + d;\n  a = 1; int e = (c + d);\n  a = 1; if (c + d) g();\n}\n");
    // the first statement on its line: no column
    for (auto [n, c] : std::vector<std::pair<long, long>>{{2, 10}, {3, 13}, {4, 9}, {5, 12}, {9, 3}})
        CHECK_FALSE(target_column(p, n, c).has_value());
    // inside a for header, or a statement that began on an earlier line: the sanitizer's column
    CHECK(target_column(p, 6, 20) == 20);
    CHECK(target_column(p, 8, 5) == 5);
    // a later statement: its start (an initializer, a controlling expression)
    CHECK(target_column(p, 10, 16) == 10);
    CHECK(target_column(p, 11, 21) == 19);
    CHECK(target_column(p, 12, 15) == 14);
    CHECK(target_column(p, 99, 4) == 4);  // no such line
    CHECK(first_code_column(p, 3) == 2);
    CHECK(first_code_column(p, 99) == 1);
    for (const char* h : {"int x", "unsigned int *p", "struct s v[3]"}) CHECK(decl_head(h));
    for (const char* h : {"y", "*p", "a[i]"}) CHECK_FALSE(decl_head(h));
}

// ---------------------------------------------------------------- task definitions (YAML subset)

TEST_CASE("taskdef: the YAML subset reader") {
    auto y = prism::taskdef::parse_yaml(
        "format_version: '2.0'\n\n# comment\ninput_files: 'byte_add-1.i'\n\nproperties:\n"
        "  - property_file: ../properties/no-overflow.prp\n    expected_verdict: true\n"
        "  - property_file: ../properties/coverage-branches.prp\n"
        "options:\n  language: C\n  data_model: ILP32\n");
    REQUIRE(y.value.has_value());
    const auto& v = *y.value;
    CHECK(v["format_version"] == "2.0");
    CHECK(v["input_files"] == "byte_add-1.i");
    CHECK(v["properties"].size() == 2);
    CHECK(v["properties"][0]["expected_verdict"] == true);
    CHECK_FALSE(v["properties"][1].contains("expected_verdict"));
    CHECK(v["options"]["data_model"] == "ILP32");
    auto t = prism::taskdef::parse_taskdef(v);
    REQUIRE(t.has_value());
    CHECK(t->input_files == std::vector<std::string>{"byte_add-1.i"});
    CHECK(t->properties[0].expected_verdict == true);
    CHECK_FALSE(t->properties[1].expected_verdict.has_value());
    CHECK(t->data_model == "ILP32");
    // PRISM's conformance files: flow sequences, nested maps, typed scalars
    y = prism::taskdef::parse_yaml(
        "format_version: 1\ninput_files: a.c\nesbmc_options: \"--unwind 12\"\nexpected:\n  main: true\n"
        "witness:\n  f_false: [0, -2147483648, -9223372036854775808]\n  g: []\ndeterministic: yes\nratio: 0.5\n"
        "text: a # comment\nlist:\n- x\n- 'it''s'\nflow: {a: 1, b: [x, y]}\n");
    REQUIRE(y.value.has_value());
    CHECK(y.value->at("format_version") == 1);
    CHECK(y.value->at("esbmc_options") == "--unwind 12");
    CHECK(y.value->at("expected")["main"] == true);
    CHECK(y.value->at("witness")["f_false"][2] == std::numeric_limits<long long>::min());
    CHECK(y.value->at("witness")["g"].empty());
    CHECK(y.value->at("deterministic") == true);
    CHECK(y.value->at("ratio") == 0.5);
    CHECK(y.value->at("text") == "a");
    CHECK(y.value->at("list")[1] == "it's");
    CHECK(y.value->at("flow")["b"][1] == "y");
    // what it does not read is an error, never a wrong value
    CHECK_FALSE(prism::taskdef::parse_yaml("a: &x 1\nb: *x\n").value.has_value());
    CHECK_FALSE(prism::taskdef::parse_yaml("a: [1,\n  2]\n").value.has_value());
    CHECK_FALSE(prism::taskdef::parse_yaml("a: 1\na: 2\n").value.has_value());
}

// ---------------------------------------------------------------- one task end to end

TEST_CASE("svcomp solve: unsupported properties and ILP32 width answer unknown before any analysis") {
    TempDir d;
    auto task = d.write("t.c", "int main(void) { long x = 1; return (int)x; }\n");
    auto prp = d.write("p.prp", "CHECK( init(main()), LTL(F end) )\n");
    SolveOptions opt;
    opt.out = d.path / "out";
    opt.witness = d.path / "w.yml";
    auto oc = solve(task, prp, opt);
    CHECK(oc.decision.answer == "unknown");
    CHECK(oc.decision.reason == "unsupported property file p.prp");
    opt.data_model = "ilp32";
    oc = solve(task, props_dir() / "no-overflow.prp", opt);
    CHECK(oc.decision.answer == "unknown");
    CHECK(oc.decision.reason == "ILP32 task uses width-dependent types; PRISM's encoders are LP64");
    CHECK_FALSE(fs::exists(d.path / "out"));
    CHECK(version_string() == prism::PRISM_VERSION);
}

TEST_CASE("svcomp solve: a pinned false task (violation witness) and a true task (correctness witness)") {
    const fs::path sv = repo_root() / "tests" / "conformance" / "sv-comp";
    TempDir d;
    SolveOptions opt;
    opt.allow_exec = true;
    SUBCASE("PostfixIncrement: false(no-overflow)") {
        if (!cc_with_ubsan()) {
            MESSAGE("NOTRUN: no C compiler with UBSan");
            return;
        }
        opt.out = d.path / "o1";
        opt.witness = d.path / "w1.yml";
        auto oc = solve(sv / "signedintegeroverflow-regression" / "PostfixIncrement.i", props_dir() / "no-overflow.prp", opt);
        INFO(oc.decision.reason);
        CHECK(oc.decision.answer == "false(no-overflow)");
        REQUIRE(oc.witness_path.has_value());
        auto text = slurp(*oc.witness_path);
        CHECK(text.find("entry_type: violation_sequence") != std::string::npos);
        auto doc = prism::taskdef::parse_yaml(text);
        REQUIRE(doc.value.has_value());
        CHECK((*doc.value)[0]["content"].back()["segment"][0]["waypoint"]["type"] == "target");
        // without --allow-exec the same refutation is unknown (Law 9)
        opt.allow_exec = false;
        opt.out = d.path / "o1b";
        oc = solve(sv / "signedintegeroverflow-regression" / "PostfixIncrement.i", props_dir() / "no-overflow.prp", opt);
        CHECK(oc.decision.answer == "unknown");
        CHECK(oc.decision.reason.find("notrun: replay executes task code") != std::string::npos);
    }
    SUBCASE("nested_1: true") {
        opt.out = d.path / "o2";
        opt.witness = d.path / "w2.yml";
        auto oc = solve(sv / "loop-simple" / "nested_1.c", props_dir() / "no-overflow.prp", opt);
        INFO(oc.decision.reason);
        CHECK(oc.decision.answer == "true");
        // a `true` answer carries a correctness witness (format 2.0 invariant_set)
        REQUIRE(oc.witness_path.has_value());
        CHECK(slurp(*oc.witness_path).find("entry_type: invariant_set") != std::string::npos);
    }
}

// ---------------------------------------------------------------- BenchExec tool-info module

TEST_CASE("svcomp: the BenchExec tool-info module runs `prism svcomp` and knows every answer it prints") {
    // tools/svcomp/prism.py is BenchExec's plugin interface (Python by
    // necessity); CI runs benchexec.test_tool_info on it. Here: its contract
    // with this subcommand.
    const std::string mod = slurp(repo_root() / "tools" / "svcomp" / "prism.py");
    REQUIRE_FALSE(mod.empty());
    CHECK(mod.find(R"(REQUIRED_PATHS = ["prism"])") != std::string::npos);
    CHECK(mod.find(R"(tool_locator.find_executable("prism"))") != std::string::npos);
    CHECK(mod.find(R"(cmd = [executable, "svcomp", *options])") != std::string::npos);
    CHECK(mod.find(R"("--prop")") != std::string::npos);
    CHECK(mod.find(R"("--data-model")") != std::string::npos);
    CHECK(mod.find(std::string("RESULT_PREFIX = \"") + RESULT_PREFIX + "\"") != std::string::npos);
    for (const char* answer : {"true", "false(no-overflow)", "false(unreach-call)", "false(valid-deref)",
                               "false(valid-free)", "unknown"})
        CHECK(mod.find(std::string("\"") + answer + "\":") != std::string::npos);
    // `prism --version` is what version() parses ("prism X (C++ engine)")
    CHECK(mod.find(R"(line_prefix="prism ")") != std::string::npos);
}

// ---------------------------------------------------------------- archive

TEST_CASE("svcomp pack: the tool archive") {
    prism::Config cfg;
    auto lister = cfg.which({"tar"});
    if (!cfg.which({"cmake"}) && !lister) {
        MESSAGE("NOTRUN: neither cmake nor tar on PATH");
        return;
    }
    TempDir d;
    auto fake = d.write("prism", "#!/bin/sh\necho 'prism 0.1.0 (C++ engine)'\n");
    fs::permissions(fake, fs::perms(0755), fs::perm_options::replace);
    const fs::path out = d.path / "prism-svcomp.tar.gz";
    auto res = pack(fake, repo_root(), out);
    INFO(res.error);
    REQUIRE(res.ok);
    CHECK(fs::is_regular_file(out));
    CHECK(res.manifest["prism_version"] == "0.1.0");
    CHECK(res.manifest["format_version"] == 2);
    const std::vector<std::string> want = {"LICENSE", "MANIFEST.json", "README.md", "dependencies.json",
                                           "fm-tools.yml", "prism", "prism.py"};
    CHECK(res.manifest["files"].get<std::vector<std::string>>() == want);
    CHECK(res.manifest["dependencies"].contains("clang"));
    CHECK(res.manifest["dependencies"].contains("prism_env"));
    CHECK_FALSE(res.manifest["dependencies"].contains("python"));
    if (!lister) return;
    auto ls = prism::detail::run_process({lister->string(), "tzvf", out.string()}, 60.0);
    for (const auto& name : want) CHECK(ls.text.find("prism/" + name) != std::string::npos);
    CHECK(ls.text.find("rwxr-xr-x") != std::string::npos);  // the binary stays executable
    fs::create_directories(d.path / "x");
    prism::detail::run_process({lister->string(), "xzf", out.string(), "-C", (d.path / "x").string()}, 60.0);
    auto deps = json::parse(slurp(d.path / "x" / "prism" / "dependencies.json"));
    CHECK(deps.contains("clang"));
    CHECK(deps.contains("prism_env"));
    CHECK(json::parse(slurp(d.path / "x" / "prism" / "MANIFEST.json"))["files"].size() == want.size());
    // a missing binary is an error, not an archive
    CHECK_FALSE(pack(d.path / "nope", repo_root(), d.path / "n.tar.gz").ok);
}

// ---------------------------------------------------------------- session runner

#if defined(__linux__)
TEST_CASE("run_session: a timeout kills the whole session, also a child in its own process group") {
    TempDir d;
    const fs::path pidfile = d.path / "solver.pid";
    // a stand-in for PRISM: starts a "solver" in a process group of its own
    // (as the solver runner does), writes its pid, then waits
    const std::string script = "setsid sleep 60 & echo $! > '" + pidfile.string() +
                               "'; exec sleep 60";
    // setsid would leave the session; use a new process group instead
    const std::string pg_script = "(set -m; sleep 60 & echo $! > '" + pidfile.string() + "'; wait) & exec sleep 60";
    (void)script;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = prism::detail::run_session({"/bin/sh", "-c", pg_script}, 2.0);
    CHECK(r.timed_out);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
    long solver = 0;
    for (int k = 0; k < 100 && solver == 0; ++k) {
        std::ifstream in(pidfile);
        in >> solver;
        if (!solver) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(solver > 1);
    bool gone = false;
    for (int k = 0; k < 100 && !gone; ++k) {
        std::ifstream st("/proc/" + std::to_string(solver) + "/stat");
        std::string s((std::istreambuf_iterator<char>(st)), std::istreambuf_iterator<char>());
        auto close = s.rfind(')');
        gone = !st.good() && s.empty();
        if (!gone && close != std::string::npos && close + 2 < s.size() && s[close + 2] == 'Z') gone = true;
        if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!gone) ::kill(static_cast<pid_t>(solver), SIGKILL);
    CHECK(gone);
}

TEST_CASE("run_session: normal exit returns stdout and the exit status") {
    auto r = prism::detail::run_session({"/bin/sh", "-c", "echo ok; echo err >&2; exit 3"}, 30.0);
    CHECK_FALSE(r.timed_out);
    CHECK(r.rc == 3);
    CHECK(r.out == "ok\n");
}
#endif
