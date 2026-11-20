// --pir-input-hash (stage.cpp inkey, docs/PIR.md "Input key"): equal input keys mean the same check, so a verdict
// may be copied between functions with equal keys. These tests change one thing at a time (a callee's body, a
// global's initializer, a struct's layout, a flag, a budget in the environment) and require the key to change; and
// they require copies of one function at other lines or in another file to share the location-free key only.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;
namespace L = prism::laws;

// the runner's settings: automatic pointer harness, harness discharge
prism::Config cfg0() {
    auto cfg = prism::default_config();
    cfg.pir_harness = true;
    cfg.pir_discharge = true;
    return cfg;
}

bool skip_without_frontend() {
    auto cfg = cfg0();
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

struct Key {
    std::string exact, loc_free, scope, status;
};

const fs::path kDir = fs::temp_directory_path() / "prism_pir_input_key_test";

// one unit, every function's keys
std::map<std::string, Key> keys(const std::string& name, const std::string& text, prism::Config cfg) {
    fs::create_directories(kDir);
    std::ofstream(kDir / name, std::ios::binary) << text;
    cfg.root = kDir;
    cfg.jobs = 1;
    cfg.pir_input_hash = true;
    std::map<std::string, Key> m;
    for (auto& f : pp::run_pir({kDir / name}, cfg)) {
        if (!f.function) continue;
        auto get = [&](const char* k) {
            auto it = f.extra.find(k);
            return it == f.extra.end() ? std::string() : it->second;
        };
        m[*f.function] = Key{get("input_key"), get("input_key_loc_free"), get("input_scope"), f.status};
    }
    return m;
}

const char* kBase = R"(struct S { int a; int b; };
int g = 1;
static int helper(int x) { return x + 1; }
int f(int x) { return helper(x) * 2; }
int reads_g(int x) { return g + x; }
int field(struct S *s) { return s->b; }
static int get(struct S *s) { return s->a; }
int use(void) { struct S t = {1, 2}; return get(&t); }
)";

std::string replace(std::string s, const std::string& from, const std::string& to) {
    auto at = s.find(from);
    REQUIRE(at != std::string::npos);
    return s.replace(at, from.size(), to);
}

}  // namespace

TEST_CASE("pir input key: rows are NOTRUN with both keys, and a called static function is unit-scoped") {
    if (skip_without_frontend()) return;
    auto k = keys("ik_base.c", kBase, cfg0());
    REQUIRE(k.contains("f"));
    CHECK(k["f"].status == L::NOTRUN);
    CHECK(k["f"].exact.size() == 64);
    CHECK(k["f"].loc_free.size() == 64);
    CHECK(k["f"].exact != k["f"].loc_free);
    CHECK(k["f"].scope == "function");
    // get is static, not address-taken, called in the unit and has an automatic harness: its callers decide the
    // harness discharge, so its key covers the unit
    REQUIRE(k.contains("get"));
    CHECK_MESSAGE(k["get"].scope == "unit:discharge", k["get"].scope);
    // helper is static and called too, but has no harness to discharge
    CHECK_MESSAGE(k["helper"].scope == "function", k["helper"].scope);
    // deterministic: the same unit again gives the same keys
    auto again = keys("ik_base.c", kBase, cfg0());
    CHECK(again["f"].exact == k["f"].exact);
    CHECK(again["f"].loc_free == k["f"].loc_free);
    CHECK(again["get"].exact == k["get"].exact);
}

TEST_CASE("pir input key: a copy at other lines in another file shares only the location-free key") {
    if (skip_without_frontend()) return;
    auto a = keys("ik_copy_a.c", kBase, cfg0());
    auto b = keys("ik_copy_b.c", std::string("\n\n\n/* moved down */\n") + kBase, cfg0());
    for (auto* fn : {"f", "helper", "reads_g", "field"}) {
        CHECK_MESSAGE(a[fn].loc_free == b[fn].loc_free, fn);
        CHECK_MESSAGE(a[fn].exact != b[fn].exact, fn);
    }
    // unit-scoped: the unit's text differs, so no key is shared
    CHECK(a["get"].loc_free != b["get"].loc_free);
}

TEST_CASE("pir input key: one byte of an inlined callee changes the caller's keys") {
    if (skip_without_frontend()) return;
    auto a = keys("ik_callee_a.c", kBase, cfg0());
    auto b = keys("ik_callee_b.c", replace(kBase, "return x + 1;", "return x + 2;"), cfg0());
    CHECK(a["f"].loc_free != b["f"].loc_free);
    CHECK(a["f"].exact != b["f"].exact);
    // functions that do not reach the change keep their location-free key
    CHECK(a["field"].loc_free == b["field"].loc_free);
}

TEST_CASE("pir input key: a global's initializer and a struct's layout are in the key") {
    if (skip_without_frontend()) return;
    auto a = keys("ik_glob_a.c", kBase, cfg0());
    auto b = keys("ik_glob_b.c", replace(kBase, "int g = 1;", "int g = 2;"), cfg0());
    CHECK_MESSAGE(a["reads_g"].loc_free != b["reads_g"].loc_free, "a mutable global's initial value is read by the memory policy's re-check");
    CHECK(a["f"].loc_free == b["f"].loc_free);
    auto c = keys("ik_type_c.c", replace(kBase, "struct S { int a; int b; };", "struct S { long a; int b; };"), cfg0());
    CHECK(a["field"].loc_free != c["field"].loc_free);
}

TEST_CASE("pir input key: flags and budgets are in the key") {
    if (skip_without_frontend()) return;
    auto base = keys("ik_flag.c", kBase, cfg0());
    auto cfg = cfg0();
    cfg.timeout = 31;
    auto t = keys("ik_flag.c", kBase, cfg);
    CHECK(base["f"].exact != t["f"].exact);
    cfg = cfg0();
    cfg.pir_vacuity = true;
    auto v = keys("ik_flag.c", kBase, cfg);
    CHECK(base["f"].exact != v["f"].exact);
    cfg = cfg0();
    cfg.pir_harness_deep = true;   // the harness's depth bound (an assumption of the function)
    auto c = keys("ik_flag.c", kBase, cfg);
    CHECK(base["field"].exact != c["field"].exact);
    cfg = cfg0();
    cfg.pir_harness = false;   // the pointer parameter is then not translated: no key at all, never a shared one
    auto h = keys("ik_flag.c", kBase, cfg);
    CHECK(base["field"].exact.size() == 64);
    CHECK(base["field"].exact != h["field"].exact);
#ifndef _WIN32
    ::setenv("PRISM_FUNCTION_BUDGET", "19", 1);
    auto e = keys("ik_flag.c", kBase, cfg0());
    ::unsetenv("PRISM_FUNCTION_BUDGET");
    CHECK(base["f"].exact != e["f"].exact);
    CHECK(base["f"].loc_free != e["f"].loc_free);
#endif
    fs::remove_all(kDir);
}
