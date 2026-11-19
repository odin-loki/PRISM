// Doctests: length pairing of the automatic pointer harness (--pir-length-pairing,
// docs/PIR.md "Length pairing") and the depth bound on its own (--pir-harness-depth).
// Soundness: a pairing applies only where the IR shows the bound; a wrong or
// missing bound proves nothing; an off-by-one stays a violation; a larger
// depth never yields PROVED.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace fs = std::filesystem;
namespace L = prism::laws;

bool skip_without_frontend() {
    auto cfg = prism::default_config();
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

std::map<std::string, prism::Finding> run_file(prism::Config cfg, const char* file) {
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    std::map<std::string, prism::Finding> st;
    for (auto& f : pp::run_pir({dir / file}, cfg))
        if (f.function) st[*f.function] = f;
    return st;
}

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

bool paired(const prism::Finding& f) { return extra(f, "assumptions").find("length pairing: ") != std::string::npos; }

std::string show(const std::string& name, const prism::Finding& f) {
    return name + ": " + f.status + " " + f.message + " | " + extra(f, "assumptions");
}

prism::Config pairing_cfg() {
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_length_pairing = true;
    return cfg;
}

}  // namespace

TEST_CASE("pir length pairing: off by default, the automatic harness alone does not size by a count") {
    if (skip_without_frontend()) return;
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    auto st = run_file(cfg, "harness_length.c");
    for (auto* fn : {"sum_buf", "csum_words", "copy_bytes", "pick_checked", "via_helper", "ring_get"}) {
        REQUIRE_MESSAGE(st.contains(fn), fn);
        CHECK_MESSAGE(!paired(st[fn]), show(fn, st[fn]));
        // the default 16 elements do not cover an index up to an arbitrary count
        CHECK_MESSAGE(st[fn].status != L::PROVED_ASSUMING, show(fn, st[fn]));
    }
    for (auto& [name, f] : st) CHECK_MESSAGE(f.status != L::PROVED, show(name, f));
}

TEST_CASE("pir length pairing: a bound the IR shows sizes the object, named, PROVED-ASSUMING at best, non-vacuity checked") {
    if (skip_without_frontend()) return;
    auto st = run_file(pairing_cfg(), "harness_length.c");
    const std::map<std::string, std::string> want{
        {"sum_buf", "length pairing: buf points to at least n elements (inferred from usage)"},
        {"csum_words", "length pairing: data points to at least size elements (inferred from usage)"},
        {"copy_bytes", "length pairing: dst points to at least len elements (inferred from usage)"},
        {"pick_checked", "length pairing: buf points to at least len elements (inferred from usage)"},
        {"via_helper", "length pairing: v points to at least cnt elements (inferred from usage)"},
        {"ring_get", "length pairing: r->@0 points to at least r->@8 elements (inferred from usage)"},
    };
    for (auto& [fn, line] : want) {
        REQUIRE_MESSAGE(st.contains(fn), fn);
        const auto& f = st[fn];
        CHECK_MESSAGE(f.status == L::PROVED_ASSUMING, show(fn, f));
        CHECK_MESSAGE(extra(f, "assumptions").find(line) != std::string::npos, show(fn, f));
        // the assumptions leave an execution (checked even without --pir-vacuity)
        CHECK_MESSAGE(extra(f, "non_vacuity").starts_with("checked"), show(fn, f) + " | " + extra(f, "non_vacuity"));
    }
    CHECK(extra(st["copy_bytes"], "assumptions").find("src points to at least len elements") != std::string::npos);
    // the element size is the indexed type's: 4-byte words of the void * buffer, 2-byte shorts through the helper
    CHECK(extra(st["csum_words"], "assumptions").find("size (unsigned) x 4 bytes") != std::string::npos);
    CHECK(extra(st["via_helper"], "assumptions").find("max(cnt, 0) x 2 bytes") != std::string::npos);
    // nothing in the file is PROVED: a pairing is always an assumption
    for (auto& [name, f] : st) CHECK_MESSAGE(f.status != L::PROVED, show(name, f));
}

