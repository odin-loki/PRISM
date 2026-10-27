// Lock for prism::svcomp::decide (src/prism/svcomp/decide.cpp).
//
// true: PROVED, PROVED-UNBOUNDED or PROVED-CERTIFIED of main, from bmc or
// pir, on a property that stage covers (no-overflow, unreach-call).
// false(<prop>): FAILED of that property's class whose counterexample
// replayed. Replay is the confirmed-violation rule; without --allow-exec
// it returns "notrun" and the answer stays unknown.
// unknown: BOUNDED, PROVED-ASSUMING, HYPOTHESIS, READS, NOTRUN, TIMEOUT,
// CLEAN, a fuzzer row, a stage that did not run, valid-memsafety proofs,
// and a FAILED whose replay did not confirm (including a replay timeout).
// docs/finish-notes/svcomp-decide.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/svcomp.hpp"

#include <string>
#include <utility>
#include <vector>

using prism::svcomp::decide;
using prism::svcomp::json;
namespace laws = prism::laws;

namespace {

json row(std::string_view status, std::string_view cls = "", std::string_view message = "",
         std::string_view function = "main") {
    return json{{"function", std::string(function)},
                {"status", std::string(status)},
                {"cls", std::string(cls)},
                {"message", std::string(message)}};
}

json report(std::initializer_list<std::pair<std::string, std::vector<json>>> stages) {
    json rep = {{"stages", json::array()}};
    for (const auto& [name, findings] : stages) {
        json rows = json::array();
        for (const auto& f : findings) rows.push_back(f);
        rep["stages"].push_back({{"name", name}, {"status", "ok"}, {"findings", rows}});
    }
    return rep;
}

json confirmed(const json&) {
    return json{{"replay", "replayed"}, {"detail", "UBSan: signed integer overflow"}};
}

// Law 9: replay executes task code. Without --allow-exec the replay result
// is "notrun" and decide must not answer false.
json allow_exec_off(const json&) {
    return json{{"replay", "notrun"},
                {"why", "replay executes task code: re-run with --allow-exec (Law 9)"}};
}

json replay_timeout(const json&) {
    return json{{"replay", "not-replayed"}, {"why", "program did not finish in 10s"}};
}

json overflow_cex() {
    json f = row(laws::FAILED, "INT-SIGNED-OVF", "signed overflow");
    f["counterexample"] = "n=2147483647";
    f["extra"] = json{{"nondet", "n=2147483647"}};
    return f;
}

}  // namespace

TEST_CASE("svcomp decide: PROVED of main on bmc or pir is true") {
    for (const char* stage : {"bmc", "pir"}) {
        for (const auto status : {laws::PROVED, laws::PROVED_UNBOUNDED, laws::PROVED_CERTIFIED}) {
            INFO(stage << " " << status);
            for (const char* prop : {"no-overflow", "unreach-call"}) {
                INFO(prop);
                auto d = decide(report({{stage, {row(status)}}}), prop, confirmed);
                CHECK(d.answer == "true");
                REQUIRE(d.finding.has_value());
                CHECK((*d.finding)["stage"] == stage);
            }
        }
    }
    // pir did not run. bmc's proof still covers the property.
    json pir_out = {{"name", "pir"}, {"status", std::string(laws::NOTRUN)}, {"findings", json::array()}};
    json bmc_proved = {{"name", "bmc"},
                       {"status", "ok"},
                       {"findings", json::array({row(laws::PROVED, "", "main proved")})}};
    auto d = decide(json{{"stages", json::array({pir_out, bmc_proved})}}, "no-overflow", confirmed);
    CHECK(d.answer == "true");
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "bmc");

    // A proof of some other function is not a proof of main.
    CHECK(decide(report({{"bmc", {row(laws::PROVED, "", "", "helper")}}}), "no-overflow", confirmed).answer ==
          "unknown");
    // Nothing covers valid-memtrack, so a proof of main is still unknown.
    auto mem = decide(report({{"bmc", {row(laws::PROVED_CERTIFIED)}}, {"pir", {row(laws::PROVED)}}}),
                      "valid-memsafety", confirmed);
    CHECK(mem.answer == "unknown");
}

TEST_CASE("svcomp decide: BOUNDED, HYPOTHESIS, NOTRUN and fuzzer CLEAN stay unknown") {
    for (const auto status : {laws::BOUNDED, laws::PROVED_ASSUMING, laws::HYPOTHESIS, laws::READS, laws::NOTRUN,
                              laws::CLEAN, laws::TIMEOUT, laws::UNKNOWN}) {
        INFO(status);
        auto d = decide(report({{"bmc", {row(status)}}}), "no-overflow", confirmed);
        CHECK(d.answer == "unknown");
        d = decide(report({{"pir", {row(status)}}}), "unreach-call", confirmed);
        CHECK(d.answer == "unknown");
    }
    // A fuzzer CLEAN, including one that mis-labels itself PROVED, is not a verdict stage.
    for (const char* stage : {"fuzz", "libfuzzer"}) {
        INFO(stage);
        CHECK(decide(report({{stage, {row(laws::CLEAN, "", "no crash")}}}), "no-overflow", confirmed).answer ==
              "unknown");
        CHECK(decide(report({{stage, {row(laws::PROVED)}}}), "no-overflow", confirmed).answer == "unknown");
        auto fuzz_fail = row(laws::FAILED, "INT-SIGNED-OVF", "fuzz hit an overflow");
        fuzz_fail["counterexample"] = "n=1";
        CHECK(decide(report({{stage, {fuzz_fail}}}), "no-overflow", confirmed).answer == "unknown");
    }
    // BOUNDED on bmc next to a fuzzer CLEAN is still unknown.
    auto mixed = decide(report({{"fuzz", {row(laws::CLEAN)}}, {"bmc", {row(laws::BOUNDED)}}}), "no-overflow", confirmed);
    CHECK(mixed.answer == "unknown");
    CHECK(mixed.reason.find("bmc: BOUNDED") != std::string::npos);
}

