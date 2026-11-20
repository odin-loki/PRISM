// Doctests: loop proofs by CHC (Z3 Spacer) and data-driven candidates
// (src/prism/pir/chc.inc, docs/PIR.md "Loop proofs by CHC (Spacer)").
// Spacer's invariant is only a proposal: Houdini's base, step and final
// queries re-check it, a CHC counterexample is replayed by the bounded
// encoding, and on FAILED functions the CHC system must not be "safe"
// (the over-approximation test).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

namespace pp = prism::pir;

bool chc_skip() {
    std::string why;
    if (!pp::frontend_usable(prism::default_config(), &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

void chc_setenv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (v && *v) ::setenv(k, v, 1);
    else ::unsetenv(k);
#endif
}

struct EnvGuard {
    std::string k;
    EnvGuard(const char* key, const char* v) : k(key) { chc_setenv(key, v); }
    ~EnvGuard() { chc_setenv(k.c_str(), ""); }
};

std::map<std::string, pp::Function> chc_translate(const std::filesystem::path& src) {
    std::map<std::string, pp::Function> out;
    auto fe = pp::find_frontend(prism::default_config());
    std::string err;
    std::vector<pp::FoldedUb> folded;
    std::vector<std::pair<int, int>> sshl;
    auto ir = pp::lower_to_ir(fe, src, 60, err, &folded, &sshl);
    REQUIRE_MESSAGE(ir.has_value(), err);
    auto mod = pp::ir::parse_module(*ir);
    pp::TranslateOptions topt;
    topt.signed_shl = sshl;
    topt.folded = folded;
    for (auto& f : mod.functions) {
        auto tr = pp::translate(mod, f, topt);
        if (tr.fn) out.emplace(f.name, *tr.fn);
    }
    return out;
}

// x + y == i is a three-term invariant: no template of the generic family
// or the idiom library states it; the data-driven candidates do (Spacer over
// 32-bit vectors does not find it in 60 s). sp_*: invariants Spacer finds.
const char* kChcSrc = R"(#include <assert.h>
int split_ok(unsigned n, unsigned pat) {
    if (n > 100000) return 0;
    unsigned i = 0, x = 0, y = 0;
    while (i < n) {
        if ((pat >> (i & 31)) & 1) x++; else y++;
        i++;
    }
    assert(x + y == n);
    return 0;
}
int split_bad(unsigned n, unsigned pat) {
    if (n > 100000) return 0;
    unsigned i = 0, x = 0, y = 0;
    while (i < n) {
        if ((pat >> (i & 31)) & 1) x++; else y++;
        i++;
    }
    assert(x + y == n || n < 40);
    assert(x != 37);
    return 0;
}
int two_loops_ok(unsigned n, unsigned pat) {
    if (n > 100000) return 0;
    unsigned i = 0, x = 0, y = 0;
    while (i < n) {
        if ((pat >> (i & 31)) & 1) x++; else y++;
        i++;
    }
    unsigned j = 0, z = 0;
    while (j < x) { z += 3; j++; }
    assert(x + y == n);
    assert(z == 3 * x);
    return 0;
}
int sp_flag(int n) {
    int i = 0, f = 0, g = 1;
    while (i < n) { f = 1 - f; g = 1 - g; i++; }
    assert(f + g == 1);
    return 0;
}
int sp_flag_bad(int n) {
    int i = 0, f = 0, g = 1;
    while (i < n) { f = 1 - f; g = 1 - g; i++; }
    assert(f + g == 1 && f == 0);
    return 0;
}
int sp_two(unsigned n) {
    if (n > 1000) return 0;
    unsigned i = 0, a = 0, b = 0;
    while (i < n) { a += 2; b += 1; i++; }
    assert(a == 2 * b);
    return 0;
}
int two_loops_bad(unsigned n, unsigned pat) {
    if (n > 100000) return 0;
    unsigned i = 0, x = 0, y = 0;
    while (i < n) {
        if ((pat >> (i & 31)) & 1) x++; else y++;
        i++;
    }
    unsigned j = 0, z = 0;
    while (j < y) { z += 3; j++; }
    assert(z != 3 * 33);
    return 0;
}
)";

pp::CheckOptions chc_opts() {
    pp::CheckOptions o;
    o.unwind = 8;
    o.timeout_s = 20;
    o.use_cache = false;
    return o;
}

}  // namespace

TEST_CASE("pir chc: Spacer's invariant proves only after Houdini's re-check; false twins are not proved") {
    if (chc_skip()) return;
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_chc";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "chc.c") << kChcSrc;
    auto fns = chc_translate(dir / "chc.c");
    // the CHC round alone (no template search): Spacer finds the invariant,
    // Houdini's base, step and final queries prove it
    EnvGuard on("PRISM_PIR_CHC", "only");
    EnvGuard to("PRISM_PIR_CHC_TIMEOUT", "20");
    for (const char* name : {"sp_flag", "sp_two"}) {
        CAPTURE(name);
        REQUIRE(fns.count(name));
        auto v = pp::check_function(fns.at(name), chc_opts());
        CAPTURE(v.message);
        CAPTURE(v.extra["chc"]);
        CAPTURE(v.extra["chc_note"]);
        CHECK(v.status == prism::laws::PROVED_UNBOUNDED);
        CHECK(v.extra["invariant_engine"] == "spacer");
        CHECK(v.extra["k_induction"] == "closed-invariants");
        CHECK(v.extra["chc"].rfind("invariant found", 0) == 0);
    }
    for (const char* name : {"sp_flag_bad", "split_bad", "two_loops_bad"}) {
        CAPTURE(name);
        auto v = pp::check_function(fns.at(name), chc_opts());
        CAPTURE(v.message);
        CAPTURE(v.extra["chc"]);
        CHECK(v.status != prism::laws::PROVED_UNBOUNDED);
        CHECK(v.status != prism::laws::PROVED);
    }
}