TEST_CASE("pir length pairing: SOUNDNESS - an off-by-one under the pairing is still a violation") {
    if (skip_without_frontend()) return;
    auto st = run_file(pairing_cfg(), "harness_length.c");
    // parameter pairs: the pairing is the code's own bound, a read past it is FAILED with the pairing named
    for (auto* fn : {"sum_incl", "sum_next"}) {
        REQUIRE_MESSAGE(st.contains(fn), fn);
        CHECK_MESSAGE(st[fn].status == L::FAILED, show(fn, st[fn]));
        CHECK_MESSAGE(paired(st[fn]), show(fn, st[fn]));
        CHECK_MESSAGE(!st[fn].counterexample.empty(), fn);
    }
    // a field pair lives in an automatic harness object: the violation is FAILED under it, reported NEEDS-HARNESS
    REQUIRE(st.contains("ring_get_incl"));
    CHECK_MESSAGE(st["ring_get_incl"].status == L::NEEDS_HARNESS, show("ring_get_incl", st["ring_get_incl"]));
    CHECK(paired(st["ring_get_incl"]));
    CHECK(extra(st["ring_get_incl"], "verdict_under_draft").starts_with("FAILED"));
}

TEST_CASE("pir length pairing: SOUNDNESS - no bound in the code, the wrong bound or two bounds: no pairing, no proof") {
    if (skip_without_frontend()) return;
    auto st = run_file(pairing_cfg(), "harness_length.c");
    for (auto* fn : {"pick_unchecked", "pick_inverted", "two_bounds", "ring_head"}) {
        REQUIRE_MESSAGE(st.contains(fn), fn);
        CHECK_MESSAGE(!paired(st[fn]), show(fn, st[fn]));
        CHECK_MESSAGE(st[fn].status == L::NEEDS_HARNESS, show(fn, st[fn]));
    }
}

TEST_CASE("pir length pairing: a pairing is not discharged at call sites (the obligations are the fixed-size plan)") {
    if (skip_without_frontend()) return;
    auto cfg = pairing_cfg();
    cfg.pir_discharge = true;
    auto st = run_file(cfg, "harness_length.c");
    for (auto& [name, f] : st) {
        CHECK_MESSAGE(f.status != L::PROVED, show(name, f));
        if (paired(f)) CHECK_MESSAGE(extra(f, "harness_discharge").find("discharged at") == std::string::npos, show(name, f));
    }
}

TEST_CASE("pir harness depth: a depth increase by itself never yields PROVED, and never bounds an unbound pointer") {
    if (skip_without_frontend()) return;
    for (int depth : {0, 1, 2, 3, 4, 5, 8, 16}) {
        auto cfg = prism::default_config();
        cfg.pir_harness = true;
        cfg.pir_harness_depth = depth;
        auto st = run_file(cfg, "harness_depth_only.c");
        const std::string d = "depth " + std::to_string(depth) + " ";
        for (auto* fn : {"chain5", "cookie_far", "walk"}) REQUIRE_MESSAGE(st.contains(fn), d + fn);
        for (auto& [name, f] : st) {
            // never PROVED: the harness is an assumption at every depth
            CHECK_MESSAGE(f.status != L::PROVED, d + show(name, f));
            if (f.status == L::PROVED_ASSUMING)
                CHECK_MESSAGE(extra(f, "assumptions").find("nested pointer depth bound " + std::to_string(depth)) != std::string::npos,
                              d + show(name, f));
        }
        // the read goes through a pointer no depth bounds: never a proof
        CHECK_MESSAGE(st["cookie_far"].status != L::PROVED_ASSUMING, d + show("cookie_far", st["cookie_far"]));
        CHECK_MESSAGE(st["walk"].status != L::PROVED_ASSUMING, d + show("walk", st["walk"]));
        // the fifth link is an object at level 5 (the parameter's own object is level 0)
        if (depth < 5) CHECK_MESSAGE(st["chain5"].status == L::NEEDS_HARNESS, d + show("chain5", st["chain5"]));
    }
}

TEST_CASE("cli: --pir-length-pairing, off by default") {
    std::vector<const char*> on{"fixture.c", "--pir-harness", "--pir-length-pairing"};
    auto r = prism::parse_cli(std::span<const char* const>(on.data(), on.size()));
    REQUIRE(r);
    CHECK(r->cfg.pir_length_pairing);
    std::vector<const char*> off{"fixture.c", "--pir-harness"};
    auto d = prism::parse_cli(std::span<const char* const>(off.data(), off.size()));
    REQUIRE(d);
    CHECK_FALSE(d->cfg.pir_length_pairing);
}