TEST_CASE("svcomp decide: FAILED with a replayed counterexample is false") {
    auto d = decide(report({{"bmc", {overflow_cex()}}}), "no-overflow", confirmed);
    CHECK(d.answer == "false(no-overflow)");
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "bmc");
    CHECK(d.reason.find("counterexample replayed") != std::string::npos);

    json reach = row(laws::FAILED, "FUNC-CONTRACT", "reach_error: FUNC-CONTRACT");
    reach["counterexample"] = "x=1";
    d = decide(report({{"bmc", {reach}}}), "unreach-call", confirmed);
    CHECK(d.answer == "false(unreach-call)");
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "bmc");

    d = decide(report({{"pir", {overflow_cex()}}}), "no-overflow", confirmed);
    CHECK(d.answer == "false(no-overflow)");
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "pir");

    // A confirmed run of a different class does not refute no-overflow.
    json other = row(laws::FAILED, "FUNC-CONTRACT", "reach_error: FUNC-CONTRACT");
    other["counterexample"] = "x=1";
    CHECK(decide(report({{"bmc", {other}}}), "no-overflow", confirmed).answer == "unknown");
}

TEST_CASE("svcomp decide: an unconfirmed counterexample stays unknown") {
    auto d = decide(report({{"bmc", {overflow_cex()}}}), "no-overflow", allow_exec_off);
    CHECK(d.answer == "unknown");
    CHECK(d.reason.find("did not replay (notrun:") != std::string::npos);
    REQUIRE(d.finding.has_value());
    CHECK((*d.finding)["stage"] == "bmc");

    d = decide(report({{"bmc", {overflow_cex()}}}), "no-overflow", replay_timeout);
    CHECK(d.answer == "unknown");
    CHECK(d.reason.find("did not replay (not-replayed:") != std::string::npos);

    // The same FAILED row blocks a proof until the counterexample replays.
    auto both = report({{"bmc", {row(laws::PROVED)}}, {"pir", {overflow_cex()}}});
    CHECK(decide(both, "no-overflow", allow_exec_off).answer == "unknown");
    CHECK(decide(both, "no-overflow", confirmed).answer == "false(no-overflow)");
}

TEST_CASE("svcomp decide: a timeout is unknown") {
    json timed = row(laws::TIMEOUT, "INT-SIGNED-OVF", "bounded model check timed out");
    timed["counterexample"] = "n=1";
    auto d = decide(report({{"bmc", {timed}}}), "no-overflow", confirmed);
    CHECK(d.answer == "unknown");
    CHECK(d.reason.find("bmc: TIMEOUT") != std::string::npos);

    json stage_timeout = {{"name", "bmc"}, {"status", std::string(laws::TIMEOUT)}, {"findings", json::array()}};
    d = decide(json{{"stages", json::array({stage_timeout})}}, "no-overflow", confirmed);
    CHECK(d.answer == "unknown");

    json crashed = {{"name", "bmc"},
                    {"status", "failed"},
                    {"detail", "timeout"},
                    {"findings", json::array()}};
    d = decide(json{{"stages", json::array({crashed})}}, "no-overflow", confirmed);
    CHECK(d.answer == "unknown");
    CHECK(d.reason.find("STAGE-FAILED") != std::string::npos);
}

TEST_CASE("svcomp decide: a stage that did not run stays unknown") {
    CHECK(decide(json::object(), "no-overflow", confirmed).answer == "unknown");
    CHECK(decide(json{{"stages", json::array()}}, "no-overflow", confirmed).reason ==
          "no verdict stage reported main");

    json bmc_out = {{"name", "bmc"}, {"status", std::string(laws::NOTRUN)}, {"findings", json::array()}};
    json pir_out = {{"name", "pir"}, {"status", std::string(laws::NOTRUN)}, {"findings", json::array()}};
    auto d = decide(json{{"stages", json::array({bmc_out, pir_out})}}, "unreach-call", confirmed);
    CHECK(d.answer == "unknown");
    CHECK(d.reason == "no verdict stage reported main");

    // The only row is a NOTRUN finding of main.
    d = decide(report({{"bmc", {row(laws::NOTRUN, "", "bmc did not run")}}}), "no-overflow", confirmed);
    CHECK(d.answer == "unknown");
    CHECK(d.reason.find("bmc: NOTRUN") != std::string::npos);

    // Inventory can be PROVED of main and still leaves the verdict unknown:
    // it is not a verdict stage. bmc's absence is the did-not-run case.
    CHECK(decide(report({{"inventory", {row(laws::PROVED)}}}), "no-overflow", confirmed).answer == "unknown");
}