TEST_CASE("pir chc: data-driven candidates (GPU when built with CUDA) prove a three-term invariant; never assumed") {
    if (chc_skip()) return;
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_chc_data";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "chc.c") << kChcSrc;
    auto fns = chc_translate(dir / "chc.c");
    {
        // the templates alone do not prove x + y == i
        auto v = pp::check_function(fns.at("split_ok"), chc_opts());
        CAPTURE(v.message);
        CHECK(v.status == prism::laws::BOUNDED);
    }
    EnvGuard on("PRISM_PIR_CHC", "data");
    EnvGuard to("PRISM_PIR_CHC_TIMEOUT", "10");
    EnvGuard dv("PRISM_PIR_CHC_DEVICE", "both");
    for (const char* name : {"split_ok", "two_loops_ok"}) {
        CAPTURE(name);
        auto v = pp::check_function(fns.at(name), chc_opts());
        CAPTURE(v.message);
        CAPTURE(v.extra["chc_data"]);
        CAPTURE(v.extra["chc_note"]);
        CHECK(v.status == prism::laws::PROVED_UNBOUNDED);
        CHECK(v.extra["invariant_engine"] == "data");
        CHECK(v.extra["chc_data"].find("DIFFER") == std::string::npos);
        CHECK(v.extra["pir_invariants"].find("== 0 [int]") != std::string::npos);
    }
    for (const char* name : {"split_bad", "two_loops_bad", "sp_flag_bad"}) {
        CAPTURE(name);
        auto b = pp::check_function(fns.at(name), chc_opts());
        CAPTURE(b.message);
        CHECK(b.status != prism::laws::PROVED_UNBOUNDED);
        CHECK(b.status != prism::laws::PROVED);
    }
}

TEST_CASE("pir chc: on FAILED functions the CHC system is never solved safe (over-approximation)") {
    if (chc_skip()) return;
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_chc_diff";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "chc.c") << kChcSrc;
    auto fns = chc_translate(dir / "chc.c");
    EnvGuard on("PRISM_PIR_CHC_DIFF", "1");
    EnvGuard to("PRISM_PIR_CHC_TIMEOUT", "30");
    for (const char* name : {"split_bad", "two_loops_bad", "sp_flag_bad"}) {
        CAPTURE(name);
        auto o = chc_opts();
        o.deepen_max = 64;
        o.unwind = 64;  // the violation is deep: the bounded encoding finds it at this unwind
        auto v = pp::check_function(fns.at(name), o);
        CAPTURE(v.message);
        REQUIRE(v.status == prism::laws::FAILED);
        CAPTURE(v.extra["chc_differential"]);
        CHECK(v.extra["chc_differential"] != "safe");  // never: that would be an under-approximation
        // a shallow counterexample Spacer finds; the deep ones may stay unknown in the time given
        if (std::string(name) == "sp_flag_bad") CHECK(v.extra["chc_differential"] == "cex");
    }
}

TEST_CASE("pir chc: outside loop candidates (PRISM_PIR_LOOP_CANDIDATES) are proved by Houdini or dropped") {
    if (chc_skip()) return;
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_chc_ext";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "chc.c") << kChcSrc;
    auto fns = chc_translate(dir / "chc.c");
    // split_ok's loop header is line 5 of kChcSrc; split_bad's line 15
    std::ofstream(dir / "cands.tsv") << "# function\theader line\tC expression\tsource\n"
                                       "split_ok\t5\tx + y == i\ttest\n"
                                       "split_ok\t5\tx >= 0 && y >= 0\ttest\n"
                                       "split_ok\t5\ti <= n\ttest\n"
                                       "split_ok\t5\tx == i\ttest (false: dropped)\n"
                                       "split_ok\t5\tx +* y\ttest (does not parse)\n"
                                       "split_ok\t5\tnosuchvar == 0\ttest (unknown name)\n"
                                       "split_ok\t99\tx == 0\ttest (no loop there)\n"
                                       "split_bad\t15\tx + y == i\ttest\n"
                                       "split_bad\t15\tx >= 0 && y >= 0 && i <= n\ttest\n"
                                       "sp_flag_bad\t44\tf + g == 1 && f == 0\ttest (not inductive)\n";
    EnvGuard on("PRISM_PIR_LOOP_CANDIDATES", (dir / "cands.tsv").string().c_str());
    {
        auto v = pp::check_function(fns.at("split_ok"), chc_opts());
        CAPTURE(v.message);
        CAPTURE(v.extra["loop_candidates"]);
        CAPTURE(v.extra["chc_note"]);
        CHECK(v.status == prism::laws::PROVED_UNBOUNDED);
        CHECK(v.extra["invariant_engine"] == "outside");
        CHECK(v.extra["loop_candidates"].rfind("7 given, 4 parsed", 0) == 0);
        CHECK(v.extra["pir_invariants"].find("x == i") == std::string::npos);  // the false one was dropped
    }
    for (const char* name : {"split_bad", "sp_flag_bad"}) {
        CAPTURE(name);
        auto v = pp::check_function(fns.at(name), chc_opts());
        CAPTURE(v.message);
        CAPTURE(v.extra["loop_candidates"]);
        CHECK(v.status != prism::laws::PROVED_UNBOUNDED);
        CHECK(v.status != prism::laws::PROVED);
    }
}
