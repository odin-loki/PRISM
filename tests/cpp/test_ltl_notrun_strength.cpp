// run_ltl NOTRUN rows are READS. An empty spec list and a formula outside
// the safety fragment did not run, so they carry STRENGTH_READS. Status
// stays NOTRUN. A G formula that holds on the extracted switch FSM stays
// PROVED with STRENGTH_PROVES. An unbounded F approximation stays BOUNDED.
// docs/finish-notes/ltl-notrun.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

prism::FunctionInfo fsm_step() {
    auto p = fs::path(PRISM_SOURCE_DIR) / "testdata" / "planted" / "fsm.c";
    for (auto& f : prism::extract_functions(p, p.string())) {
        if (f.name == "fsm_step") return f;
    }
    FAIL("missing plant function fsm_step");
    return {};
}

// Same shape as test_ltl.cpp's spec writer: one .ltl file, then run_ltl.
struct SpecDir {
    fs::path dir;
    explicit SpecDir(const std::string& text) {
        dir = fs::temp_directory_path() / "prism_ltl_notrun_strength";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        std::ofstream(dir / "spec.ltl") << text;
    }
    ~SpecDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path path() const { return dir / "spec.ltl"; }
};

}  // namespace

TEST_CASE("ltl NOTRUN: an empty spec list is READS and is not a proof") {
    const std::vector<prism::FunctionInfo> functions;
    const std::vector<fs::path> specs;
    auto recs = prism::run_ltl(functions, specs);
    REQUIRE(recs.size() == 1);
    const auto& f = recs[0];
    CHECK(f.stage == "ltl");
    CHECK(f.status == prism::laws::NOTRUN);
    CHECK(f.status != prism::laws::PROVED);
    CHECK(f.status != prism::laws::CLEAN);
    CHECK(f.strength == prism::laws::STRENGTH_READS);
    CHECK(f.strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message.find("no .ltl spec") != std::string::npos);
    CHECK(f.message == "no .ltl spec next to the sources");
    CHECK(f.extra.at("install") == "add a file with G (...)");
}

TEST_CASE("ltl NOTRUN: a nonsafety formula on fsm_step is READS and is not PROVED") {
    auto fn = fsm_step();
    REQUIRE(fn.name == "fsm_step");
    auto fsm = prism::extract_ltl_fsm(fn.body);
    REQUIRE(fsm.has_value());

    SpecDir written("F (state == ST_IDLE)\n");
    auto recs = prism::run_ltl({fn}, {written.path()});
    REQUIRE(recs.size() == 1);
    const auto& f = recs[0];
    CHECK(f.status == prism::laws::NOTRUN);
    CHECK(f.status != prism::laws::PROVED);
    CHECK(f.status != prism::laws::BOUNDED);
    CHECK(f.status != prism::laws::PROVED_UNBOUNDED);
    CHECK(f.strength == prism::laws::STRENGTH_READS);
    CHECK(f.strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message.find("F (state == ST_IDLE)") != std::string::npos);
    CHECK(f.message.find("not in the safety fragment") != std::string::npos);
    CHECK(f.extra.at("formula") == "F (state == ST_IDLE)");
    CHECK(f.extra.at("strix_not_proved") == "true");
    CHECK(f.extra.at("install") == "https://github.com/meyerphi/strix");
}

TEST_CASE("ltl: a holding G stays PROVED with PROVES; unbounded F stays BOUNDED") {
    auto fn = fsm_step();
    REQUIRE(fn.name == "fsm_step");
    auto fsm = prism::extract_ltl_fsm(fn.body);
    REQUIRE(fsm.has_value());

    auto held = prism::check_ltl_safety(
        "G (state == ST_IDLE -> X (state == ST_IDLE || state == ST_WORK))", *fsm);
    REQUIRE(held.has_value());
    CHECK(held->status == prism::laws::PROVED);
    CHECK(held->strength == prism::laws::STRENGTH_PROVES);
    CHECK(prism::laws::is_proof(held->status));

    auto approx = prism::check_ltl_safety("G (state == ST_IDLE -> F (state == ST_IDLE))", *fsm);
    REQUIRE(approx.has_value());
    CHECK(approx->status == prism::laws::BOUNDED);
    CHECK(approx->status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(approx->status));
    CHECK(approx->extra.at("approx_kind") == "F");
    CHECK(approx->extra.at("strix_not_proved") == "true");
}
