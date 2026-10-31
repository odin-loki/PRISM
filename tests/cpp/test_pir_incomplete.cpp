// Soundness of __prism_incomplete (translate.cpp): a model path PRISM does not
// follow. Reaching it may never yield PROVED (the path is not proved away)
// nor FAILED (a violation after it is not a counterexample PRISM checked).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace pp = prism::pir;

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}
}  // namespace

TEST_CASE("pir: __prism_incomplete reached is never PROVED and never FAILED") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const auto dir = fs::temp_directory_path() / ("prism_incomplete_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    std::ofstream(dir / "inc.c") << R"(void __prism_incomplete(const char *msg);
int inc_reached(int x) { if (x == 5) __prism_incomplete("m"); return 0; }
int inc_then_bug(int x) { if (x == 5) { __prism_incomplete("m"); return 1 / (x - 5); } return 0; }
int inc_always(int x) { __prism_incomplete("m"); return 1 / (x - x); }
int inc_in_loop(int x) { int s = 0; for (int i = 0; i < x && i < 20; i++) { if (i == 15) __prism_incomplete("m"); s += i; } return s; }
int inc_unreached(int x) { if (x > 5 && x < 3) __prism_incomplete("m"); return 0; }
int inc_bug_elsewhere(int x) { if (x == 5) __prism_incomplete("m"); return 10 / x; }
)";
    cfg.root = dir;
    cfg.jobs = 1;
    std::map<std::string, prism::Finding> m;
    for (auto& f : pp::run_pir({dir / "inc.c"}, cfg))
        if (f.function) m[*f.function] = f;
    fs::remove_all(dir);
    for (const char* fn : {"inc_reached", "inc_then_bug", "inc_always", "inc_in_loop"}) {
        CAPTURE(fn);
        REQUIRE(m.count(fn));
        const auto& st = m[fn].status;
        CAPTURE(st);
        CHECK_FALSE(st.starts_with("PROVED"));
        CHECK(st != prism::laws::FAILED);
        CHECK(m[fn].counterexample.empty());
    }
    CHECK(m["inc_reached"].status == prism::laws::NEEDS_HARNESS);
    CHECK(m["inc_reached"].message.find("model incomplete: m") != std::string::npos);
    // Not reachable: no obstacle to a proof.
    REQUIRE(m.count("inc_unreached"));
    CHECK(m["inc_unreached"].status.starts_with("PROVED"));
    // A violation on a path that does not reach it stays a real FAILED.
    REQUIRE(m.count("inc_bug_elsewhere"));
    CHECK(m["inc_bug_elsewhere"].status == prism::laws::FAILED);
}
