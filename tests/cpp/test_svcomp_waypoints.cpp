// Lock for witness waypoints (src/prism/svcomp/waypoints.cpp).
//
// A waypoint comes from a location on the counterexample. An empty
// counterexample yields no waypoints and no invented target. A missing
// line stays missing: it is not rewritten to line 1. docs/finish-notes/waypoints.md

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/svcomp.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using prism::svcomp::LineCol;
using prism::svcomp::Num;
using prism::svcomp::json;
using prism::svcomp::nondet_locations;
using prism::svcomp::nondet_waypoints;
using prism::svcomp::reach_error_call_site;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
#ifdef _WIN32
        const long pid = static_cast<long>(_getpid());
#else
        const long pid = static_cast<long>(::getpid());
#endif
        path = fs::temp_directory_path() /
               ("prism_svcomp_waypoints_" + std::to_string(pid) + "_" + std::to_string(n++));
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

using Trace = std::vector<std::pair<std::string, Num>>;
using Locs = std::vector<std::optional<LineCol>>;

Trace trace_of(std::initializer_list<std::pair<const char*, long long>> xs) {
    Trace t;
    for (const auto& [n, v] : xs) t.emplace_back(n, Num::of_int(v));
    return t;
}

// Two call sites, neither on line 1. A location picks one; a missing line
// cannot fall back to a single site, and must not invent line 1.
const char* kTwoSites =
    "extern int __VERIFIER_nondet_int(void);\n"
    "int main(void) {\n"
    "  int a = __VERIFIER_nondet_int();\n"
    "  int b = __VERIFIER_nondet_int();\n"
    "  return a + b;\n"
    "}\n";

// The only call is on line 4. A missing location may use that call; it
// must not move the call onto line 1.
const char* kLaterCall =
    "extern int __VERIFIER_nondet_int(void);\n"
    "\n"
    "int main(void) {\n"
    "  return __VERIFIER_nondet_int();\n"
    "}\n";

}  // namespace

TEST_CASE("svcomp waypoints: an empty counterexample yields no waypoints") {
    TempDir d;
    // A call on line 1 would be the fake target if an empty trace were filled in.
    auto src = d.write("empty.c", "int main(void) { return __VERIFIER_nondet_int(); }\n");
    CHECK(nondet_waypoints(src, {}).empty());
    CHECK(nondet_waypoints(src, {}, Locs{}).empty());
    auto later = d.write("later.c", kLaterCall);
    CHECK(nondet_waypoints(later, {}).empty());
    // No reach_error() call: no target site, and not line 1.
    CHECK_FALSE(reach_error_call_site(src, std::nullopt).has_value());
    CHECK_FALSE(nondet_locations(json{{"extra", json::object()}}, 0).has_value());
}

TEST_CASE("svcomp waypoints: one counterexample location keeps its file and line") {
    TempDir d;
    auto src = d.write("placed.c", kTwoSites);
    // Column 11 is the start of __VERIFIER_nondet_int on both call lines.
    // Two sites: without this location the trace places nothing.
    CHECK(nondet_waypoints(src, trace_of({{"__VERIFIER_nondet_int", 7}})).empty());
    auto wps = nondet_waypoints(src, trace_of({{"__VERIFIER_nondet_int", 7}}), Locs{LineCol{4, 11}});
    REQUIRE(wps.size() == 1);
    CHECK(wps[0].location.file_name == "placed.c");
    CHECK(wps[0].location.line == 4);
    CHECK(wps[0].value == Num::of_int(7));

    const char* reach =
        "void reach_error(void);\n"
        "int main(void) {\n"
        "  reach_error();\n"
        "  return 0;\n"
        "}\n";
    auto site = reach_error_call_site(d.write("reach.c", reach), std::nullopt);
    REQUIRE(site.has_value());
    CHECK(site->first == 3);
}

TEST_CASE("svcomp waypoints: a missing line does not become line 1") {
    auto missing = nondet_locations(json{{"extra", {{"nondet_loc", "0:11"}}}}, 1);
    REQUIRE(missing.has_value());
    REQUIRE(missing->size() == 1);
    CHECK_FALSE((*missing)[0].has_value());
    // A line that is not a number is not parsed as line 1.
    CHECK_FALSE(nondet_locations(json{{"extra", {{"nondet_loc", ":11"}}}}, 1).has_value());

    TempDir d;
    auto two = d.write("two.c", kTwoSites);
    auto none = nondet_waypoints(two, trace_of({{"__VERIFIER_nondet_int", 1}}), Locs{std::nullopt});
    CHECK(none.empty());

    auto later = d.write("later.c", kLaterCall);
    auto wps = nondet_waypoints(later, trace_of({{"__VERIFIER_nondet_int", 4}}), Locs{std::nullopt});
    REQUIRE(wps.size() == 1);
    CHECK(wps[0].location.file_name == "later.c");
    CHECK(wps[0].location.line == 4);
    CHECK(wps[0].location.line != 1);
    // Line 0 does not pass call_at, and the unique call is still line 4.
    wps = nondet_waypoints(later, trace_of({{"__VERIFIER_nondet_int", 4}}), Locs{LineCol{0, 1}});
    REQUIRE(wps.size() == 1);
    CHECK(wps[0].location.line == 4);
}
