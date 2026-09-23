#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/cparse.hpp"
#include "prism/regex.hpp"
#include "prism/config.hpp"
#include "prism/stages.hpp"
#include "prism/journal.hpp"
#include "prism/pipeline.hpp"
#include "prism/sandbox.hpp"
#include "prism/taxonomy.hpp"
#include "prism/simd.hpp"
#ifdef PRISM_HAS_CUDA
#include "prism/cuda_mutate.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdlib.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

// testdata lives at <repo>/testdata. Walk from this file so a space in
// "Code Analysis" is never split the way an env var / argv path would be.
static std::filesystem::path testdata_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
           / "testdata";
}

static std::string extra_get(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    REQUIRE(it != f.extra.end());
    return it->second;
}

static prism::FunctionInfo load_fn(const char* file, const char* name) {
    auto td = testdata_root() / file;
    // Store the real path: WP/rapid re-read // ensures: from fn.file, not cwd.
    auto fns = prism::extract_functions(td, td.string());
    for (auto& f : fns)
        if (f.name == name) return f;
    FAIL("missing function");
    return {};
}

static std::string compact_cex(std::string s) {
    std::string out;
    for (char c : s)
        if (c != ' ') out += c;
    return out;
}

TEST_CASE("laws refuse merging proofs") {
    using namespace prism::laws;
    CHECK(is_proof(PROVED_UNBOUNDED));
    CHECK_FALSE(is_proof(BOUNDED));
    CHECK_THROWS(refuse_merge(PROVED, BOUNDED));
    CHECK_THROWS(refuse_merge(CLEAN, PROVED));
    refuse_merge(FAILED, FAILED);
}

TEST_CASE("pcre2 named groups") {
    prism::Regex re("(?P<p>[A-Za-z_]+)\\s*=\\s*realloc");
    auto m = re.search_match("p = realloc(p, n)");
    REQUIRE(m);
    CHECK(m->named("p") == "p");
}

TEST_CASE("strip comments keeps lines") {
    auto s = prism::strip_comments_keep_lines("int x; // c\nint y;");
    CHECK(s.find('\n') != std::string::npos);
    CHECK(s.find("int y") != std::string::npos);
}

TEST_CASE("designated-init regex stays exact") {
    // PLAN: must not match `{ dst[i] =`
    prism::Regex re("\\{\\s*(?:\\[[^\\]]+\\]|\\.[A-Za-z_]\\w*)\\s*=", true);
    CHECK(re.search("{ .x = 1 }"));
    CHECK(re.search("{ [0] = 1 }"));
    CHECK_FALSE(re.search("{ dst[i] = 1 }"));
}

TEST_CASE("hypothesize missing GGUF is NOTRUN") {
    prism::Config cfg = prism::default_config();
    cfg.gguf = "/nonexistent/prism-missing.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    auto fn = load_fn("abs_ok.c", "abs_ok");
    auto findings = prism::hypothesize({fn}, 1, cfg);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::NOTRUN));
    CHECK(findings[0].status != std::string(prism::laws::CLEAN));
}

TEST_CASE("rapid shrinks extra to boundary") {
    auto fn = load_fn("shrink_ge.c", "shrink_ge");
    auto findings = prism::run_rapid({fn}, 64);
    REQUIRE_FALSE(findings.empty());
    auto& r = findings[0];
    CHECK(r.status == std::string(prism::laws::FAILED));
    CHECK(r.status != std::string(prism::laws::PROVED));
    CHECK(compact_cex(r.counterexample).find("x=10") != std::string::npos);
    int shrinks = 0;
    auto it = r.extra.find("shrinks");
    if (it != r.extra.end() && !it->second.empty())
        shrinks = std::stoi(it->second);
    CHECK(shrinks >= 1);
}

TEST_CASE("taint fread and strcat") {
    auto fns = prism::extract_functions(testdata_root() / "taint_fread.c", "taint_fread.c");
    auto hits = prism::run_taint(fns);
    bool fread_hit = false;
    bool strcat_hit = false;
    for (auto& f : hits) {
        if (f.function == "run_fread") {
            fread_hit = true;
            CHECK(f.status == std::string(prism::laws::FAILED));
            CHECK(f.cls == "TAINT-SINK");
        }
        if (f.function == "run_strcat") {
            strcat_hit = true;
            CHECK(f.message.find("strcat") != std::string::npos);
        }
    }
    CHECK(fread_hit);
    CHECK(strcat_hit);
}

TEST_CASE("one-line return is not missing-return") {
    auto root = testdata_root();
    auto iso_fns = prism::extract_functions(root / "iso_thread_race.c", "iso_thread_race.c");
    bool t1 = false, t2 = false;
    for (auto& fn : iso_fns) {
        if (fn.name == "iso_thrd_t1") t1 = true;
        if (fn.name == "iso_thrd_t2") t2 = true;
    }
    REQUIRE(t1);
    REQUIRE(t2);
    auto iso = prism::run_lints({root / "iso_thread_race.c"}, root);
    for (auto& f : iso) {
        if (f.cls != "CTRL-MISSING-RETURN") continue;
        CHECK(f.function != "iso_thrd_t1");
        CHECK(f.function != "iso_thrd_t2");
    }

    auto miss = prism::run_lints({root / "missing_return.c"}, root);
    bool bad = false, oneline_bad = false, ok = false, oneline_ok = false;
    for (auto& f : miss) {
        if (f.cls != "CTRL-MISSING-RETURN") continue;
        if (f.function == "missing_return_bad") bad = true;
        if (f.function == "missing_return_oneline_bad") oneline_bad = true;
        if (f.function == "missing_return_ok") ok = true;
        if (f.function == "missing_return_oneline_ok") oneline_ok = true;
    }
    CHECK(bad);
    CHECK(oneline_bad);
    CHECK_FALSE(ok);
    CHECK_FALSE(oneline_ok);
}

// testdata_fp/ (correct code in the idioms the lints used to misread) and
// testdata_tp/ (the buggy twins) sit beside testdata/. Same corpus and
// expectations as tests/test_false_positives.py.
static std::filesystem::path repo_dir(const char* name) {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / name;
}

static std::vector<std::string> fn_names(const std::filesystem::path& p) {
    std::vector<std::string> out;
    for (auto& f : prism::extract_functions(p, p.filename().string())) out.push_back(f.name);
    return out;
}

static bool has_hit(const std::vector<prism::Finding>& fs, int line, const char* cls) {
    for (auto& f : fs)
        if (f.status == prism::laws::FAILED && f.line && *f.line == line && f.cls == cls) return true;
    return false;
}

TEST_CASE("false-positive corpus: zero FAILED lint findings") {
    auto root = repo_dir("testdata_fp");
    auto srcs = prism::iter_sources(root);
    CHECK(srcs.size() >= 20);
    for (auto& f : prism::run_lints(srcs, root)) {
        INFO(f.file << ":" << f.line.value_or(0) << " " << f.cls << " " << f.message);
        CHECK(f.status != prism::laws::FAILED);
    }
    for (auto& p : srcs) {
        INFO(p.filename().string());
        CHECK(prism::parse_gaps(p).empty());
        CHECK_FALSE(prism::extract_functions(p).empty());
    }
}

TEST_CASE("true-positive twins still fire") {
    auto root = repo_dir("testdata_tp");
    auto lint = [&](const char* name) { return prism::run_lints({root / name}, root); };
    auto uaf = lint("uaf_paths.c");
    CHECK(has_hit(uaf, 13, "MEM-UAF"));
    CHECK(has_hit(uaf, 25, "MEM-UAF"));
    CHECK(has_hit(uaf, 44, "MEM-UAF"));
    CHECK(has_hit(uaf, 55, "MEM-DOUBLE-FREE"));
    CHECK_FALSE(has_hit(uaf, 55, "MEM-UAF"));
    CHECK(has_hit(lint("lock_twice.c"), 15, "LOCK-DOUBLE-UNLOCK"));
    auto leaks = lint("leaks.c");
    CHECK(has_hit(leaks, 12, "MEM-LEAK"));
    CHECK(has_hit(leaks, 23, "RES-FD-LEAK"));
    auto api = lint("api_and_fmt.c");
    CHECK(has_hit(api, 11, "API-GETS"));
    CHECK_FALSE(has_hit(api, 10, "API-GETS"));
    CHECK(has_hit(api, 18, "FMT-STRING"));
    CHECK(has_hit(api, 19, "FMT-STRING"));
    CHECK_FALSE(has_hit(api, 17, "FMT-STRING"));
    auto alloc = lint("unchecked_alloc.c");
    CHECK(has_hit(alloc, 6, "PTR-UNCHECKED-ALLOC"));
    CHECK(has_hit(alloc, 12, "PTR-UNCHECKED-ALLOC"));
    auto cpy = lint("strcpy_escape.c");
    CHECK(has_hit(cpy, 7, "STR-OFF-BY-ONE"));
    CHECK_FALSE(has_hit(cpy, 14, "STR-OFF-BY-ONE"));
    bool go = false, inl = false, inner = false;
    for (auto& f : lint("methods_uaf.cpp")) {
        if (f.cls != "MEM-UAF") continue;
        go = go || (f.function == "W::go" && f.line == 21);
        inl = inl || (f.function == "W::inl" && f.line == 11);
        inner = inner || (f.function == "inner" && f.line == 30);
    }
    CHECK(go);
    CHECK(inl);
    CHECK(inner);
}

TEST_CASE("parser forms and PARSE-GAP") {
    auto fp = repo_dir("testdata_fp");
    CHECK(fn_names(fp / "methods.cpp") ==
          std::vector<std::string>{"Buffer::Buffer", "Buffer::~Buffer", "Buffer::ok", "Buffer::size",
                                   "Buffer::fill", "use_buffer"});
    CHECK(fn_names(fp / "namespaced.cpp") ==
          std::vector<std::string>{"clamp_to", "twice", "Point::operator==", "label"});
    CHECK(fn_names(fp / "knr_style.c") == std::vector<std::string>{"knr_add", "knr_first"});
    CHECK(fn_names(fp / "attr_static_inline.c") ==
          std::vector<std::string>{"attr_first", "sinl", "pick", "run_pick"});
    std::map<std::string, std::string> kinds;
    for (auto& f : prism::extract_functions(fp / "attr_static_inline.c")) kinds[f.name] = f.kind;
    CHECK(kinds["pick"] == "OTHER");
    CHECK(kinds["attr_first"] == "SCALAR");
    for (auto& f : prism::extract_functions(fp / "knr_style.c")) CHECK(f.kind == "OTHER");

    auto gap = repo_dir("testdata_tp") / "parse_gap.c";
    auto gaps = prism::parse_gaps(gap);
    REQUIRE(gaps.size() == 1);
    CHECK(gaps[0].first == 8);
    auto recs = prism::parse_gap_findings(gap, "parse_gap.c");
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].stage == "inventory");
    CHECK(recs[0].status == prism::laws::NOTRUN);
    CHECK(recs[0].cls == "PARSE-GAP");
    CHECK(recs[0].message.find("line 8") != std::string::npos);
    // The macro-bodied definition does not swallow the next function.
    CHECK(fn_names(gap) == std::vector<std::string>{"after_macro"});

    // Same source as tests/test_false_positives.py::test_head_shapes.
    auto tmp = std::filesystem::temp_directory_path() / "prism_head_shapes.cpp";
    {
        std::ofstream o(tmp, std::ios::binary);
        o << "std::vector<int> make(int n) {\n    return {};\n}\n"
             "std::map<int, std::vector<int>> nested(void) {\n    return {};\n}\n"
             "Z3_ast Z3_API mk(Z3_context c) {\n    return 0;\n}\n"
             "W& W::operator=(const W& o) {\n    return *this;\n}\n"
             "W::W(int a) : v(a) {\n}\n"
             "struct S { int get() const { return 1; } S() : x(0) {} int x; };\n"
             "int body_char(void) {\n    return '}';\n}\n"
             "int after(void) {\n    return 1;\n}\n";
    }
    auto fns = prism::extract_functions(tmp, "x.cpp");
    std::filesystem::remove(tmp);
    std::vector<std::string> names;
    for (auto& f : fns) names.push_back(f.name);
    CHECK(names == std::vector<std::string>{"make", "nested", "mk", "W::operator=", "W::W", "S::get",
                                            "S::S", "body_char", "after"});
    REQUIRE(fns.size() == 9);
    for (std::size_t k = 0; k < 7; ++k) CHECK(fns[k].kind == "OTHER");
    CHECK(fns[7].span == std::pair<int, int>{16, 18});  // `'}'` does not end the body
}

TEST_CASE("interval shift_wide is INT-SHIFT-UB") {
    auto fn = load_fn("interval_ops.c", "shift_wide");
    auto findings = prism::run_interval({fn});
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::FAILED));
    CHECK(findings[0].cls == "INT-SHIFT-UB");
    CHECK(findings[0].strength == std::string(prism::laws::STRENGTH_FINDS));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::CLEAN));
}

TEST_CASE("interval mod_param is INT-DIV-ZERO") {
    auto findings = prism::run_interval({load_fn("interval_ops.c", "mod_param")});
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::FAILED));
    CHECK(findings[0].cls == "INT-DIV-ZERO");
    CHECK(findings[0].strength == std::string(prism::laws::STRENGTH_FINDS));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
}

TEST_CASE("interval intmin_div is INT-SIGNED-OVF") {
    auto findings = prism::run_interval({load_fn("interval_ops.c", "intmin_div")});
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::FAILED));
    CHECK(findings[0].cls == "INT-SIGNED-OVF");
    CHECK(findings[0].strength == std::string(prism::laws::STRENGTH_FINDS));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
}

TEST_CASE("interval unsigned wrap is not PROVED or CLEAN") {
    for (const char* name : {"wrap_u_local", "wrap_u_branch"}) {
        auto fn = load_fn("interval_ops.c", name);
        auto findings = prism::run_interval({fn});
        CHECK(findings.empty());
        for (auto& f : findings) {
            CHECK(f.status == std::string(prism::laws::FAILED));
            CHECK(f.status != std::string(prism::laws::PROVED));
            CHECK(f.status != std::string(prism::laws::CLEAN));
        }
    }
}

TEST_CASE("interval ops plants never prove") {
    auto fns = prism::extract_functions(testdata_root() / "interval_ops.c", "interval_ops.c");
    auto hits = prism::run_interval(fns);
    bool div0 = false, ovf = false, shift = false;
    for (auto& f : hits) {
        CHECK(f.status == std::string(prism::laws::FAILED));
        CHECK(f.strength == std::string(prism::laws::STRENGTH_FINDS));
        CHECK(f.status != std::string(prism::laws::PROVED));
        CHECK(f.status != std::string(prism::laws::CLEAN));
        if (f.cls == "INT-DIV-ZERO") div0 = true;
        if (f.cls == "INT-SIGNED-OVF") ovf = true;
        if (f.cls == "INT-SHIFT-UB") shift = true;
    }
    CHECK(div0);
    CHECK(ovf);
    CHECK(shift);
}

TEST_CASE("execute_cex replay is not a proof") {
    auto fn = load_fn("div_param.c", "div_param");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = fn.file;
    fail.function = fn.name;
    fail.line = fn.line;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    fail.counterexample = "x=1,y=0";
    prism::Config cfg = prism::default_config();
    cfg.llm = false;
    auto out = prism::execute_cex({fail}, {fn}, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(out[0].stage == "execute");
    CHECK_FALSE(prism::laws::is_proof(out[0].status));
    CHECK(out[0].status != std::string(prism::laws::PROVED));
    CHECK(out[0].status != std::string(prism::laws::BOUNDED));
    CHECK((out[0].status == prism::laws::CRASH || out[0].status == prism::laws::CLEAN));
    if (out[0].status == prism::laws::CLEAN)
        CHECK(out[0].message.find("not a proof") != std::string::npos);
}

TEST_CASE("execute_cex llm down is NOTRUN for llm half") {
    auto fn = load_fn("div_param.c", "div_param");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = fn.file;
    fail.function = fn.name;
    fail.line = fn.line;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    fail.counterexample = "x=1,y=0";
    prism::Config cfg = prism::default_config();
    cfg.llm = true;
    cfg.gguf = "/nonexistent/prism-missing.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    auto out = prism::execute_cex({fail}, {fn}, cfg);
    bool saw_nr = false;
    for (auto& f : out) {
        CHECK_FALSE(prism::laws::is_proof(f.status));
        CHECK(f.status != std::string(prism::laws::PROVED));
        if (f.status == prism::laws::NOTRUN) {
            saw_nr = true;
            CHECK(f.status != std::string(prism::laws::CLEAN));
        }
    }
    CHECK(saw_nr);
}

TEST_CASE("rlef_repair missing llm is NOTRUN never CLEAN") {
    auto td = testdata_root() / "div_param.c";
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = td.string();
    fail.function = std::string("div_param");
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    prism::Config cfg = prism::default_config();
    cfg.gguf = "/nonexistent/prism-missing.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    auto out = prism::rlef_repair(fail, cfg);
    REQUIRE_FALSE(out.empty());
    CHECK(out[0].stage == "repair");
    CHECK(out[0].status == std::string(prism::laws::NOTRUN));
    CHECK(out[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(out[0].status));
}

#ifdef PRISM_HAS_Z3
TEST_CASE("bmc proves abs_ok") {
    auto fn = load_fn("abs_ok.c", "abs_ok");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].function == "abs_ok");
    CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
    CHECK((findings[0].status == prism::laws::PROVED ||
           findings[0].status == prism::laws::PROVED_UNBOUNDED));
}

TEST_CASE("bmc fails div_param") {
    auto fn = load_fn("div_param.c", "div_param");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == prism::laws::FAILED);
    CHECK(findings[0].cls == "INT-DIV-ZERO");
}

TEST_CASE("bmc fails add_overflow") {
    auto fn = load_fn("add_overflow.c", "add_overflow");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == prism::laws::FAILED);
    CHECK(findings[0].cls == "INT-SIGNED-OVF");
}

TEST_CASE("bmc fails oob_write") {
    auto fn = load_fn("oob_write.c", "oob_write");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == prism::laws::FAILED);
}

TEST_CASE("thrd_create is not a vacuous proof") {
    auto fn = load_fn("iso_thread_race.c", "iso_thrd_start");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == prism::laws::NEEDS_HARNESS);
    CHECK(findings[0].message.find("thrd") != std::string::npos);
}

TEST_CASE("wp acsl_abs is PROVED-ASSUMING never PROVED") {
    auto fn = load_fn("acsl_abs.c", "acsl_abs");
    auto findings = prism::run_wp({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].stage == "wp");
    CHECK(findings[0].status == std::string(prism::laws::PROVED_ASSUMING));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    auto engine = findings[0].extra.find("engine");
    REQUIRE(engine != findings[0].extra.end());
    CHECK(engine->second == "prism-wp");
}

TEST_CASE("wp skips functions with no ensures") {
    auto fn = load_fn("acsl_abs.c", "acsl_plain");
    auto findings = prism::run_wp({fn}, 4);
    CHECK(findings.empty());
}

TEST_CASE("wp pointer is NEEDS-HARNESS") {
    auto fn = load_fn("wp_ptr.c", "wp_ptr_get");
    auto findings = prism::run_wp({fn}, 4);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].stage == "wp");
    CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_ASSUMING));
}

TEST_CASE("wp unencodable ACSL is ERROR") {
    for (const char* name : {"wp_valid_bad", "wp_old_bad", "wp_forall_bad"}) {
        auto fn = load_fn("wp_unenc.c", name);
        auto findings = prism::run_wp({fn}, 4);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::ERROR));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_ASSUMING));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        auto it = findings[0].extra.find("wp_unencoded");
        REQUIRE(it != findings[0].extra.end());
        CHECK(it->second == "true");
    }
}
#endif

static void require_decreases_unencoded(const prism::Finding& r) {
    CHECK(r.status == std::string(prism::laws::ERROR));
    CHECK(r.status != std::string(prism::laws::PROVED));
    CHECK(r.status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(r.status != std::string(prism::laws::PROVED_ASSUMING));
    auto it = r.extra.find("decreases_unencoded");
    REQUIRE(it != r.extra.end());
    CHECK(it->second == "true");
}

static prism::FunctionInfo load_contract_fn(const char* file, const char* name) {
    auto fn = load_fn(file, name);
    fn.file = (testdata_root() / file).string();
    return fn;
}

TEST_CASE("compound decreases is ERROR not proved-assuming") {
    auto fn = load_contract_fn("decreases_complex.c", "countdown_complex");
    auto findings = prism::prove_contracts({fn}, 8);
    REQUIRE(findings.size() == 1);
    require_decreases_unencoded(findings[0]);
    CHECK(findings[0].message.find("n - i") != std::string::npos);

    auto star = prism::prove_contracts({load_contract_fn("decreases_complex.c", "countdown_star")}, 8);
    REQUIRE(star.size() == 1);
    require_decreases_unencoded(star[0]);

    auto absfn = prism::prove_contracts({load_contract_fn("decreases_complex.c", "countdown_abs")}, 8);
    REQUIRE(absfn.size() == 1);
    require_decreases_unencoded(absfn[0]);
}

#ifdef PRISM_HAS_Z3
TEST_CASE("identifier decreases is allowed") {
    auto fn = load_contract_fn("decreases_loop.c", "countdown");
    auto findings = prism::prove_contracts({fn}, 8);
    REQUIRE(findings.size() == 1);
    auto it = findings[0].extra.find("decreases_unencoded");
    CHECK((it == findings[0].extra.end() || it->second.empty() || it->second == "false"));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
}
#endif

#ifdef PRISM_HAS_Z3
TEST_CASE("concolic z3 negated branch finds crash not proof") {
    auto fn = load_fn("klee_fork.c", "klee_fork_neg");
    auto findings = prism::run_concolic({fn}, 32);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].stage == "concolic");
    CHECK(findings[0].status == std::string(prism::laws::CRASH));
    CHECK(findings[0].cls == "INT-DIV-ZERO");
    CHECK_FALSE(findings[0].counterexample.empty());
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_ASSUMING));
    CHECK(findings[0].extra.at("oracle") == "z3");
    CHECK(std::stoi(findings[0].extra.at("z3_seeds")) > 0);
}

TEST_CASE("concolic unsat then-branch is skipped clean not proof") {
    auto fn = load_fn("klee_fork.c", "klee_fork_unsat");
    auto findings = prism::run_concolic({fn}, 32);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::CLEAN));
    CHECK(findings[0].message.find("not a proof") != std::string::npos);
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::CRASH));
    CHECK(std::stoi(findings[0].extra.at("skipped_unsat")) > 0);
    auto got = prism::solve_fork_flip(fn, {{"x", 0}}, "x == 4 && x == 5", true);
    CHECK(got.kind == prism::ForkFlipKind::Unsat);
}

TEST_CASE("concolic z3 model of negated predicate is x=10") {
    auto fn = load_fn("klee_fork.c", "klee_fork_neg");
    auto got = prism::solve_fork_flip(fn, {{"x", 0}}, "x > 8 && x < 12 && x % 17 == 10", true);
    REQUIRE(got.kind == prism::ForkFlipKind::Model);
    CHECK(got.args.at("x") == 10);
}
#endif

TEST_CASE("concolic unsat plant stays clean never a proof") {
    auto fn = load_fn("klee_fork.c", "klee_fork_unsat");
    auto findings = prism::run_concolic({fn}, 32);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::CLEAN));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(findings[0].status != std::string(prism::laws::CRASH));
}

TEST_CASE("wp pointer is NEEDS-HARNESS never unguarded BMC") {
    auto fn = load_fn("wp_ptr.c", "wp_ptr_get");
    auto findings = prism::run_wp({fn}, 4);
    REQUIRE_FALSE(findings.empty());
    for (auto& r : findings) {
        CHECK(r.status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(r.status != std::string(prism::laws::PROVED));
        CHECK(r.status != std::string(prism::laws::CLEAN));
        CHECK(r.stage == "wp");
    }
    CHECK(extra_get(findings[0], "engine") == "prism-wp");
    CHECK(extra_get(findings[0], "wp") == "return-substitution");
}

TEST_CASE("wp unencodable valid is ERROR never proved-assuming") {
    auto fn = load_fn("wp_unenc.c", "wp_valid_bad");
    auto findings = prism::run_wp({fn}, 4);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].status == std::string(prism::laws::ERROR));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_ASSUMING));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(extra_get(findings[0], "wp_unencoded") == "true");
    CHECK(findings[0].message.find("\\valid") != std::string::npos);
    CHECK(extra_get(findings[0], "engine") == "prism-wp");
    CHECK(extra_get(findings[0], "wp") == "return-substitution");
}

TEST_CASE("wp unencodable old and forall are ERROR") {
    auto oldf = prism::run_wp({load_fn("wp_unenc.c", "wp_old_bad")}, 4);
    REQUIRE(oldf.size() == 1);
    CHECK(oldf[0].status == std::string(prism::laws::ERROR));
    CHECK(oldf[0].status != std::string(prism::laws::PROVED_ASSUMING));
    CHECK(extra_get(oldf[0], "wp_unencoded") == "true");
    CHECK(oldf[0].message.find("\\old") != std::string::npos);

    auto fa = prism::run_wp({load_fn("wp_unenc.c", "wp_forall_bad")}, 4);
    REQUIRE(fa.size() == 1);
    CHECK(fa[0].status == std::string(prism::laws::ERROR));
    CHECK(fa[0].status != std::string(prism::laws::PROVED_ASSUMING));
    CHECK(extra_get(fa[0], "wp_unencoded") == "true");
}

static std::vector<prism::Finding> ltl_on(const prism::FunctionInfo& fn, std::string_view formula) {
    auto spec = testdata_root() /
                ("_prism_ltl_" + std::to_string(std::hash<std::string>{}(std::string(formula))) + ".ltl");
    {
        std::ofstream out(spec);
        REQUIRE(out.good());
        out << formula << "\n";
    }
    auto recs = prism::run_ltl({fn}, {spec});
    std::error_code ec;
    std::filesystem::remove(spec, ec);
    return recs;
}

TEST_CASE("ltl GF on fsm_recur is BOUNDED never PROVED") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = prism::run_ltl({fn}, {testdata_root() / "gf_recur.ltl"});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::BOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK(recs[0].extra.at("approx_kind") == "GF");
    CHECK(recs[0].extra.at("strix_not_proved") == "true");
    CHECK(recs[0].extra.contains("safety_approx"));
    CHECK(recs[0].message.find("not a proof") != std::string::npos);
    CHECK_THROWS(prism::laws::refuse_merge(prism::laws::PROVED, recs[0].status));
    CHECK_THROWS(prism::laws::refuse_merge(prism::laws::PROVED, prism::laws::BOUNDED));
}

TEST_CASE("ltl G F unbounded is same GF approx") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "G F (state == LIVE_IDLE)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::BOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].extra.at("approx_kind") == "GF");
}

TEST_CASE("ltl G (F p) unbounded is GF approx") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "G (F (state == LIVE_IDLE))");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::BOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].extra.at("approx_kind") == "GF");
}

TEST_CASE("ltl explicit G (F_k p) is safety fragment PROVED") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "G (F_8 (state == LIVE_IDLE))");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::PROVED));
    CHECK_FALSE(recs[0].extra.contains("approx_kind"));
}

TEST_CASE("ltl GF approx sink is FAILED never PROVED") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "GF (state == ST_IDLE)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::FAILED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::BOUNDED));
    CHECK(recs[0].extra.at("approx_kind") == "GF");
    CHECK(recs[0].extra.at("strix_not_proved") == "true");
}

TEST_CASE("ltl FG approx holds is BOUNDED not PROVED") {
    auto fn = load_fn("fsm_recur.c", "fsm_settle");
    auto recs = ltl_on(fn, "FG (state == LIVE_IDLE)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::BOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].extra.at("approx_kind") == "FG");
}

TEST_CASE("ltl FG approx cycle is FAILED") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "F G (state == LIVE_IDLE)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::FAILED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
}

TEST_CASE("ltl top-level until approx is BOUNDED") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "state == LIVE_ACK U state == LIVE_IDLE");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::BOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].extra.at("approx_kind") == "UNTIL");
}

TEST_CASE("ltl until real violation is FAILED") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "state == ST_WORK U state == ST_IDLE");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::FAILED));
    CHECK(recs[0].message.find("before q") != std::string::npos);
}

TEST_CASE("ltl nested G (p U q) stays NOTRUN") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "G (state == ST_WORK U state == ST_IDLE)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
}

TEST_CASE("ltl bare F stays NOTRUN never CLEAN") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "F (state == ST_IDLE)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK(recs[0].message.find("Strix") != std::string::npos);
    CHECK(recs[0].extra.at("strix_not_proved") == "true");
}

TEST_CASE("ltl nested GF until stays NOTRUN") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "G F (state == ST_IDLE U state == ST_WORK)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
}

TEST_CASE("ltl missing spec is NOTRUN never CLEAN") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = prism::run_ltl({fn}, {});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
}

TEST_CASE("ltl unbounded G (req -> F ack) is FApprox BOUNDED") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "G (state == LIVE_IDLE -> F (state == LIVE_ACK))");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::BOUNDED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].extra.at("approx_kind") == "F");
    CHECK_THROWS(prism::laws::refuse_merge(recs[0].status, prism::laws::PROVED));
}

TEST_CASE("ltl unbounded G (req -> F ack) sink is FAILED") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "G (state == ST_WORK -> F (state == ST_IDLE))");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::FAILED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].extra.at("approx_kind") == "F");
}

TEST_CASE("ltl explicit G (req -> F_k ack) stays BoundedF PROVED") {
    auto fn = load_fn("fsm_recur.c", "fsm_recur");
    auto recs = ltl_on(fn, "G (state == LIVE_IDLE -> F_8 (state == LIVE_ACK))");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::PROVED));
    CHECK_FALSE(recs[0].extra.contains("approx_kind"));
}

TEST_CASE("ltl G implies boolean is invariant") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "G (state == ST_IDLE -> (state == ST_IDLE || state == ST_WORK))");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::PROVED));
}

TEST_CASE("ltl G implies boolean fails") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = ltl_on(fn, "G (state == ST_IDLE -> state == ST_WORK)");
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::FAILED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
}

static std::filesystem::path journal_tmpdir(const char* tag) {
    static int seq = 0;
    auto p = std::filesystem::temp_directory_path() /
             (std::string("prism_journal_") + tag + "_" + std::to_string(++seq));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

static void require_no_journal_proof(const std::vector<prism::StageResult>& stages,
                                     const std::map<std::string, prism::StageResult>& ok) {
    for (auto& s : stages) {
        CHECK(s.status != std::string(prism::laws::CLEAN));
        CHECK(s.status != std::string(prism::laws::PROVED));
        CHECK_FALSE(prism::laws::is_proof(s.status));
    }
    for (auto& [name, s] : ok) {
        (void)name;
        CHECK(s.status != std::string(prism::laws::CLEAN));
        CHECK(s.status != std::string(prism::laws::PROVED));
        CHECK_FALSE(prism::laws::is_proof(s.status));
        CHECK((s.status == "ok" || s.status == "NOTRUN"));
    }
}

TEST_CASE("journal_reset removes stages.jsonl progress.json functions.json") {
    auto out = journal_tmpdir("reset");
    {
        std::ofstream(out / "stages.jsonl") << "{\"name\":\"bmc\",\"status\":\"ok\"}\n";
        std::ofstream(out / "progress.json") << "{\"last\":\"bmc\"}\n";
        std::ofstream(out / "functions.json") << "[]\n";
    }
    REQUIRE(std::filesystem::exists(out / "stages.jsonl"));
    REQUIRE(std::filesystem::exists(out / "progress.json"));
    REQUIRE(std::filesystem::exists(out / "functions.json"));
    prism::journal_reset(out);
    CHECK_FALSE(std::filesystem::exists(out / "stages.jsonl"));
    CHECK_FALSE(std::filesystem::exists(out / "progress.json"));
    CHECK_FALSE(std::filesystem::exists(out / "functions.json"));
    CHECK(prism::journal_read_stages(out).empty());
    CHECK(prism::journal_read_functions(out).empty());
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("journal_append_stage journal_read_stages roundtrip") {
    auto out = journal_tmpdir("append");
    prism::journal_reset(out);
    CHECK(prism::journal_read_stages(out).empty());
    prism::StageResult bmc;
    bmc.name = "bmc";
    bmc.status = "ok";
    bmc.records = 3;
    prism::StageResult fuzz;
    fuzz.name = "fuzz";
    fuzz.status = "ok";
    fuzz.records = 4;
    prism::journal_append_stage(out, bmc);
    prism::journal_append_stage(out, fuzz);
    auto recs = prism::journal_read_stages(out);
    REQUIRE(recs.size() == 2);
    CHECK(recs[0].name == "bmc");
    CHECK(recs[0].status == "ok");
    CHECK(recs[0].records == 3);
    CHECK(recs[1].name == "fuzz");
    CHECK(recs[1].status == "ok");
    CHECK(recs[1].records == 4);
    CHECK(std::filesystem::exists(out / "stages.jsonl"));
    CHECK(std::filesystem::exists(out / "progress.json"));
    prism::journal_reset(out);
    CHECK(prism::journal_read_stages(out).empty());
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("journal_completed_ok last ok NOTRUN wins skipped failed not resumable") {
    auto out = journal_tmpdir("completed");
    prism::StageResult bmc2;
    bmc2.name = "bmc";
    bmc2.status = "ok";
    bmc2.records = 2;
    prism::StageResult llm;
    llm.name = "llm";
    llm.status = "skipped";
    llm.detail = "excluded by --stage/--skip";
    prism::StageResult bmc9;
    bmc9.name = "bmc";
    bmc9.status = "ok";
    bmc9.records = 9;
    prism::StageResult fuzz;
    fuzz.name = "fuzz";
    fuzz.status = "failed";
    fuzz.records = 0;
    prism::StageResult ltl;
    ltl.name = "ltl";
    ltl.status = "NOTRUN";
    ltl.records = 1;
    prism::journal_append_stage(out, bmc2);
    prism::journal_append_stage(out, llm);
    prism::journal_append_stage(out, bmc9);
    prism::journal_append_stage(out, fuzz);
    prism::journal_append_stage(out, ltl);
    auto recs = prism::journal_completed_ok(out);
    CHECK(recs.size() == 2);
    CHECK(recs.count("bmc") == 1);
    CHECK(recs.count("ltl") == 1);
    CHECK(recs["bmc"].records == 9);
    CHECK(recs["ltl"].status == "NOTRUN");
    CHECK(recs.count("llm") == 0);
    CHECK(recs.count("fuzz") == 0);
    require_no_journal_proof(prism::journal_read_stages(out), recs);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("journal_write_functions journal_read_functions roundtrip") {
    auto out = journal_tmpdir("functions");
    prism::FunctionInfo fn;
    fn.file = "a.c";
    fn.name = "add";
    fn.kind = "SCALAR";
    fn.line = 1;
    fn.signature = "int add(int x)";
    fn.params = {{"int", "x"}};
    fn.body = "return x;";
    prism::journal_write_functions(out, {fn});
    auto loaded = prism::journal_read_functions(out);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "add");
    CHECK(loaded[0].body == "return x;");
    REQUIRE(loaded[0].params.size() == 1);
    CHECK(loaded[0].params[0].first == "int");
    CHECK(loaded[0].params[0].second == "x");
    CHECK(loaded[0].file == "a.c");
    prism::journal_reset(out);
    CHECK(prism::journal_read_functions(out).empty());
    CHECK(prism::journal_read_stages(out).empty());
    CHECK_FALSE(std::filesystem::exists(out / "functions.json"));
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("missing journal is never CLEAN or PROVED") {
    auto missing = std::filesystem::temp_directory_path() /
                   "prism_journal_missing_never_clean_proved";
    std::error_code ec;
    std::filesystem::remove_all(missing, ec);
    auto stages = prism::journal_read_stages(missing);
    auto ok = prism::journal_completed_ok(missing);
    auto fns = prism::journal_read_functions(missing);
    CHECK(stages.empty());
    CHECK(ok.empty());
    CHECK(fns.empty());
    require_no_journal_proof(stages, ok);

    auto empty = journal_tmpdir("missing_empty");
    prism::journal_reset(empty);
    stages = prism::journal_read_stages(empty);
    ok = prism::journal_completed_ok(empty);
    fns = prism::journal_read_functions(empty);
    CHECK(stages.empty());
    CHECK(ok.empty());
    CHECK(fns.empty());
    require_no_journal_proof(stages, ok);
    CHECK_FALSE(ok.count("classify"));
    std::filesystem::remove_all(empty, ec);
    std::filesystem::remove_all(missing, ec);
}

#ifdef _WIN32
static void pbsd_env_line(const std::string& line) { _putenv(line.c_str()); }
#endif

// PRISM_PBSD overrides cfg.pbsd_root; tests clear it and restore it after.
struct PbsdEnvGuard {
    std::string was;
    bool had = false;
    std::string clear{"PRISM_PBSD="};
    PbsdEnvGuard() {
        if (const char* v = std::getenv("PRISM_PBSD")) {
            had = true;
            was = v;
        }
#ifdef _WIN32
        pbsd_env_line(clear);
#else
        unsetenv("PRISM_PBSD");
#endif
    }
    ~PbsdEnvGuard() {
#ifdef _WIN32
        // _putenv may keep the pointer; leak the restore line for process lifetime.
        pbsd_env_line(*(new std::string(had ? "PRISM_PBSD=" + was : "PRISM_PBSD=")));
#else
        if (had) setenv("PRISM_PBSD", was.c_str(), 1);
        else unsetenv("PRISM_PBSD");
#endif
    }
};

static void require_pbsd_never_clean_as_proof(const std::vector<prism::Finding>& hits) {
    for (auto& f : hits) {
        CHECK(f.status != std::string(prism::laws::CLEAN));
        CHECK(f.status != std::string(prism::laws::PROVED));
        CHECK(f.status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(f.status != std::string(prism::laws::PROVED_ASSUMING));
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }
}

TEST_CASE("pbsd missing tree is NOTRUN never CLEAN") {
    PbsdEnvGuard env;
    prism::Config cfg = prism::default_config();
    cfg.root = testdata_root();
    cfg.pbsd_root = std::filesystem::current_path().root_path() / "prism-no-such-paranoidbsd";
    auto findings = prism::run_pbsd_lints({testdata_root() / "abs_ok.c"}, cfg);
    REQUIRE_FALSE(findings.empty());
    require_pbsd_never_clean_as_proof(findings);
    for (auto& f : findings) {
        CHECK(f.stage == "pbsd");
        auto via = f.extra.find("via");
        if (via != f.extra.end() && via->second == "prism.portable")
            continue;
        CHECK(f.status == std::string(prism::laws::NOTRUN));
        CHECK(f.status != std::string(prism::laws::CLEAN));
    }
}

TEST_CASE("pbsd onesided plant is FAILED via prism.portable") {
    PbsdEnvGuard env;
    prism::Config cfg = prism::default_config();
    cfg.root = testdata_root();
    cfg.pbsd_root = std::filesystem::current_path().root_path() / "prism-no-such-paranoidbsd";
    auto findings = prism::run_pbsd_lints({testdata_root() / "onesided.c"}, cfg);
    REQUIRE_FALSE(findings.empty());
    require_pbsd_never_clean_as_proof(findings);
    bool hit = false;
    for (auto& f : findings) {
        CHECK(f.status != std::string(prism::laws::PROVED));
        if (f.cls != "MEM-ONESIDED-INDEX") continue;
        hit = true;
        CHECK(f.status == std::string(prism::laws::FAILED));
        CHECK(f.stage == "pbsd");
        CHECK(extra_get(f, "via") == "prism.portable");
    }
    CHECK(hit);
}

TEST_CASE("pbsd empty tools/verify dir is never CLEAN") {
    PbsdEnvGuard env;
    auto td = journal_tmpdir("pbsd_verify");
    std::filesystem::create_directories(td / "tools" / "verify");
    prism::Config cfg = prism::default_config();
    cfg.root = testdata_root();
    cfg.pbsd_root = td;
    auto findings = prism::run_pbsd_lints({testdata_root() / "abs_ok.c"}, cfg);
    require_pbsd_never_clean_as_proof(findings);
    for (auto& f : findings) {
        CHECK(f.stage == "pbsd");
        CHECK(f.status != std::string(prism::laws::CLEAN));
    }
    std::error_code ec;
    std::filesystem::remove_all(td, ec);
}

TEST_CASE("muttest no requires ensures plan is empty never fake CLEAN") {
    auto plain = load_fn("acsl_abs.c", "acsl_plain");
    auto recs = prism::run_muttest({plain}, 8);
    CHECK(recs.empty());
    for (auto& f : recs) {
        CHECK(f.status != std::string(prism::laws::CLEAN));
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }

    auto abs_fn = load_fn("acsl_abs.c", "acsl_abs");
    auto recs_abs = prism::run_muttest({abs_fn}, 8);
    // ACSL /*@ ensures */ is a contract: muttest may sample. CLEAN is not a proof.
    for (auto& f : recs_abs) {
        CHECK(f.status != std::string(prism::laws::PROVED));
        CHECK(f.status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK_FALSE(prism::laws::is_proof(f.status));
        CHECK(f.stage == "muttest");
    }
}

TEST_CASE("muttest killed mutant is CLEAN and is_proof false") {
    auto fn = load_fn("contract_add.c", "inc");
    auto recs = prism::run_muttest({fn}, 32);
    REQUIRE_FALSE(recs.empty());
    const prism::Finding* plus = nullptr;
    for (auto& r : recs) {
        auto it = r.extra.find("from");
        if (it != r.extra.end() && it->second == "+") {
            plus = &r;
            break;
        }
    }
    REQUIRE(plus != nullptr);
    CHECK(plus->stage == "muttest");
    CHECK(plus->status == std::string(prism::laws::CLEAN));
    CHECK(plus->status != std::string(prism::laws::PROVED));
    CHECK(plus->status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(plus->status != std::string(prism::laws::FAILED));
    CHECK_FALSE(prism::laws::is_proof(plus->status));
    auto msg = plus->message;
    for (char& c : msg)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    CHECK(msg.find("mutant killed") != std::string::npos);
    CHECK(msg.find("not a proof") != std::string::npos);
}

TEST_CASE("muttest never emits PROVED or PROVED-UNBOUNDED") {
    for (const char* name : {"inc", "loose_add", "not_inc"}) {
        auto fn = load_fn("contract_add.c", name);
        auto recs = prism::run_muttest({fn}, 16);
        for (auto& r : recs) {
            CHECK(r.stage == "muttest");
            CHECK(r.status != std::string(prism::laws::PROVED));
            CHECK(r.status != std::string(prism::laws::PROVED_UNBOUNDED));
            CHECK(r.status != std::string(prism::laws::PROVED_ASSUMING));
            CHECK_FALSE(prism::laws::is_proof(r.status));
        }
    }
}

TEST_CASE("muttest eval error probes gcc clang NOTRUN never CLEAN") {
    auto fn = load_fn("contract_add.c", "inc");
    fn.body = "return @@@ + 1;";
    auto recs = prism::run_muttest({fn}, 4);
    REQUIRE_FALSE(recs.empty());
    auto& r = recs[0];
    CHECK(r.stage == "muttest");
    CHECK(r.status != std::string(prism::laws::CLEAN));
    CHECK(r.status != std::string(prism::laws::PROVED));
    CHECK(r.status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(r.status != std::string(prism::laws::FAILED));
    CHECK_FALSE(prism::laws::is_proof(r.status));
    auto msg = r.message;
    for (char& c : msg)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    CHECK(msg.find("cannot score") != std::string::npos);
    prism::Config cfg = prism::default_config();
    bool have_cc = cfg.which({"gcc", "clang"}).has_value();
    if (have_cc) {
        CHECK(r.status == std::string(prism::laws::ERROR));
    } else {
        CHECK(r.status == std::string(prism::laws::NOTRUN));
        CHECK(extra_get(r, "install") == "install gcc or clang");
    }
}

TEST_CASE("compiler empty units is UNKNOWN never CLEAN") {
    prism::Config cfg = prism::default_config();
    bool have_cc = cfg.which({"gcc"}).has_value() || cfg.which({"clang"}).has_value();
    if (!have_cc) {
        auto out = prism::run_compiler({}, cfg);
        REQUIRE_FALSE(out.empty());
        CHECK(out[0].stage == "warnings");
        CHECK(out[0].status == std::string(prism::laws::NOTRUN));
        CHECK(out[0].status != std::string(prism::laws::CLEAN));
        CHECK_FALSE(prism::laws::is_proof(out[0].status));
        CHECK(extra_get(out[0], "install") == "install gcc or clang");
        return;
    }
    auto none = prism::run_compiler({}, cfg);
    REQUIRE_FALSE(none.empty());
    CHECK(none[0].stage == "warnings");
    CHECK(none[0].status == std::string(prism::laws::UNKNOWN));
    CHECK(none[0].status != std::string(prism::laws::CLEAN));
    CHECK(none[0].status != std::string(prism::laws::NOTRUN));
    CHECK_FALSE(prism::laws::is_proof(none[0].status));
    auto md = prism::run_compiler({std::filesystem::path("readme.md")}, cfg);
    REQUIRE_FALSE(md.empty());
    CHECK(md[0].status == std::string(prism::laws::UNKNOWN));
    CHECK_FALSE(prism::laws::is_proof(md[0].status));
}

TEST_CASE("compiler abs_ok silence is not CLEAN or a proof") {
    prism::Config cfg = prism::default_config();
    if (!cfg.which({"gcc"}).has_value() && !cfg.which({"clang"}).has_value()) {
        auto out = prism::run_compiler({testdata_root() / "abs_ok.c"}, cfg);
        REQUIRE_FALSE(out.empty());
        CHECK(out[0].status == std::string(prism::laws::NOTRUN));
        CHECK_FALSE(prism::laws::is_proof(out[0].status));
        return;
    }
    auto out = prism::run_compiler({testdata_root() / "abs_ok.c"}, cfg);
    for (auto& f : out) {
        CHECK(f.stage == "warnings");
        CHECK(f.status != std::string(prism::laws::CLEAN));
        CHECK(f.status != std::string(prism::laws::PROVED));
        CHECK(f.status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }
}

TEST_CASE("diff POINTER pair is NEEDS-HARNESS never ERROR") {
    prism::FunctionInfo a;
    a.file = "x.c";
    a.name = "foo_a";
    a.kind = "POINTER";
    a.line = 2;
    a.signature = "int foo_a(int *p)";
    a.params = {{"int *", "p"}};
    a.return_type = "int";
    a.body = "    return *p;";
    prism::FunctionInfo b = a;
    b.name = "foo_b";
    b.signature = "int foo_b(int *p)";
    auto recs = prism::run_diff({a, b}, testdata_root());
    REQUIRE_EQ(recs.size(), 1);
    CHECK(recs[0].stage == "diff");
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(recs[0].status != std::string(prism::laws::ERROR));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("POINTER") != std::string::npos);
}

TEST_CASE("diff OTHER pair is NEEDS-HARNESS never ERROR") {
    prism::FunctionInfo a;
    a.file = "x.c";
    a.name = "foo_a";
    a.kind = "OTHER";
    a.line = 2;
    a.signature = "int foo_a(struct S s)";
    a.params = {{"struct S", "s"}};
    a.return_type = "int";
    a.body = "    return 0;";
    prism::FunctionInfo b = a;
    b.name = "foo_b";
    b.signature = "int foo_b(struct S s)";
    auto recs = prism::run_diff({a, b}, testdata_root());
    REQUIRE_EQ(recs.size(), 1);
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(recs[0].status != std::string(prism::laws::ERROR));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

TEST_CASE("cpu havoc INTERESTING overlays mutate a buffer") {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t orig[8];
    std::memcpy(orig, buf, 8);
    prism::havoc(buf, 8, 0xC0FFEEULL);
    bool changed = false;
    for (int i = 0; i < 8; ++i) if (buf[i] != orig[i]) changed = true;
    CHECK(changed);
}

#ifdef PRISM_HAS_CUDA
TEST_CASE("cuda havoc returns 0 or honest device failure never a proof") {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int rc = prism_cuda_havoc(buf, 8, 1, 1);
    CHECK(rc <= 0);
    CHECK(rc != 1);
}
#endif

TEST_CASE("empty-scope confidence is 0 not n/a and note once") {
    prism::RunReport r;
    prism::apply_confidence(r);
    CHECK(r.confidence == 0.0);
    CHECK(r.visibility == 0.0);
    CHECK(r.answer == 0.0);
    CHECK(r.resolution == 0.0);
    REQUIRE_EQ(r.notes.size(), 1);
    CHECK(r.notes[0].find("no functions parsed") != std::string::npos);
    prism::apply_confidence(r);
    CHECK(r.notes.size() == 1);
}

TEST_CASE("llm_forced_reads demotes proofs to HYPOTHESIS") {
    prism::Finding proved;
    proved.stage = "bmc";
    proved.status = std::string(prism::laws::PROVED);
    proved.cls = "INT-DIV-ZERO";
    proved.strength = std::string(prism::laws::STRENGTH_PROVES);
    prism::Finding keep;
    keep.stage = "llm";
    keep.status = std::string(prism::laws::NOTRUN);
    keep.strength = std::string(prism::laws::STRENGTH_FINDS);
    auto out = prism::llm_forced_reads({proved, keep});
    REQUIRE_EQ(out.size(), 2);
    CHECK(out[0].stage == "llm");
    CHECK(out[0].status == std::string(prism::laws::HYPOTHESIS));
    CHECK(out[0].status != std::string(prism::laws::PROVED));
    CHECK(out[0].strength == std::string(prism::laws::STRENGTH_READS));
    CHECK_FALSE(prism::laws::is_proof(out[0].status));
    CHECK(out[1].status == std::string(prism::laws::NOTRUN));
    CHECK(out[1].strength == std::string(prism::laws::STRENGTH_READS));
}

TEST_CASE("write_report_md keeps UNKNOWN and TIMEOUT") {
    auto out = journal_tmpdir("report_md");
    prism::RunReport r;
    r.root = "x";
    prism::StageResult s;
    s.name = "warnings";
    s.status = "ok";
    prism::Finding u;
    u.stage = "warnings";
    u.status = std::string(prism::laws::UNKNOWN);
    u.message = "no C files";
    prism::Finding t;
    t.stage = "warnings";
    t.status = std::string(prism::laws::TIMEOUT);
    t.message = "compiler timeout";
    s.findings = {u, t};
    r.stages = {s};
    auto md = out / "report.md";
    prism::write_report_md(r, md);
    std::ifstream in(md);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("UNKNOWN") != std::string::npos);
    CHECK(body.find("TIMEOUT") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("taxonomy llm-only capable instrument is READS never COVERED") {
    prism::RunReport r;
    prism::StageResult llm;
    llm.name = "llm";
    llm.status = "ok";
    r.stages = {llm};
    auto rows = prism::coverage_from_report(r);
    bool saw_intent = false;
    for (auto& row : rows) {
        if (row.id != "INTENT") continue;
        saw_intent = true;
        CHECK(row.best == std::string(prism::laws::STRENGTH_READS));
        CHECK(row.verdict == "PARTIAL");
        CHECK(row.verdict != "COVERED");
    }
    CHECK(saw_intent);
}

TEST_CASE("taxonomy READS never overwrites FINDS COVERED") {
    prism::RunReport r;
    prism::StageResult lints;
    lints.name = "lints";
    lints.status = "ok";
    prism::Finding finds;
    finds.stage = "lints";
    finds.status = std::string(prism::laws::FAILED);
    finds.cls = "INT-SIGNED-OVF";
    finds.strength = std::string(prism::laws::STRENGTH_FINDS);
    lints.findings = {finds};
    prism::StageResult llm;
    llm.name = "llm";
    llm.status = "ok";
    prism::Finding hyp;
    hyp.stage = "llm";
    hyp.status = std::string(prism::laws::HYPOTHESIS);
    hyp.cls = "INT-SIGNED-OVF";
    hyp.strength = std::string(prism::laws::STRENGTH_READS);
    llm.findings = {hyp};
    r.stages = {lints, llm};
    auto rows = prism::coverage_from_report(r);
    bool saw = false;
    for (auto& row : rows) {
        if (row.id != "INT-SIGNED-OVF") continue;
        saw = true;
        CHECK(row.best == std::string(prism::laws::STRENGTH_FINDS));
        CHECK(row.verdict == "COVERED");
        CHECK(row.verdict != "PARTIAL");
        CHECK(row.best != std::string(prism::laws::STRENGTH_READS));
    }
    CHECK(saw);
}

TEST_CASE("refuse_llm_cover demotes READS and only-LLM COVERED") {
    prism::RunReport empty;
    CHECK(prism::refuse_llm_cover(empty, "INT-SIGNED-OVF", "COVERED",
                                  std::string(prism::laws::STRENGTH_READS)) == "PARTIAL");
    CHECK(prism::refuse_llm_cover(empty, "INT-SIGNED-OVF", "GAP",
                                  std::string(prism::laws::STRENGTH_READS)) == "GAP");
    prism::RunReport llm_only;
    prism::StageResult llm;
    llm.name = "llm";
    llm.status = "ok";
    prism::Finding hyp;
    hyp.stage = "llm";
    hyp.status = std::string(prism::laws::HYPOTHESIS);
    hyp.cls = "INT-SIGNED-OVF";
    hyp.message = "maybe";
    llm.findings = {hyp};
    llm_only.stages = {llm};
    CHECK(prism::refuse_llm_cover(llm_only, "INT-SIGNED-OVF", "COVERED",
                                  std::string(prism::laws::STRENGTH_PROVES)) == "PARTIAL");
    prism::RunReport mixed;
    prism::StageResult lints;
    lints.name = "lints";
    lints.status = "ok";
    prism::Finding finds;
    finds.stage = "lints";
    finds.status = std::string(prism::laws::FAILED);
    finds.cls = "INT-SIGNED-OVF";
    finds.strength = std::string(prism::laws::STRENGTH_FINDS);
    lints.findings = {finds};
    mixed.stages = {lints, llm};
    CHECK(prism::refuse_llm_cover(mixed, "INT-SIGNED-OVF", "COVERED",
                                  std::string(prism::laws::STRENGTH_PROVES)) == "COVERED");
}

TEST_CASE("unify CLEAN carries not_a_proof") {
    prism::Finding f;
    f.stage = "unify";
    f.status = std::string(prism::laws::CLEAN);
    f.extra["not_a_proof"] = "true";
    CHECK(f.status == std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.extra["not_a_proof"] == "true");
}

TEST_CASE("failed jsonl does not resume stale report.json ok rows") {
    auto out = journal_tmpdir("resume_stale");
    prism::StageResult failed;
    failed.name = "inventory";
    failed.status = "failed";
    prism::journal_append_stage(out, failed);
    REQUIRE(prism::journal_stages_present(out));
    CHECK(prism::journal_completed_ok(out).empty());

    prism::RunReport stale;
    stale.root = "stale";
    prism::StageResult fake_bmc;
    fake_bmc.name = "bmc";
    fake_bmc.status = "ok";
    prism::Finding lie;
    lie.stage = "bmc";
    lie.status = std::string(prism::laws::CLEAN);
    lie.function = "abs_ok";
    lie.message = "stale report.json must not resume";
    fake_bmc.findings = {lie};
    fake_bmc.records = 1;
    stale.stages = {fake_bmc};
    stale.save(out / "report.json");

    prism::Config cfg = prism::default_config();
    cfg.root = testdata_root() / "abs_ok.c";
    cfg.out = out;
    cfg.resume = true;
    cfg.llm = false;
    cfg.skip = {"llm", "execute", "repair", "optional", "fuzz", "sanitize", "pbsd",
                "esbmc", "dafny", "ltl", "rapid", "muttest", "diff", "concolic",
                "harness", "wp", "contracts", "interval", "taint", "thread",
                "lints", "warnings", "cppcheck"};
    auto report = prism::run_pipeline(cfg);
    bool resumed_report_json = false;
    for (auto& n : report.notes)
        if (n.find("report.json") != std::string::npos) resumed_report_json = true;
    CHECK_FALSE(resumed_report_json);
    bool bmc_stale = false;
    bool bmc_proved = false;
    for (auto& s : report.stages) {
        if (s.name != "bmc") continue;
        for (auto& f : s.findings) {
            if (f.message.find("stale report.json") != std::string::npos) bmc_stale = true;
            if (f.status == std::string(prism::laws::PROVED_UNBOUNDED)) bmc_proved = true;
        }
    }
    CHECK_FALSE(bmc_stale);
    CHECK(bmc_proved);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("contracts POINTER is NEEDS-HARNESS never unguarded BMC") {
    auto fn = load_fn("wp_ptr.c", "wp_ptr_get");
    auto recs = prism::prove_contracts({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("POINTER") != std::string::npos);
}

TEST_CASE("rapid ACSL-only POINTER is NEEDS-HARNESS") {
    auto fn = load_fn("wp_ptr.c", "wp_acsl_ptr");
    auto recs = prism::run_rapid({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

TEST_CASE("wp pointer member arrow is ERROR not proved-assuming") {
    auto fn = load_fn("wp_ptr.c", "wp_ptr_get");
    auto recs = prism::run_wp({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

TEST_CASE("execute_cex POINTER is NEEDS-HARNESS") {
    auto fn = load_fn("wp_ptr.c", "wp_ptr_get");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = fn.file;
    fail.function = fn.name;
    fail.line = fn.line;
    fail.counterexample = "p=0";
    prism::Config cfg = prism::default_config();
    cfg.llm = false;
    auto recs = prism::execute_cex({fail}, {fn}, cfg);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

#ifdef PRISM_HAS_Z3
TEST_CASE("bmc leftover10 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"kld_load_unenc.c", "kld_load_unenc_bad", "kld_load"},
        {"ksem_close_unenc.c", "ksem_close_unenc_bad", "ksem"},
        {"print_unenc.c", "print_unenc_bad", "format"},
        {"future_unenc.c", "future_unenc_bad", "async"},
        {"ulock_unenc.c", "ulock_unenc_bad", "mutex"},
        {"destroy_at_unenc.c", "destroy_at_unenc_bad", "construct_at"},
        {"subsat_unenc.c", "subsat_unenc_bad", "add_sat"},
        {"getterm_unenc.c", "getterm_unenc_bad", "set_terminate"},
        {"fmtn_unenc.c", "fmtn_unenc_bad", "format_to"},
        {"bitfloor_unenc.c", "bitfloor_unenc_bad", "bit_ceil"},
        {"stopsrc_unenc.c", "stopsrc_unenc_bad", "stop_token"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover10 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"kld_load_unenc.c", "kld_load_unenc_ok"},
        {"ksem_close_unenc.c", "ksem_close_unenc_ok"},
        {"print_unenc.c", "print_unenc_ok"},
        {"future_unenc.c", "future_unenc_ok"},
        {"ulock_unenc.c", "ulock_unenc_ok"},
        {"destroy_at_unenc.c", "destroy_at_unenc_ok"},
        {"subsat_unenc.c", "subsat_unenc_ok"},
        {"getterm_unenc.c", "getterm_unenc_ok"},
        {"fmtn_unenc.c", "fmtn_unenc_ok"},
        {"bitfloor_unenc.c", "bitfloor_unenc_ok"},
        {"stopsrc_unenc.c", "stopsrc_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover10 goto stays ERROR and abs_ok stays PROVED-UNBOUNDED") {
    auto gt = load_fn("goto_unenc.c", "with_goto");
    auto gf = prism::run_bmc({gt}, 8);
    REQUIRE_FALSE(gf.empty());
    CHECK(gf[0].status == std::string(prism::laws::ERROR));
    CHECK(gf[0].status != std::string(prism::laws::NEEDS_HARNESS));

    auto fn = load_fn("abs_ok.c", "abs_ok");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
    CHECK(findings[0].status != std::string(prism::laws::ERROR));
}

TEST_CASE("bmc leftover11 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"mulsat_unenc.c", "mulsat_unenc_bad", "add_sat"},
        {"divsat_unenc.c", "divsat_unenc_bad", "add_sat"},
        {"satcast_unenc.c", "satcast_unenc_bad", "add_sat"},
        {"lguard_unenc.cpp", "lguard_unenc_bad", "mutex"},
        {"slock_unenc.cpp", "slock_unenc_bad", "shared_lock"},
        {"hasbit_unenc.c", "hasbit_unenc_bad", "bit_ceil"},
        {"stopcb_unenc.cpp", "stopcb_unenc_bad", "stop_token"},
        {"popcnt_unenc.cpp", "popcnt_unenc_bad", "bit_ceil"},
        {"scoped_unenc.c", "scoped_unenc_bad", "mutex"},
        {"promise_unenc.c", "promise_unenc_bad", "async"},
        {"smutex_unenc.c", "smutex_unenc_bad", "condition_variable"},
        {"czone_unenc.c", "czone_unenc_bad", "tzdb"},
        {"chunkby_unenc.c", "chunkby_unenc_bad", "chunk"},
        {"adjt_unenc.c", "adjt_unenc_bad", "adjacent"},
        {"texscan_unenc.c", "texscan_unenc_bad", "transform_inclusive_scan"},
        {"rotr_unenc.c", "rotr_unenc_bad", "rotl"},
        {"cmpg_unenc.c", "cmpg_unenc_bad", "cmp_less"},
        {"cntrz_unenc.c", "cntrz_unenc_bad", "countl_zero"},
        {"uimove_unenc.c", "uimove_unenc_bad", "uninitialized_copy"},
        {"uifilln_unenc.c", "uifilln_unenc_bad", "uninitialized_fill"},
        {"pinterb_unenc.c", "pinterb_unenc_bad", "is_pointer_interconvertible"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover11 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"mulsat_unenc.c", "mulsat_unenc_ok"},
        {"divsat_unenc.c", "divsat_unenc_ok"},
        {"satcast_unenc.c", "satcast_unenc_ok"},
        {"lguard_unenc.cpp", "lguard_unenc_ok"},
        {"slock_unenc.cpp", "slock_unenc_ok"},
        {"hasbit_unenc.c", "hasbit_unenc_ok"},
        {"stopcb_unenc.cpp", "stopcb_unenc_ok"},
        {"popcnt_unenc.cpp", "popcnt_unenc_ok"},
        {"scoped_unenc.c", "scoped_unenc_ok"},
        {"promise_unenc.c", "promise_unenc_ok"},
        {"smutex_unenc.c", "smutex_unenc_ok"},
        {"czone_unenc.c", "czone_unenc_ok"},
        {"chunkby_unenc.c", "chunkby_unenc_ok"},
        {"adjt_unenc.c", "adjt_unenc_ok"},
        {"texscan_unenc.c", "texscan_unenc_ok"},
        {"rotr_unenc.c", "rotr_unenc_ok"},
        {"cmpg_unenc.c", "cmpg_unenc_ok"},
        {"cntrz_unenc.c", "cntrz_unenc_ok"},
        {"uimove_unenc.c", "uimove_unenc_ok"},
        {"uifilln_unenc.c", "uifilln_unenc_ok"},
        {"pinterb_unenc.c", "pinterb_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover12 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"madvise_unenc.c", "madvise_unenc_bad", "madvise"},
        {"posix_madvise_unenc.c", "posix_madvise_unenc_bad", "madvise"},
        {"setusercontext_unenc.c", "setusercontext_unenc_bad", "login_getclass"},
        {"strtofflags_unenc.c", "strtofflags_unenc_bad", "fflagstostr"},
        {"kinfo_getfile_unenc.c", "kinfo_getfile_unenc_bad", "kinfo_getproc"},
        {"kinfo_getvmmap_unenc.c", "kinfo_getvmmap_unenc_bad", "kinfo_getproc"},
        {"chunkv_unenc.c", "chunkv_unenc_bad", "chunk"},
        {"adjvw_unenc.c", "adjvw_unenc_bad", "adjacent"},
        {"uidc_unenc.c", "uidc_unenc_bad", "uninitialized_fill"},
        {"uidcn_unenc.c", "uidcn_unenc_bad", "uninitialized_fill"},
        {"uicn_unenc.c", "uicn_unenc_bad", "uninitialized_copy"},
        {"uimn_unenc.c", "uimn_unenc_bad", "uninitialized_copy"},
        {"cmple_unenc.c", "cmple_unenc_bad", "cmp_less"},
        {"cmpge_unenc.c", "cmpge_unenc_bad", "cmp_less"},
        {"cmpeq_unenc.c", "cmpeq_unenc_bad", "cmp_less"},
        {"cmpne_unenc.c", "cmpne_unenc_bad", "cmp_less"},
        {"inrng_unenc.c", "inrng_unenc_bad", "cmp_less"},
        {"cntlo_unenc.c", "cntlo_unenc_bad", "countl_zero"},
        {"cntro_unenc.c", "cntro_unenc_bad", "countl_zero"},
        {"ifs_unenc.cpp", "ifs_unenc_bad", "fstream"},
        {"ofs_unenc.cpp", "ofs_unenc_bad", "fstream"},
        {"u8view_unenc.cpp", "u8view_unenc_bad", "u8string"},
        {"mofn_unenc.cpp", "mofn_unenc_bad", "move_only_function"},
        {"uvaln_unenc.c", "uvaln_unenc_bad", "uninitialized_value_construct"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover12 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"madvise_unenc.c", "madvise_unenc_ok"},
        {"posix_madvise_unenc.c", "posix_madvise_unenc_ok"},
        {"setusercontext_unenc.c", "setusercontext_unenc_ok"},
        {"strtofflags_unenc.c", "strtofflags_unenc_ok"},
        {"kinfo_getfile_unenc.c", "kinfo_getfile_unenc_ok"},
        {"kinfo_getvmmap_unenc.c", "kinfo_getvmmap_unenc_ok"},
        {"chunkv_unenc.c", "chunkv_unenc_ok"},
        {"adjvw_unenc.c", "adjvw_unenc_ok"},
        {"uidc_unenc.c", "uidc_unenc_ok"},
        {"uidcn_unenc.c", "uidcn_unenc_ok"},
        {"uicn_unenc.c", "uicn_unenc_ok"},
        {"uimn_unenc.c", "uimn_unenc_ok"},
        {"cmple_unenc.c", "cmple_unenc_ok"},
        {"cmpge_unenc.c", "cmpge_unenc_ok"},
        {"cmpeq_unenc.c", "cmpeq_unenc_ok"},
        {"cmpne_unenc.c", "cmpne_unenc_ok"},
        {"inrng_unenc.c", "inrng_unenc_ok"},
        {"cntlo_unenc.c", "cntlo_unenc_ok"},
        {"cntro_unenc.c", "cntro_unenc_ok"},
        {"ifs_unenc.cpp", "ifs_unenc_ok"},
        {"ofs_unenc.cpp", "ofs_unenc_ok"},
        {"u8view_unenc.cpp", "u8view_unenc_ok"},
        {"mofn_unenc.cpp", "mofn_unenc_ok"},
        {"uvaln_unenc.c", "uvaln_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover13 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"setloginclass_unenc.c", "setloginclass_unenc_bad", "loginclass"},
        {"setfsent_unenc.c", "setfsent_unenc_bad", "getfsent"},
        {"endfsent_unenc.c", "endfsent_unenc_bad", "getfsent"},
        {"modstat_unenc.c", "modstat_unenc_bad", "modfind"},
        {"modnext_unenc.c", "modnext_unenc_bad", "modfind"},
        {"modfnext_unenc.c", "modfnext_unenc_bad", "modfind"},
        {"slidev_unenc.c", "slidev_unenc_bad", "slide"},
        {"jwithv_unenc.c", "jwithv_unenc_bad", "join_with"},
        {"joinv_unenc.c", "joinv_unenc_bad", "join"},
        {"ztransv_unenc.c", "ztransv_unenc_bad", "zip_transform"},
        {"zipv_unenc.c", "zipv_unenc_bad", "zip"},
        {"asrvalv_unenc.c", "asrvalv_unenc_bad", "as_rvalue"},
        {"enumvw_unenc.c", "enumvw_unenc_bad", "enumerate"},
        {"cartv_unenc.c", "cartv_unenc_bad", "cartesian_product"},
        {"stridev_unenc.c", "stridev_unenc_bad", "stride"},
        {"repeatv_unenc.c", "repeatv_unenc_bad", "repeat"},
        {"twhilev_unenc.c", "twhilev_unenc_bad", "take_while"},
        {"takevw_unenc.c", "takevw_unenc_bad", "take"},
        {"dwhilev_unenc.c", "dwhilev_unenc_bad", "drop_while"},
        {"dropvw_unenc.c", "dropvw_unenc_bad", "drop"},
        {"keysv_unenc.c", "keysv_unenc_bad", "keys"},
        {"valsv_unenc.c", "valsv_unenc_bad", "values"},
        {"revv_unenc.c", "revv_unenc_bad", "reverse"},
        {"countvw_unenc.c", "countvw_unenc_bad", "counted"},
        {"filtervw_unenc.c", "filtervw_unenc_bad", "filter"},
        {"tvw_unenc.c", "tvw_unenc_bad", "views::transform"},
        {"elemsv_unenc.c", "elemsv_unenc_bad", "elements"},
        {"iotav_unenc.c", "iotav_unenc_bad", "iota"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover13 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"setloginclass_unenc.c", "setloginclass_unenc_ok"},
        {"setfsent_unenc.c", "setfsent_unenc_ok"},
        {"endfsent_unenc.c", "endfsent_unenc_ok"},
        {"modstat_unenc.c", "modstat_unenc_ok"},
        {"modnext_unenc.c", "modnext_unenc_ok"},
        {"modfnext_unenc.c", "modfnext_unenc_ok"},
        {"slidev_unenc.c", "slidev_unenc_ok"},
        {"jwithv_unenc.c", "jwithv_unenc_ok"},
        {"joinv_unenc.c", "joinv_unenc_ok"},
        {"ztransv_unenc.c", "ztransv_unenc_ok"},
        {"zipv_unenc.c", "zipv_unenc_ok"},
        {"asrvalv_unenc.c", "asrvalv_unenc_ok"},
        {"enumvw_unenc.c", "enumvw_unenc_ok"},
        {"cartv_unenc.c", "cartv_unenc_ok"},
        {"stridev_unenc.c", "stridev_unenc_ok"},
        {"repeatv_unenc.c", "repeatv_unenc_ok"},
        {"twhilev_unenc.c", "twhilev_unenc_ok"},
        {"takevw_unenc.c", "takevw_unenc_ok"},
        {"dwhilev_unenc.c", "dwhilev_unenc_ok"},
        {"dropvw_unenc.c", "dropvw_unenc_ok"},
        {"keysv_unenc.c", "keysv_unenc_ok"},
        {"valsv_unenc.c", "valsv_unenc_ok"},
        {"revv_unenc.c", "revv_unenc_ok"},
        {"countvw_unenc.c", "countvw_unenc_ok"},
        {"filtervw_unenc.c", "filtervw_unenc_ok"},
        {"tvw_unenc.c", "tvw_unenc_ok"},
        {"elemsv_unenc.c", "elemsv_unenc_ok"},
        {"iotav_unenc.c", "iotav_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover14 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"thr_new_unenc.c", "thr_new_unenc_bad", "thr_kill"},
        {"thr_kill2_unenc.c", "thr_kill2_unenc_bad", "thr_kill"},
        {"thr_self_unenc.c", "thr_self_unenc_bad", "thr_kill"},
        {"thr_exit_unenc.c", "thr_exit_unenc_bad", "thr_kill"},
        {"thr_suspend_unenc.c", "thr_suspend_unenc_bad", "thr_kill"},
        {"thr_wake_unenc.c", "thr_wake_unenc_bad", "thr_kill"},
        {"kldunload_unenc.c", "kldunload_unenc_bad", "kldload"},
        {"kldfind_unenc.c", "kldfind_unenc_bad", "kldload"},
        {"kldsym_unenc.c", "kldsym_unenc_bad", "kldload"},
        {"kldstat_unenc.c", "kldstat_unenc_bad", "kldload"},
        {"extattr_get_file_unenc.c", "extattr_get_file_unenc_bad", "extattr"},
        {"extattr_delete_file_unenc.c", "extattr_delete_file_unenc_bad", "extattr"},
        {"extattr_list_file_unenc.c", "extattr_list_file_unenc_bad", "extattr"},
        {"extattr_set_fd_unenc.c", "extattr_set_fd_unenc_bad", "extattr"},
        {"extattr_get_fd_unenc.c", "extattr_get_fd_unenc_bad", "extattr"},
        {"extattr_delete_fd_unenc.c", "extattr_delete_fd_unenc_bad", "extattr"},
        {"extattr_list_fd_unenc.c", "extattr_list_fd_unenc_bad", "extattr"},
        {"extattr_set_link_unenc.c", "extattr_set_link_unenc_bad", "extattr"},
        {"extattr_get_link_unenc.c", "extattr_get_link_unenc_bad", "extattr"},
        {"extattr_delete_link_unenc.c", "extattr_delete_link_unenc_bad", "extattr"},
        {"extattr_list_link_unenc.c", "extattr_list_link_unenc_bad", "extattr"},
        {"mac_get_proc_unenc.c", "mac_get_proc_unenc_bad", "mac_set"},
        {"mac_set_fd_unenc.c", "mac_set_fd_unenc_bad", "mac_set"},
        {"mac_get_fd_unenc.c", "mac_get_fd_unenc_bad", "mac_set"},
        {"mac_set_file_unenc.c", "mac_set_file_unenc_bad", "mac_set"},
        {"mac_get_file_unenc.c", "mac_get_file_unenc_bad", "mac_set"},
        {"getaudit_unenc.c", "getaudit_unenc_bad", "auditon"},
        {"setaudit_unenc.c", "setaudit_unenc_bad", "auditon"},
        {"auditctl_unenc.c", "auditctl_unenc_bad", "auditon"},
        {"kvm_openfiles_unenc.c", "kvm_openfiles_unenc_bad", "kvm_open"},
        {"kvm_getprocs_unenc.c", "kvm_getprocs_unenc_bad", "kvm_open"},
        {"kvm_close_unenc.c", "kvm_close_unenc_bad", "kvm_open"},
        {"kvm_nlist_unenc.c", "kvm_nlist_unenc_bad", "kvm_open"},
        {"cap_ioctls_limit_unenc.c", "cap_ioctls_limit_unenc_bad", "cap_fcntls"},
        {"pdwait4_unenc.c", "pdwait4_unenc_bad", "pdgetpid"},
        {"crypt_checkpass_unenc.c", "crypt_checkpass_unenc_bad", "crypt_newhash"},
        {"jail_attach_unenc.c", "jail_attach_unenc_bad", "jail"},
        {"jail_get_unenc.c", "jail_get_unenc_bad", "jail"},
        {"jail_set_unenc.c", "jail_set_unenc_bad", "jail"},
        {"jail_remove_unenc.c", "jail_remove_unenc_bad", "jail"},
        {"getresgid_unenc.c", "getresgid_unenc_bad", "getresuid"},
        {"timingsafe_memcmp_unenc.c", "timingsafe_memcmp_unenc_bad", "timingsafe"},
        {"setprogname_unenc.c", "setprogname_unenc_bad", "getprogname"},
        {"setproctitle_unenc.c", "setproctitle_unenc_bad", "daemon"},
        {"arc4random_buf_unenc.c", "arc4random_buf_unenc_bad", "arc4random"},
        {"arc4random_uniform_unenc.c", "arc4random_uniform_unenc_bad", "arc4random"},
        {"fchflags_unenc.c", "fchflags_unenc_bad", "chflags"},
        {"lchflags_unenc.c", "lchflags_unenc_bad", "chflags"},
        {"ntp_adjtime_unenc.c", "ntp_adjtime_unenc_bad", "adjtime"},
        {"sem_getvalue_unenc.c", "sem_getvalue_unenc_bad", "sem_trywait"},
        {"rtprio_thread_unenc.c", "rtprio_thread_unenc_bad", "rtprio"},
        {"cpuset_getaffinity_unenc.c", "cpuset_getaffinity_unenc_bad", "cpuset"},
        {"fhopen_unenc.c", "fhopen_unenc_bad", "getfh"},
        {"fhstat_unenc.c", "fhstat_unenc_bad", "getfh"},
        {"fhstatfs_unenc.c", "fhstatfs_unenc_bad", "getfh"},
        {"getfhat_unenc.c", "getfhat_unenc_bad", "getfh"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover14 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"thr_new_unenc.c", "thr_new_unenc_ok"},
        {"thr_kill2_unenc.c", "thr_kill2_unenc_ok"},
        {"thr_self_unenc.c", "thr_self_unenc_ok"},
        {"thr_exit_unenc.c", "thr_exit_unenc_ok"},
        {"thr_suspend_unenc.c", "thr_suspend_unenc_ok"},
        {"thr_wake_unenc.c", "thr_wake_unenc_ok"},
        {"kldunload_unenc.c", "kldunload_unenc_ok"},
        {"kldfind_unenc.c", "kldfind_unenc_ok"},
        {"kldsym_unenc.c", "kldsym_unenc_ok"},
        {"kldstat_unenc.c", "kldstat_unenc_ok"},
        {"extattr_get_file_unenc.c", "extattr_get_file_unenc_ok"},
        {"extattr_delete_file_unenc.c", "extattr_delete_file_unenc_ok"},
        {"extattr_list_file_unenc.c", "extattr_list_file_unenc_ok"},
        {"extattr_set_fd_unenc.c", "extattr_set_fd_unenc_ok"},
        {"extattr_get_fd_unenc.c", "extattr_get_fd_unenc_ok"},
        {"extattr_delete_fd_unenc.c", "extattr_delete_fd_unenc_ok"},
        {"extattr_list_fd_unenc.c", "extattr_list_fd_unenc_ok"},
        {"extattr_set_link_unenc.c", "extattr_set_link_unenc_ok"},
        {"extattr_get_link_unenc.c", "extattr_get_link_unenc_ok"},
        {"extattr_delete_link_unenc.c", "extattr_delete_link_unenc_ok"},
        {"extattr_list_link_unenc.c", "extattr_list_link_unenc_ok"},
        {"mac_get_proc_unenc.c", "mac_get_proc_unenc_ok"},
        {"mac_set_fd_unenc.c", "mac_set_fd_unenc_ok"},
        {"mac_get_fd_unenc.c", "mac_get_fd_unenc_ok"},
        {"mac_set_file_unenc.c", "mac_set_file_unenc_ok"},
        {"mac_get_file_unenc.c", "mac_get_file_unenc_ok"},
        {"getaudit_unenc.c", "getaudit_unenc_ok"},
        {"setaudit_unenc.c", "setaudit_unenc_ok"},
        {"auditctl_unenc.c", "auditctl_unenc_ok"},
        {"kvm_openfiles_unenc.c", "kvm_openfiles_unenc_ok"},
        {"kvm_getprocs_unenc.c", "kvm_getprocs_unenc_ok"},
        {"kvm_close_unenc.c", "kvm_close_unenc_ok"},
        {"kvm_nlist_unenc.c", "kvm_nlist_unenc_ok"},
        {"cap_ioctls_limit_unenc.c", "cap_ioctls_limit_unenc_ok"},
        {"pdwait4_unenc.c", "pdwait4_unenc_ok"},
        {"crypt_checkpass_unenc.c", "crypt_checkpass_unenc_ok"},
        {"jail_attach_unenc.c", "jail_attach_unenc_ok"},
        {"jail_get_unenc.c", "jail_get_unenc_ok"},
        {"jail_set_unenc.c", "jail_set_unenc_ok"},
        {"jail_remove_unenc.c", "jail_remove_unenc_ok"},
        {"getresgid_unenc.c", "getresgid_unenc_ok"},
        {"timingsafe_memcmp_unenc.c", "timingsafe_memcmp_unenc_ok"},
        {"setprogname_unenc.c", "setprogname_unenc_ok"},
        {"setproctitle_unenc.c", "setproctitle_unenc_ok"},
        {"arc4random_buf_unenc.c", "arc4random_buf_unenc_ok"},
        {"arc4random_uniform_unenc.c", "arc4random_uniform_unenc_ok"},
        {"fchflags_unenc.c", "fchflags_unenc_ok"},
        {"lchflags_unenc.c", "lchflags_unenc_ok"},
        {"ntp_adjtime_unenc.c", "ntp_adjtime_unenc_ok"},
        {"sem_getvalue_unenc.c", "sem_getvalue_unenc_ok"},
        {"rtprio_thread_unenc.c", "rtprio_thread_unenc_ok"},
        {"cpuset_getaffinity_unenc.c", "cpuset_getaffinity_unenc_ok"},
        {"fhopen_unenc.c", "fhopen_unenc_ok"},
        {"fhstat_unenc.c", "fhstat_unenc_ok"},
        {"fhstatfs_unenc.c", "fhstatfs_unenc_ok"},
        {"getfhat_unenc.c", "getfhat_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover15 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"cap_rights_get_unenc.c", "cap_rights_get_unenc_bad", "cap_rights"},
        {"pthread_attr_destroy_unenc.c", "pthread_attr_destroy_unenc_bad", "pthread_attr"},
        {"pthread_attr_setstacksize_unenc.c", "pthread_attr_setstacksize_unenc_bad", "pthread_attr"},
        {"pthread_attr_setstack_unenc.c", "pthread_attr_setstack_unenc_bad", "pthread_attr"},
        {"pthread_attr_setdetachstate_unenc.c", "pthread_attr_setdetachstate_unenc_bad", "pthread_attr"},
        {"pthread_attr_getstacksize_unenc.c", "pthread_attr_getstacksize_unenc_bad", "pthread_attr"},
        {"pthread_attr_getstack_unenc.c", "pthread_attr_getstack_unenc_bad", "pthread_attr"},
        {"pthread_attr_getdetachstate_unenc.c", "pthread_attr_getdetachstate_unenc_bad", "pthread_attr"},
        {"setcontext_unenc.c", "setcontext_unenc_bad", "getcontext"},
        {"swapcontext_unenc.c", "swapcontext_unenc_bad", "getcontext"},
        {"makecontext_unenc.c", "makecontext_unenc_bad", "getcontext"},
        {"wait3_unenc.c", "wait3_unenc_bad", "wait4"},
        {"setregid_unenc.c", "setregid_unenc_bad", "setreuid"},
        {"setresuid_unenc.c", "setresuid_unenc_bad", "setreuid"},
        {"setresgid_unenc.c", "setresgid_unenc_bad", "setreuid"},
        {"raise_unenc.c", "raise_unenc_bad", "kill"},
        {"alarm_unenc.c", "alarm_unenc_bad", "kill"},
        {"pthread_detach_unenc.c", "pthread_detach_unenc_bad", "pthread_join"},
        {"sem_post_unenc.c", "sem_post_unenc_bad", "sem_wait"},
        {"sem_init_unenc.c", "sem_init_unenc_bad", "sem"},
        {"sem_destroy_unenc.c", "sem_destroy_unenc_bad", "sem"},
        {"fdopendir_unenc.c", "fdopendir_unenc_bad", "opendir"},
        {"readdir_unenc.c", "readdir_unenc_bad", "opendir"},
        {"closedir_unenc.c", "closedir_unenc_bad", "opendir"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover15 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"cap_rights_get_unenc.c", "cap_rights_get_unenc_ok"},
        {"pthread_attr_destroy_unenc.c", "pthread_attr_destroy_unenc_ok"},
        {"pthread_attr_setstacksize_unenc.c", "pthread_attr_setstacksize_unenc_ok"},
        {"pthread_attr_setstack_unenc.c", "pthread_attr_setstack_unenc_ok"},
        {"pthread_attr_setdetachstate_unenc.c", "pthread_attr_setdetachstate_unenc_ok"},
        {"pthread_attr_getstacksize_unenc.c", "pthread_attr_getstacksize_unenc_ok"},
        {"pthread_attr_getstack_unenc.c", "pthread_attr_getstack_unenc_ok"},
        {"pthread_attr_getdetachstate_unenc.c", "pthread_attr_getdetachstate_unenc_ok"},
        {"setcontext_unenc.c", "setcontext_unenc_ok"},
        {"swapcontext_unenc.c", "swapcontext_unenc_ok"},
        {"makecontext_unenc.c", "makecontext_unenc_ok"},
        {"wait3_unenc.c", "wait3_unenc_ok"},
        {"setregid_unenc.c", "setregid_unenc_ok"},
        {"setresuid_unenc.c", "setresuid_unenc_ok"},
        {"setresgid_unenc.c", "setresgid_unenc_ok"},
        {"raise_unenc.c", "raise_unenc_ok"},
        {"alarm_unenc.c", "alarm_unenc_ok"},
        {"pthread_detach_unenc.c", "pthread_detach_unenc_ok"},
        {"sem_post_unenc.c", "sem_post_unenc_ok"},
        {"sem_init_unenc.c", "sem_init_unenc_ok"},
        {"sem_destroy_unenc.c", "sem_destroy_unenc_ok"},
        {"fdopendir_unenc.c", "fdopendir_unenc_ok"},
        {"readdir_unenc.c", "readdir_unenc_ok"},
        {"closedir_unenc.c", "closedir_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover16 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"pthread_once_unenc.c", "pthread_once_unenc_bad", "pthread join"},
        {"pthread_key_delete_unenc.c", "pthread_key_delete_unenc_bad", "pthread_key_create"},
        {"pthread_setspecific_unenc.c", "pthread_setspecific_unenc_bad", "pthread_key_create"},
        {"pthread_getspecific_unenc.c", "pthread_getspecific_unenc_bad", "pthread_key_create"},
        {"pthread_cond_timedwait_unenc.c", "pthread_cond_timedwait_unenc_bad", "pthread_cond"},
        {"pthread_cond_signal_unenc.c", "pthread_cond_signal_unenc_bad", "pthread_cond"},
        {"pthread_cond_broadcast_unenc.c", "pthread_cond_broadcast_unenc_bad", "pthread_cond"},
        {"pthread_cond_init_unenc.c", "pthread_cond_init_unenc_bad", "pthread_cond"},
        {"pthread_cond_destroy_unenc.c", "pthread_cond_destroy_unenc_bad", "pthread_cond"},
        {"pthread_rwlock_wrlock_unenc.c", "pthread_rwlock_wrlock_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_unlock_unenc.c", "pthread_rwlock_unlock_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_init_unenc.c", "pthread_rwlock_init_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_destroy_unenc.c", "pthread_rwlock_destroy_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_tryrdlock_unenc.c", "pthread_rwlock_tryrdlock_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_trywrlock_unenc.c", "pthread_rwlock_trywrlock_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_timedrdlock_unenc.c", "pthread_rwlock_timedrdlock_unenc_bad", "pthread_rwlock"},
        {"pthread_rwlock_timedwrlock_unenc.c", "pthread_rwlock_timedwrlock_unenc_bad", "pthread_rwlock"},
        {"pthread_spin_unlock_unenc.c", "pthread_spin_unlock_unenc_bad", "pthread_spin"},
        {"pthread_spin_trylock_unenc.c", "pthread_spin_trylock_unenc_bad", "pthread_spin"},
        {"pthread_spin_init_unenc.c", "pthread_spin_init_unenc_bad", "pthread_spin"},
        {"pthread_spin_destroy_unenc.c", "pthread_spin_destroy_unenc_bad", "pthread_spin"},
        {"pthread_barrier_init_unenc.c", "pthread_barrier_init_unenc_bad", "pthread_barrier"},
        {"pthread_barrier_destroy_unenc.c", "pthread_barrier_destroy_unenc_bad", "pthread_barrier"},
        {"dup2_unenc.c", "dup2_unenc_bad", "dup"},
        {"dup3_unenc.c", "dup3_unenc_bad", "dup"},
        {"pipe2_unenc.c", "pipe2_unenc_bad", "pipe"},
        {"wait_unenc.c", "wait_unenc_bad", "wait"},
        {"waitid_unenc.c", "waitid_unenc_bad", "wait"},
        {"poll_unenc.c", "poll_unenc_bad", "select"},
        {"pselect_unenc.c", "pselect_unenc_bad", "select"},
        {"epoll_wait_unenc.c", "epoll_wait_unenc_bad", "select"},
        {"epoll_ctl_unenc.c", "epoll_ctl_unenc_bad", "select"},
        {"recvmsg_unenc.c", "recvmsg_unenc_bad", "sendmsg"},
        {"freeaddrinfo_unenc.c", "freeaddrinfo_unenc_bad", "addrinfo"},
        {"sysctlbyname_unenc.c", "sysctlbyname_unenc_bad", "sysctl"},
        {"setfsgid_unenc.c", "setfsgid_unenc_bad", "setfsuid"},
        {"setsid_unenc.c", "setsid_unenc_bad", "setpgid"},
        {"getsid_unenc.c", "getsid_unenc_bad", "setpgid"},
        {"sem_close_unenc.c", "sem_close_unenc_bad", "sem_open"},
        {"sem_unlink_unenc.c", "sem_unlink_unenc_bad", "sem_open"},
        {"getrlimit_unenc.c", "getrlimit_unenc_bad", "setrlimit"},
        {"lstat_unenc.c", "lstat_unenc_bad", "stat"},
        {"fstat_unenc.c", "fstat_unenc_bad", "stat"},
        {"usleep_unenc.c", "usleep_unenc_bad", "sleep"},
        {"nanosleep_unenc.c", "nanosleep_unenc_bad", "sleep"},
        {"aio_write_unenc.c", "aio_write_unenc_bad", "aio"},
        {"aio_error_unenc.c", "aio_error_unenc_bad", "aio"},
        {"aio_return_unenc.c", "aio_return_unenc_bad", "aio"},
        {"aio_suspend_unenc.c", "aio_suspend_unenc_bad", "aio"},
        {"io_uring_enter_unenc.c", "io_uring_enter_unenc_bad", "io_uring"},
        {"io_uring_register_unenc.c", "io_uring_register_unenc_bad", "io_uring"},
        {"lsetxattr_unenc.c", "lsetxattr_unenc_bad", "setxattr"},
        {"fsetxattr_unenc.c", "fsetxattr_unenc_bad", "setxattr"},
        {"getxattr_unenc.c", "getxattr_unenc_bad", "setxattr"},
        {"listxattr_unenc.c", "listxattr_unenc_bad", "setxattr"},
        {"removexattr_unenc.c", "removexattr_unenc_bad", "setxattr"},
        {"landlock_add_rule_unenc.c", "landlock_add_rule_unenc_bad", "landlock"},
        {"landlock_restrict_self_unenc.c", "landlock_restrict_self_unenc_bad", "landlock"},
        {"mq_timedsend_unenc.c", "mq_timedsend_unenc_bad", "mq_unlink"},
        {"mq_timedreceive_unenc.c", "mq_timedreceive_unenc_bad", "mq_unlink"},
        {"mq_notify_unenc.c", "mq_notify_unenc_bad", "mq_unlink"},
        {"mq_getsetattr_unenc.c", "mq_getsetattr_unenc_bad", "mq_unlink"},
        {"globfree_unenc.c", "globfree_unenc_bad", "glob"},
        {"posix_spawnp_unenc.c", "posix_spawnp_unenc_bad", "posix_spawn"},
        {"shm_unlink_unenc.c", "shm_unlink_unenc_bad", "shm_open"},
        {"gettimeofday_unenc.c", "gettimeofday_unenc_bad", "clock_gettime"},
        {"ftell_unenc.c", "ftell_unenc_bad", "fseek"},
        {"rewind_unenc.c", "rewind_unenc_bad", "fseek"},
        {"fgetpos_unenc.c", "fgetpos_unenc_bad", "fseek"},
        {"fsetpos_unenc.c", "fsetpos_unenc_bad", "fseek"},
        {"setsockopt_unenc.c", "setsockopt_unenc_bad", "getsockopt"},
        {"getpeername_unenc.c", "getpeername_unenc_bad", "getsockname"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover16 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"pthread_once_unenc.c", "pthread_once_unenc_ok"},
        {"pthread_key_delete_unenc.c", "pthread_key_delete_unenc_ok"},
        {"pthread_setspecific_unenc.c", "pthread_setspecific_unenc_ok"},
        {"pthread_getspecific_unenc.c", "pthread_getspecific_unenc_ok"},
        {"pthread_cond_timedwait_unenc.c", "pthread_cond_timedwait_unenc_ok"},
        {"pthread_cond_signal_unenc.c", "pthread_cond_signal_unenc_ok"},
        {"pthread_cond_broadcast_unenc.c", "pthread_cond_broadcast_unenc_ok"},
        {"pthread_cond_init_unenc.c", "pthread_cond_init_unenc_ok"},
        {"pthread_cond_destroy_unenc.c", "pthread_cond_destroy_unenc_ok"},
        {"pthread_rwlock_wrlock_unenc.c", "pthread_rwlock_wrlock_unenc_ok"},
        {"pthread_rwlock_unlock_unenc.c", "pthread_rwlock_unlock_unenc_ok"},
        {"pthread_rwlock_init_unenc.c", "pthread_rwlock_init_unenc_ok"},
        {"pthread_rwlock_destroy_unenc.c", "pthread_rwlock_destroy_unenc_ok"},
        {"pthread_rwlock_tryrdlock_unenc.c", "pthread_rwlock_tryrdlock_unenc_ok"},
        {"pthread_rwlock_trywrlock_unenc.c", "pthread_rwlock_trywrlock_unenc_ok"},
        {"pthread_rwlock_timedrdlock_unenc.c", "pthread_rwlock_timedrdlock_unenc_ok"},
        {"pthread_rwlock_timedwrlock_unenc.c", "pthread_rwlock_timedwrlock_unenc_ok"},
        {"pthread_spin_unlock_unenc.c", "pthread_spin_unlock_unenc_ok"},
        {"pthread_spin_trylock_unenc.c", "pthread_spin_trylock_unenc_ok"},
        {"pthread_spin_init_unenc.c", "pthread_spin_init_unenc_ok"},
        {"pthread_spin_destroy_unenc.c", "pthread_spin_destroy_unenc_ok"},
        {"pthread_barrier_init_unenc.c", "pthread_barrier_init_unenc_ok"},
        {"pthread_barrier_destroy_unenc.c", "pthread_barrier_destroy_unenc_ok"},
        {"dup2_unenc.c", "dup2_unenc_ok"},
        {"dup3_unenc.c", "dup3_unenc_ok"},
        {"pipe2_unenc.c", "pipe2_unenc_ok"},
        {"wait_unenc.c", "wait_unenc_ok"},
        {"waitid_unenc.c", "waitid_unenc_ok"},
        {"poll_unenc.c", "poll_unenc_ok"},
        {"pselect_unenc.c", "pselect_unenc_ok"},
        {"epoll_wait_unenc.c", "epoll_wait_unenc_ok"},
        {"epoll_ctl_unenc.c", "epoll_ctl_unenc_ok"},
        {"recvmsg_unenc.c", "recvmsg_unenc_ok"},
        {"freeaddrinfo_unenc.c", "freeaddrinfo_unenc_ok"},
        {"sysctlbyname_unenc.c", "sysctlbyname_unenc_ok"},
        {"setfsgid_unenc.c", "setfsgid_unenc_ok"},
        {"setsid_unenc.c", "setsid_unenc_ok"},
        {"getsid_unenc.c", "getsid_unenc_ok"},
        {"sem_close_unenc.c", "sem_close_unenc_ok"},
        {"sem_unlink_unenc.c", "sem_unlink_unenc_ok"},
        {"getrlimit_unenc.c", "getrlimit_unenc_ok"},
        {"lstat_unenc.c", "lstat_unenc_ok"},
        {"fstat_unenc.c", "fstat_unenc_ok"},
        {"usleep_unenc.c", "usleep_unenc_ok"},
        {"nanosleep_unenc.c", "nanosleep_unenc_ok"},
        {"aio_write_unenc.c", "aio_write_unenc_ok"},
        {"aio_error_unenc.c", "aio_error_unenc_ok"},
        {"aio_return_unenc.c", "aio_return_unenc_ok"},
        {"aio_suspend_unenc.c", "aio_suspend_unenc_ok"},
        {"io_uring_enter_unenc.c", "io_uring_enter_unenc_ok"},
        {"io_uring_register_unenc.c", "io_uring_register_unenc_ok"},
        {"lsetxattr_unenc.c", "lsetxattr_unenc_ok"},
        {"fsetxattr_unenc.c", "fsetxattr_unenc_ok"},
        {"getxattr_unenc.c", "getxattr_unenc_ok"},
        {"listxattr_unenc.c", "listxattr_unenc_ok"},
        {"removexattr_unenc.c", "removexattr_unenc_ok"},
        {"landlock_add_rule_unenc.c", "landlock_add_rule_unenc_ok"},
        {"landlock_restrict_self_unenc.c", "landlock_restrict_self_unenc_ok"},
        {"mq_timedsend_unenc.c", "mq_timedsend_unenc_ok"},
        {"mq_timedreceive_unenc.c", "mq_timedreceive_unenc_ok"},
        {"mq_notify_unenc.c", "mq_notify_unenc_ok"},
        {"mq_getsetattr_unenc.c", "mq_getsetattr_unenc_ok"},
        {"globfree_unenc.c", "globfree_unenc_ok"},
        {"posix_spawnp_unenc.c", "posix_spawnp_unenc_ok"},
        {"shm_unlink_unenc.c", "shm_unlink_unenc_ok"},
        {"gettimeofday_unenc.c", "gettimeofday_unenc_ok"},
        {"ftell_unenc.c", "ftell_unenc_ok"},
        {"rewind_unenc.c", "rewind_unenc_ok"},
        {"fgetpos_unenc.c", "fgetpos_unenc_ok"},
        {"fsetpos_unenc.c", "fsetpos_unenc_ok"},
        {"setsockopt_unenc.c", "setsockopt_unenc_ok"},
        {"getpeername_unenc.c", "getpeername_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover17 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"posix_memalign_unenc.c", "posix_memalign_unenc_bad", "aligned_alloc"},
        {"munlock_unenc.c", "munlock_unenc_bad", "mlock"},
        {"mlockall_unenc.c", "mlockall_unenc_bad", "mlock"},
        {"munlockall_unenc.c", "munlockall_unenc_bad", "mlock"},
        {"sched_getaffinity_unenc.c", "sched_getaffinity_unenc_bad", "sched_setaffinity"},
        {"capget_unenc.c", "capget_unenc_bad", "capset"},
        {"pidfd_send_signal_unenc.c", "pidfd_send_signal_unenc_bad", "pidfd_open"},
        {"pidfd_getfd_unenc.c", "pidfd_getfd_unenc_bad", "pidfd_open"},
        {"setpriority_unenc.c", "setpriority_unenc_bad", "getpriority"},
        {"setgroups_unenc.c", "setgroups_unenc_bad", "initgroups"},
        {"setns_unenc.c", "setns_unenc_bad", "unshare"},
        {"recvmmsg_unenc.c", "recvmmsg_unenc_bad", "sendmmsg"},
        {"io_destroy_unenc.c", "io_destroy_unenc_bad", "io_setup"},
        {"io_cancel_unenc.c", "io_cancel_unenc_bad", "io_setup"},
        {"io_pgetevents_unenc.c", "io_pgetevents_unenc_bad", "io_setup"},
        {"io_getevents_unenc.c", "io_getevents_unenc_bad", "io_submit"},
        {"shmdt_unenc.c", "shmdt_unenc_bad", "shmat"},
        {"semtimedop_unenc.c", "semtimedop_unenc_bad", "semop"},
        {"msgrcv_unenc.c", "msgrcv_unenc_bad", "msgsnd"},
        {"timer_gettime_unenc.c", "timer_gettime_unenc_bad", "timer_delete"},
        {"timer_getoverrun_unenc.c", "timer_getoverrun_unenc_bad", "timer_delete"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover17 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"posix_memalign_unenc.c", "posix_memalign_unenc_ok"},
        {"munlock_unenc.c", "munlock_unenc_ok"},
        {"mlockall_unenc.c", "mlockall_unenc_ok"},
        {"munlockall_unenc.c", "munlockall_unenc_ok"},
        {"sched_getaffinity_unenc.c", "sched_getaffinity_unenc_ok"},
        {"capget_unenc.c", "capget_unenc_ok"},
        {"pidfd_send_signal_unenc.c", "pidfd_send_signal_unenc_ok"},
        {"pidfd_getfd_unenc.c", "pidfd_getfd_unenc_ok"},
        {"setpriority_unenc.c", "setpriority_unenc_ok"},
        {"setgroups_unenc.c", "setgroups_unenc_ok"},
        {"setns_unenc.c", "setns_unenc_ok"},
        {"recvmmsg_unenc.c", "recvmmsg_unenc_ok"},
        {"io_destroy_unenc.c", "io_destroy_unenc_ok"},
        {"io_cancel_unenc.c", "io_cancel_unenc_ok"},
        {"io_pgetevents_unenc.c", "io_pgetevents_unenc_ok"},
        {"io_getevents_unenc.c", "io_getevents_unenc_ok"},
        {"shmdt_unenc.c", "shmdt_unenc_ok"},
        {"semtimedop_unenc.c", "semtimedop_unenc_ok"},
        {"msgrcv_unenc.c", "msgrcv_unenc_ok"},
        {"timer_gettime_unenc.c", "timer_gettime_unenc_ok"},
        {"timer_getoverrun_unenc.c", "timer_getoverrun_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover18 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"mmap_unenc.c", "mmap_unenc_bad", "mmap"},
        {"munmap_unenc.c", "munmap_unenc_bad", "mmap"},
        {"mprotect_unenc.c", "mprotect_unenc_bad", "mmap"},
        {"chmod_unenc.c", "chmod_unenc_bad", "chmod"},
        {"fchmod_unenc.c", "fchmod_unenc_bad", "chmod"},
        {"mkdir_unenc.c", "mkdir_unenc_bad", "mkdir"},
        {"rmdir_unenc.c", "rmdir_unenc_bad", "mkdir"},
        {"rename_unenc.c", "rename_unenc_bad", "mkdir"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover18 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"mmap_unenc.c", "mmap_unenc_ok"},
        {"munmap_unenc.c", "munmap_unenc_ok"},
        {"mprotect_unenc.c", "mprotect_unenc_ok"},
        {"chmod_unenc.c", "chmod_unenc_ok"},
        {"fchmod_unenc.c", "fchmod_unenc_ok"},
        {"mkdir_unenc.c", "mkdir_unenc_ok"},
        {"rmdir_unenc.c", "rmdir_unenc_ok"},
        {"rename_unenc.c", "rename_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover19 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"clone_call_unenc.c", "clone_call_unenc_bad", "unshare"},
        {"listmount_call_unenc.c", "listmount_call_unenc_bad", "listmount"},
        {"fallocate_call_unenc.c", "fallocate_call_unenc_bad", "fallocate"},
        {"epoll_create1_unenc.c", "epoll_create1_unenc_bad", "epoll_create"},
        {"epoll_pwait2_unenc.c", "epoll_pwait2_unenc_bad", "epoll_pwait"},
        {"rt_tgsigqueueinfo_unenc.c", "rt_tgsigqueueinfo_unenc_bad", "rt_sigqueueinfo"},
        {"file_setattr_unenc.c", "file_setattr_unenc_bad", "file_getattr"},
        {"clock_adjtime_unenc.c", "clock_adjtime_unenc_bad", "clock_settime"},
        {"clock_nanosleep_unenc.c", "clock_nanosleep_unenc_bad", "clock_settime"},
        {"quick_exit_unenc.c", "quick_exit_unenc_bad", "quick_exit"},
        {"getopt_long_only_unenc.c", "getopt_long_only_unenc_bad", "getopt"},
        {"getopt_long_unenc.c", "getopt_long_unenc_bad", "getopt"},
        {"gethostname_unenc.c", "gethostname_unenc_bad", "uname"},
        {"copy_file_range_unenc.c", "copy_file_range_unenc_bad", "sendfile"},
        {"preadv2_unenc.c", "preadv2_unenc_bad", "preadv"},
        {"pwritev2_unenc.c", "pwritev2_unenc_bad", "preadv"},
        {"pwritev_unenc.c", "pwritev_unenc_bad", "preadv"},
        {"timerfd_gettime_unenc.c", "timerfd_gettime_unenc_bad", "timerfd_settime"},
        {"eventfd_write_unenc.c", "eventfd_write_unenc_bad", "eventfd_read"},
        {"eventfd_unenc.c", "eventfd_unenc_bad", "memfd"},
        {"timerfd_create_unenc.c", "timerfd_create_unenc_bad", "memfd"},
        {"ptrace_unenc.c", "ptrace_unenc_bad", "prctl"},
        {"tcsetattr_unenc.c", "tcsetattr_unenc_bad", "tcgetattr"},
        {"cfmakeraw_unenc.c", "cfmakeraw_unenc_bad", "tcgetattr"},
        {"fpathconf_unenc.c", "fpathconf_unenc_bad", "sysconf"},
        {"pathconf_unenc.c", "pathconf_unenc_bad", "sysconf"},
        {"ftw_unenc.c", "ftw_unenc_bad", "nftw"},
        {"wordfree_unenc.c", "wordfree_unenc_bad", "wordexp"},
        {"getlogin_r_unenc.c", "getlogin_r_unenc_bad", "getlogin"},
        {"ttyname_r_unenc.c", "ttyname_r_unenc_bad", "getlogin"},
        {"ttyname_unenc.c", "ttyname_unenc_bad", "getlogin"},
        {"inet_ntop_unenc.c", "inet_ntop_unenc_bad", "inet_pton"},
        {"inet_aton_unenc.c", "inet_aton_unenc_bad", "inet_pton"},
        {"posix_fadvise64_unenc.c", "posix_fadvise64_unenc_bad", "posix_fadvise"},
        {"vmsplice_unenc.c", "vmsplice_unenc_bad", "splice"},
        {"inotify_init1_unenc.c", "inotify_init1_unenc_bad", "inotify"},
        {"inotify_add_watch_unenc.c", "inotify_add_watch_unenc_bad", "inotify"},
        {"fdatasync_unenc.c", "fdatasync_unenc_bad", "fsync"},
        {"getentropy_unenc.c", "getentropy_unenc_bad", "getrandom"},
        {"getdelim_unenc.c", "getdelim_unenc_bad", "getline"},
        {"strlcat_unenc.c", "strlcat_unenc_bad", "strlcpy"},
        {"memset_s_unenc.c", "memset_s_unenc_bad", "explicit_bzero"},
        {"explicit_memset_unenc.c", "explicit_memset_unenc_bad", "explicit_bzero"},
        {"posix_openpt_unenc.c", "posix_openpt_unenc_bad", "ptsname"},
        {"ptsname_r_unenc.c", "ptsname_r_unenc_bad", "ptsname"},
        {"grantpt_unenc.c", "grantpt_unenc_bad", "ptsname"},
        {"unlockpt_unenc.c", "unlockpt_unenc_bad", "ptsname"},
        {"umount2_unenc.c", "umount2_unenc_bad", "mount"},
        {"umount_unenc.c", "umount_unenc_bad", "mount"},
        {"open_wmemstream_unenc.c", "open_wmemstream_unenc_bad", "fmemopen"},
        {"open_memstream_unenc.c", "open_memstream_unenc_bad", "fmemopen"},
        {"getxattrat_unenc.c", "getxattrat_unenc_bad", "setxattrat"},
        {"listxattrat_unenc.c", "listxattrat_unenc_bad", "setxattrat"},
        {"removexattrat_unenc.c", "removexattrat_unenc_bad", "setxattrat"},
        {"sched_getattr_unenc.c", "sched_getattr_unenc_bad", "sched_setattr"},
        {"sched_getscheduler_unenc.c", "sched_getscheduler_unenc_bad", "sched_setscheduler"},
        {"sched_setparam_unenc.c", "sched_setparam_unenc_bad", "sched_setscheduler"},
        {"sched_getparam_unenc.c", "sched_getparam_unenc_bad", "sched_setscheduler"},
        {"fanotify_mark_unenc.c", "fanotify_mark_unenc_bad", "fanotify"},
        {"getgrgid_unenc.c", "getgrgid_unenc_bad", "getgrnam"},
        {"getspnam_unenc.c", "getspnam_unenc_bad", "getgrnam"},
        {"lsm_set_self_attr_unenc.c", "lsm_set_self_attr_unenc_bad", "lsm_get_self_attr"},
        {"lsm_list_modules_unenc.c", "lsm_list_modules_unenc_bad", "lsm_get_self_attr"},
        {"sigsuspend_unenc.c", "sigsuspend_unenc_bad", "sigprocmask"},
        {"sigwaitinfo_unenc.c", "sigwaitinfo_unenc_bad", "sigwait"},
        {"sigtimedwait_unenc.c", "sigtimedwait_unenc_bad", "sigwait"},
        {"sigpending_unenc.c", "sigpending_unenc_bad", "sigwait"},
        {"open_by_handle_at_unenc.c", "open_by_handle_at_unenc_bad", "name_to_handle"},
        {"prlimit64_unenc.c", "prlimit64_unenc_bad", "prlimit"},
        {"migrate_pages_unenc.c", "migrate_pages_unenc_bad", "move_pages"},
        {"fsmount_unenc.c", "fsmount_unenc_bad", "fsopen"},
        {"open_tree_unenc.c", "open_tree_unenc_bad", "fsopen"},
        {"move_mount_unenc.c", "move_mount_unenc_bad", "fsopen"},
        {"fspick_unenc.c", "fspick_unenc_bad", "fsopen"},
        {"fsconfig_unenc.c", "fsconfig_unenc_bad", "fsopen"},
        {"ioprio_get_unenc.c", "ioprio_get_unenc_bad", "ioprio"},
        {"futex_wake_unenc.c", "futex_wake_unenc_bad", "futex_wait"},
        {"futex_requeue_unenc.c", "futex_requeue_unenc_bad", "futex_wait"},
        {"swapoff_unenc.c", "swapoff_unenc_bad", "swapon"},
        {"iopl_unenc.c", "iopl_unenc_bad", "ioperm"},
        {"finit_module_unenc.c", "finit_module_unenc_bad", "init_module"},
        {"delete_module_unenc.c", "delete_module_unenc_bad", "init_module"},
        {"kexec_file_load_unenc.c", "kexec_file_load_unenc_bad", "kexec"},
        {"pkey_mprotect_unenc.c", "pkey_mprotect_unenc_bad", "pkey_free"},
        {"mremap_unenc.c", "mremap_unenc_bad", "msync"},
        {"getitimer_unenc.c", "getitimer_unenc_bad", "setitimer"},
        {"getdents64_unenc.c", "getdents64_unenc_bad", "getdents"},
        {"futimens_unenc.c", "futimens_unenc_bad", "utimensat"},
        {"utimes_unenc.c", "utimes_unenc_bad", "utimensat"},
        {"set_mempolicy_unenc.c", "set_mempolicy_unenc_bad", "mbind"},
        {"get_mempolicy_unenc.c", "get_mempolicy_unenc_bad", "mbind"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover19 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"clone_call_unenc.c", "clone_call_unenc_ok"},
        {"listmount_call_unenc.c", "listmount_call_unenc_ok"},
        {"fallocate_call_unenc.c", "fallocate_call_unenc_ok"},
        {"epoll_create1_unenc.c", "epoll_create1_unenc_ok"},
        {"epoll_pwait2_unenc.c", "epoll_pwait2_unenc_ok"},
        {"rt_tgsigqueueinfo_unenc.c", "rt_tgsigqueueinfo_unenc_ok"},
        {"file_setattr_unenc.c", "file_setattr_unenc_ok"},
        {"clock_adjtime_unenc.c", "clock_adjtime_unenc_ok"},
        {"clock_nanosleep_unenc.c", "clock_nanosleep_unenc_ok"},
        {"quick_exit_unenc.c", "quick_exit_unenc_ok"},
        {"getopt_long_only_unenc.c", "getopt_long_only_unenc_ok"},
        {"getopt_long_unenc.c", "getopt_long_unenc_ok"},
        {"gethostname_unenc.c", "gethostname_unenc_ok"},
        {"copy_file_range_unenc.c", "copy_file_range_unenc_ok"},
        {"preadv2_unenc.c", "preadv2_unenc_ok"},
        {"pwritev2_unenc.c", "pwritev2_unenc_ok"},
        {"pwritev_unenc.c", "pwritev_unenc_ok"},
        {"timerfd_gettime_unenc.c", "timerfd_gettime_unenc_ok"},
        {"eventfd_write_unenc.c", "eventfd_write_unenc_ok"},
        {"eventfd_unenc.c", "eventfd_unenc_ok"},
        {"timerfd_create_unenc.c", "timerfd_create_unenc_ok"},
        {"ptrace_unenc.c", "ptrace_unenc_ok"},
        {"tcsetattr_unenc.c", "tcsetattr_unenc_ok"},
        {"cfmakeraw_unenc.c", "cfmakeraw_unenc_ok"},
        {"fpathconf_unenc.c", "fpathconf_unenc_ok"},
        {"pathconf_unenc.c", "pathconf_unenc_ok"},
        {"ftw_unenc.c", "ftw_unenc_ok"},
        {"wordfree_unenc.c", "wordfree_unenc_ok"},
        {"getlogin_r_unenc.c", "getlogin_r_unenc_ok"},
        {"ttyname_r_unenc.c", "ttyname_r_unenc_ok"},
        {"ttyname_unenc.c", "ttyname_unenc_ok"},
        {"inet_ntop_unenc.c", "inet_ntop_unenc_ok"},
        {"inet_aton_unenc.c", "inet_aton_unenc_ok"},
        {"posix_fadvise64_unenc.c", "posix_fadvise64_unenc_ok"},
        {"vmsplice_unenc.c", "vmsplice_unenc_ok"},
        {"inotify_init1_unenc.c", "inotify_init1_unenc_ok"},
        {"inotify_add_watch_unenc.c", "inotify_add_watch_unenc_ok"},
        {"fdatasync_unenc.c", "fdatasync_unenc_ok"},
        {"getentropy_unenc.c", "getentropy_unenc_ok"},
        {"getdelim_unenc.c", "getdelim_unenc_ok"},
        {"strlcat_unenc.c", "strlcat_unenc_ok"},
        {"memset_s_unenc.c", "memset_s_unenc_ok"},
        {"explicit_memset_unenc.c", "explicit_memset_unenc_ok"},
        {"posix_openpt_unenc.c", "posix_openpt_unenc_ok"},
        {"ptsname_r_unenc.c", "ptsname_r_unenc_ok"},
        {"grantpt_unenc.c", "grantpt_unenc_ok"},
        {"unlockpt_unenc.c", "unlockpt_unenc_ok"},
        {"umount2_unenc.c", "umount2_unenc_ok"},
        {"umount_unenc.c", "umount_unenc_ok"},
        {"open_wmemstream_unenc.c", "open_wmemstream_unenc_ok"},
        {"open_memstream_unenc.c", "open_memstream_unenc_ok"},
        {"getxattrat_unenc.c", "getxattrat_unenc_ok"},
        {"listxattrat_unenc.c", "listxattrat_unenc_ok"},
        {"removexattrat_unenc.c", "removexattrat_unenc_ok"},
        {"sched_getattr_unenc.c", "sched_getattr_unenc_ok"},
        {"sched_getscheduler_unenc.c", "sched_getscheduler_unenc_ok"},
        {"sched_setparam_unenc.c", "sched_setparam_unenc_ok"},
        {"sched_getparam_unenc.c", "sched_getparam_unenc_ok"},
        {"fanotify_mark_unenc.c", "fanotify_mark_unenc_ok"},
        {"getgrgid_unenc.c", "getgrgid_unenc_ok"},
        {"getspnam_unenc.c", "getspnam_unenc_ok"},
        {"lsm_set_self_attr_unenc.c", "lsm_set_self_attr_unenc_ok"},
        {"lsm_list_modules_unenc.c", "lsm_list_modules_unenc_ok"},
        {"sigsuspend_unenc.c", "sigsuspend_unenc_ok"},
        {"sigwaitinfo_unenc.c", "sigwaitinfo_unenc_ok"},
        {"sigtimedwait_unenc.c", "sigtimedwait_unenc_ok"},
        {"sigpending_unenc.c", "sigpending_unenc_ok"},
        {"open_by_handle_at_unenc.c", "open_by_handle_at_unenc_ok"},
        {"prlimit64_unenc.c", "prlimit64_unenc_ok"},
        {"migrate_pages_unenc.c", "migrate_pages_unenc_ok"},
        {"fsmount_unenc.c", "fsmount_unenc_ok"},
        {"open_tree_unenc.c", "open_tree_unenc_ok"},
        {"move_mount_unenc.c", "move_mount_unenc_ok"},
        {"fspick_unenc.c", "fspick_unenc_ok"},
        {"fsconfig_unenc.c", "fsconfig_unenc_ok"},
        {"ioprio_get_unenc.c", "ioprio_get_unenc_ok"},
        {"futex_wake_unenc.c", "futex_wake_unenc_ok"},
        {"futex_requeue_unenc.c", "futex_requeue_unenc_ok"},
        {"swapoff_unenc.c", "swapoff_unenc_ok"},
        {"iopl_unenc.c", "iopl_unenc_ok"},
        {"finit_module_unenc.c", "finit_module_unenc_ok"},
        {"delete_module_unenc.c", "delete_module_unenc_ok"},
        {"kexec_file_load_unenc.c", "kexec_file_load_unenc_ok"},
        {"pkey_mprotect_unenc.c", "pkey_mprotect_unenc_ok"},
        {"mremap_unenc.c", "mremap_unenc_ok"},
        {"getitimer_unenc.c", "getitimer_unenc_ok"},
        {"getdents64_unenc.c", "getdents64_unenc_ok"},
        {"futimens_unenc.c", "futimens_unenc_ok"},
        {"utimes_unenc.c", "utimes_unenc_ok"},
        {"set_mempolicy_unenc.c", "set_mempolicy_unenc_ok"},
        {"get_mempolicy_unenc.c", "get_mempolicy_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover20 unenc plants are NEEDS-HARNESS never PROVED") {
    auto fn = load_fn("twnested_unenc.c", "twnested_unenc_bad");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(findings[0].status != std::string(prism::laws::PROVED));
    CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
    CHECK(findings[0].status != std::string(prism::laws::FAILED));
    CHECK(findings[0].status != std::string(prism::laws::ERROR));
    CHECK_FALSE(prism::laws::is_proof(findings[0].status));
    std::string msg = findings[0].message;
    for (char& c : msg)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    CHECK(msg.find("throw_with_nested") != std::string::npos);
}

TEST_CASE("bmc leftover20 unenc ok still not NEEDS-HARNESS") {
    auto fn = load_fn("twnested_unenc.c", "twnested_unenc_ok");
    auto findings = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
    CHECK(findings[0].status != std::string(prism::laws::ERROR));
    CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
           findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
}

TEST_CASE("bmc leftover21 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"decimal32_unenc.c", "decimal32_unenc_bad", "decimal"},
        {"decimal128_unenc.c", "decimal128_unenc_bad", "decimal"},
        {"float32_unenc.c", "float32_unenc_bad", "ieee"},
        {"float64_unenc.c", "float64_unenc_bad", "ieee"},
        {"fp16_unenc.c", "fp16_unenc_bad", "ieee"},
        {"bitint_unenc.c", "bitint_unenc_bad", "128"},
        {"int128t_unenc.c", "int128t_unenc_bad", "128"},
        {"slaarray_unenc.c", "slaarray_unenc_bad", "start_lifetime_as"},
        {"inoutptr_unenc.c", "inoutptr_unenc_bad", "out_ptr"},
        {"sref_unenc.c", "sref_unenc_bad", "reference_wrapper"},
        {"scref_unenc.c", "scref_unenc_bad", "reference_wrapper"},
        {"csem_unenc.cpp", "csem_unenc_bad", "latch"},
        {"poly_unenc.cpp", "poly_unenc_bad", "indirect"},
        {"wview_unenc.cpp", "wview_unenc_bad", "wstring"},
        {"isps_unenc.cpp", "isps_unenc_bad", "spanstream"},
        {"osps_unenc.cpp", "osps_unenc_bad", "spanstream"},
        {"rcuobj_unenc.cpp", "rcuobj_unenc_bad", "rcu"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover21 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"decimal32_unenc.c", "decimal32_unenc_ok"},
        {"decimal128_unenc.c", "decimal128_unenc_ok"},
        {"float32_unenc.c", "float32_unenc_ok"},
        {"float64_unenc.c", "float64_unenc_ok"},
        {"fp16_unenc.c", "fp16_unenc_ok"},
        {"bitint_unenc.c", "bitint_unenc_ok"},
        {"int128t_unenc.c", "int128t_unenc_ok"},
        {"slaarray_unenc.c", "slaarray_unenc_ok"},
        {"inoutptr_unenc.c", "inoutptr_unenc_ok"},
        {"sref_unenc.c", "sref_unenc_ok"},
        {"scref_unenc.c", "scref_unenc_ok"},
        {"csem_unenc.cpp", "csem_unenc_ok"},
        {"poly_unenc.cpp", "poly_unenc_ok"},
        {"wview_unenc.cpp", "wview_unenc_ok"},
        {"isps_unenc.cpp", "isps_unenc_ok"},
        {"osps_unenc.cpp", "osps_unenc_ok"},
        {"rcuobj_unenc.cpp", "rcuobj_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover22 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"bosync_unenc.cpp", "bosync_unenc_bad", "osyncstream"},
        {"bsyncbuf_unenc.cpp", "bsyncbuf_unenc_bad", "syncbuf"},
        {"bspan_unenc.cpp", "bspan_unenc_bad", "spanstream"},
        {"bispan_unenc.cpp", "bispan_unenc_bad", "spanstream"},
        {"bospan_unenc.cpp", "bospan_unenc_bad", "spanstream"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover22 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"bosync_unenc.cpp", "bosync_unenc_ok"},
        {"bsyncbuf_unenc.cpp", "bsyncbuf_unenc_ok"},
        {"bspan_unenc.cpp", "bspan_unenc_ok"},
        {"bispan_unenc.cpp", "bispan_unenc_ok"},
        {"bospan_unenc.cpp", "bospan_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover23 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"tounqual_unenc.c", "tounqual_unenc_bad", "typeof_unqual"},
        {"dlsym_unenc.c", "dlsym_unenc_bad", "dlopen"},
        {"dlclose_unenc.c", "dlclose_unenc_bad", "dlopen"},
        {"clzll_unenc.c", "clzll_unenc_bad", "clz"},
        {"ctz_unenc.c", "ctz_unenc_bad", "clz"},
        {"ctzll_unenc.c", "ctzll_unenc_bad", "clz"},
        {"astore_unenc.c", "astore_unenc_bad", "atomic"},
        {"syncadd_unenc.c", "syncadd_unenc_bad", "atomic"},
        {"synccas_unenc.c", "synccas_unenc_bad", "atomic"},
        {"wcscat_unenc.c", "wcscat_unenc_bad", "wcscpy"},
        {"wcsncpy_unenc.c", "wcsncpy_unenc_bad", "wcscpy"},
        {"wcsncat_unenc.c", "wcsncat_unenc_bad", "wcscpy"},
        {"fork_unenc.c", "fork_unenc_bad", "fork"},
        {"vfork_unenc.c", "vfork_unenc_bad", "fork"},
        {"execl_unenc.c", "execl_unenc_bad", "fork"},
        {"execlp_unenc.c", "execlp_unenc_bad", "fork"},
        {"execle_unenc.c", "execle_unenc_bad", "fork"},
        {"execv_unenc.c", "execv_unenc_bad", "fork"},
        {"execve_unenc.c", "execve_unenc_bad", "fork"},
        {"execvp_unenc.c", "execvp_unenc_bad", "fork"},
        {"execvpe_unenc.c", "execvpe_unenc_bad", "fork"},
        {"symlink_unenc.c", "symlink_unenc_bad", "symlink"},
        {"readlink_unenc.c", "readlink_unenc_bad", "symlink"},
        {"sstream_unenc.cpp", "sstream_unenc_bad", "stringstream"},
        {"osstream_unenc.cpp", "osstream_unenc_bad", "stringstream"},
        {"isstream_unenc.cpp", "isstream_unenc_bad", "stringstream"},
        {"bsstream_unenc.cpp", "bsstream_unenc_bad", "stringstream"},
        {"bosstream_unenc.cpp", "bosstream_unenc_bad", "stringstream"},
        {"bisstream_unenc.cpp", "bisstream_unenc_bad", "stringstream"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover23 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"tounqual_unenc.c", "tounqual_unenc_ok"},
        {"dlsym_unenc.c", "dlsym_unenc_ok"},
        {"dlclose_unenc.c", "dlclose_unenc_ok"},
        {"clzll_unenc.c", "clzll_unenc_ok"},
        {"ctz_unenc.c", "ctz_unenc_ok"},
        {"ctzll_unenc.c", "ctzll_unenc_ok"},
        {"astore_unenc.c", "astore_unenc_ok"},
        {"syncadd_unenc.c", "syncadd_unenc_ok"},
        {"synccas_unenc.c", "synccas_unenc_ok"},
        {"wcscat_unenc.c", "wcscat_unenc_ok"},
        {"wcsncpy_unenc.c", "wcsncpy_unenc_ok"},
        {"wcsncat_unenc.c", "wcsncat_unenc_ok"},
        {"fork_unenc.c", "fork_unenc_ok"},
        {"vfork_unenc.c", "vfork_unenc_ok"},
        {"execl_unenc.c", "execl_unenc_ok"},
        {"execlp_unenc.c", "execlp_unenc_ok"},
        {"execle_unenc.c", "execle_unenc_ok"},
        {"execv_unenc.c", "execv_unenc_ok"},
        {"execve_unenc.c", "execve_unenc_ok"},
        {"execvp_unenc.c", "execvp_unenc_ok"},
        {"execvpe_unenc.c", "execvpe_unenc_ok"},
        {"symlink_unenc.c", "symlink_unenc_ok"},
        {"readlink_unenc.c", "readlink_unenc_ok"},
        {"sstream_unenc.cpp", "sstream_unenc_ok"},
        {"osstream_unenc.cpp", "osstream_unenc_ok"},
        {"isstream_unenc.cpp", "isstream_unenc_ok"},
        {"bsstream_unenc.cpp", "bsstream_unenc_ok"},
        {"bosstream_unenc.cpp", "bosstream_unenc_ok"},
        {"bisstream_unenc.cpp", "bisstream_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover24 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"tlocal_unenc.c", "tlocal_unenc_bad", "thread-local"},
        {"complex_unenc.c", "complex_unenc_bad", "complex"},
        {"typeof_unenc.c", "typeof_unenc_bad", "typeof"},
        {"alignof_unenc.c", "alignof_unenc_bad", "alignof"},
        {"sthread_unenc.cpp", "sthread_unenc_bad", "std::thread"},
        {"counted_unenc.cpp", "counted_unenc_bad", "counted_iterator"},
        {"vector_unenc.cpp", "vector_unenc_bad", "std::vector"},
        {"optional_unenc.cpp", "optional_unenc_bad", "optional"},
        {"variant_unenc.cpp", "variant_unenc_bad", "variant"},
        {"span_unenc.cpp", "span_unenc_bad", "std::span"},
        {"ilist_unenc.cpp", "ilist_unenc_bad", "initializer_list"},
        {"coro_unenc.cpp", "coro_unenc_bad", "coroutine"},
        {"packed_unenc.c", "packed_unenc_bad", "packed"},
        {"asm_unenc.c", "asm_unenc_bad", "asm"},
        {"generic_unenc.c", "generic_unenc_bad", "_generic"},
        {"offsetof_unenc.c", "offsetof_unenc_bad", "offsetof"},
        {"volatile_unenc.c", "volatile_unenc_bad", "volatile"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover24 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"tlocal_unenc.c", "tlocal_unenc_ok"},
        {"complex_unenc.c", "complex_unenc_ok"},
        {"typeof_unenc.c", "typeof_unenc_ok"},
        {"alignof_unenc.c", "alignof_unenc_ok"},
        {"sthread_unenc.cpp", "sthread_unenc_ok"},
        {"counted_unenc.cpp", "counted_unenc_ok"},
        {"vector_unenc.cpp", "vector_unenc_ok"},
        {"optional_unenc.cpp", "optional_unenc_ok"},
        {"variant_unenc.cpp", "variant_unenc_ok"},
        {"span_unenc.cpp", "span_unenc_ok"},
        {"ilist_unenc.cpp", "ilist_unenc_ok"},
        {"coro_unenc.cpp", "coro_unenc_ok"},
        {"packed_unenc.c", "packed_unenc_ok"},
        {"asm_unenc.c", "asm_unenc_ok"},
        {"generic_unenc.c", "generic_unenc_ok"},
        {"offsetof_unenc.c", "offsetof_unenc_ok"},
        {"volatile_unenc.c", "volatile_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover25 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"rangefor_unenc.cpp", "rangefor_unenc_bad", "range-for"},
        {"lambda_unenc.cpp", "lambda_unenc_bad", "lambda"},
        {"ccast_unenc.cpp", "ccast_unenc_bad", "const_cast"},
        {"dcast_unenc.cpp", "dcast_unenc_bad", "dynamic_cast"},
        {"tid_unenc.cpp", "tid_unenc_bad", "typeid"},
        {"rcast_unenc.cpp", "rcast_unenc_bad", "reinterpret_cast"},
        {"sbind_unenc.cpp", "sbind_unenc_bad", "std::bind"},
        {"inplace_unenc.cpp", "inplace_unenc_bad", "inplace_vector"},
        {"catchall_unenc.cpp", "catchall_unenc_bad", "catch-all"},
        {"thrownew_unenc.cpp", "thrownew_unenc_bad", "throw-new"},
        {"sfrom_unenc.cpp", "sfrom_unenc_bad", "shared_from_this"},
        {"ppack_unenc.c", "ppack_unenc_bad", "pragma pack"},
        {"widech_unenc.c", "widech_unenc_bad", "wide character"},
        {"widestr_unenc.c", "widestr_unenc_bad", "wide character"},
        {"trycatch_unenc.cpp", "trycatch_unenc_bad", "try/catch"},
        {"newdel_unenc.cpp", "newdel_unenc_bad", "new/delete"},
        {"cgoto_unenc.c", "cgoto_unenc_bad", "computed goto"},
        {"laddr_unenc.c", "laddr_unenc_bad", "label-address"},
        {"vaarg_unenc.c", "vaarg_unenc_bad", "va_arg"},
        {"dinit_unenc.c", "dinit_unenc_bad", "designated init"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover25 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"rangefor_unenc.cpp", "rangefor_unenc_ok"},
        {"lambda_unenc.cpp", "lambda_unenc_ok"},
        {"ccast_unenc.cpp", "ccast_unenc_ok"},
        {"dcast_unenc.cpp", "dcast_unenc_ok"},
        {"tid_unenc.cpp", "tid_unenc_ok"},
        {"rcast_unenc.cpp", "rcast_unenc_ok"},
        {"sbind_unenc.cpp", "sbind_unenc_ok"},
        {"inplace_unenc.cpp", "inplace_unenc_ok"},
        {"catchall_unenc.cpp", "catchall_unenc_ok"},
        {"thrownew_unenc.cpp", "thrownew_unenc_ok"},
        {"sfrom_unenc.cpp", "sfrom_unenc_ok"},
        {"ppack_unenc_ok.c", "ppack_unenc_ok"},
        {"widech_unenc.c", "widech_unenc_ok"},
        {"widestr_unenc.c", "widestr_unenc_ok"},
        {"trycatch_unenc.cpp", "trycatch_unenc_ok"},
        {"newdel_unenc.cpp", "newdel_unenc_ok"},
        {"cgoto_unenc.c", "cgoto_unenc_ok"},
        {"laddr_unenc.c", "laddr_unenc_ok"},
        {"vaarg_unenc.c", "vaarg_unenc_ok"},
        {"dinit_unenc.c", "dinit_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover26 unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"nfn_unenc.c", "nfn_unenc_bad", "nested function"},
        {"uaddr_unenc.c", "uaddr_unenc_bad", "address-of"},
        {"clocal_unenc.c", "clocal_unenc_bad", "const local"},
        {"regstor_unenc.c", "regstor_unenc_bad", "register/auto"},
        {"autostor_unenc.c", "autostor_unenc_bad", "register/auto"},
        {"slocal_unenc.c", "slocal_unenc_bad", "struct/union local"},
        {"staticloc_unenc.c", "staticloc_unenc_bad", "static/extern"},
        {"externloc_unenc.c", "externloc_unenc_bad", "static/extern"},
        {"stmtexpr_unenc.c", "stmtexpr_unenc_bad", "statement expression"},
        {"autotype_unenc.c", "autotype_unenc_bad", "__auto_type"},
        {"aenum_unenc.c", "aenum_unenc_bad", "anonymous enum"},
        {"alignas_unenc.c", "alignas_unenc_bad", "_alignas"},
        {"compound_unenc.c", "compound_unenc_bad", "compound literal"},
        {"rviews_unenc.cpp", "rviews_unenc_bad", "ranges views"},
        {"unlink_unenc.c", "unlink_unenc_bad", "unlink"},
        {"strinit_unenc.c", "strinit_unenc_bad", "array string-init"},
        {"pmtx_unenc.c", "pmtx_unenc_bad", "mutex object"},
        {"cmtx_unenc.c", "cmtx_unenc_bad", "mutex object"},
        {"utypedef_unenc.c", "utypedef_unenc_bad", "unknown typedef"},
        {"memcpy_unenc.c", "memcpy_unenc_bad", "libc buffer"},
        {"memmove_unenc.c", "memmove_unenc_bad", "libc buffer"},
        {"mkstemp_unenc.c", "mkstemp_unenc_bad", "libc buffer"},
        {"chroot_unenc.c", "chroot_unenc_bad", "libc buffer"},
        {"popen_unenc.c", "popen_unenc_bad", "libc buffer"},
        {"umask_unenc.c", "umask_unenc_bad", "libc buffer"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover26 unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"nfn_unenc.c", "nfn_unenc_ok"},
        {"uaddr_unenc.c", "uaddr_unenc_ok"},
        {"clocal_unenc.c", "clocal_unenc_ok"},
        {"regstor_unenc.c", "regstor_unenc_ok"},
        {"autostor_unenc.c", "autostor_unenc_ok"},
        {"slocal_unenc.c", "slocal_unenc_ok"},
        {"staticloc_unenc.c", "staticloc_unenc_ok"},
        {"externloc_unenc.c", "externloc_unenc_ok"},
        {"stmtexpr_unenc.c", "stmtexpr_unenc_ok"},
        {"autotype_unenc.c", "autotype_unenc_ok"},
        {"aenum_unenc.c", "aenum_unenc_ok"},
        {"alignas_unenc.c", "alignas_unenc_ok"},
        {"compound_unenc.c", "compound_unenc_ok"},
        {"rviews_unenc.cpp", "rviews_unenc_ok"},
        {"unlink_unenc.c", "unlink_unenc_ok"},
        {"strinit_unenc.c", "strinit_unenc_ok"},
        {"pmtx_unenc.c", "pmtx_unenc_ok"},
        {"cmtx_unenc.c", "cmtx_unenc_ok"},
        {"utypedef_unenc.c", "utypedef_unenc_ok"},
        {"memcpy_unenc.c", "memcpy_unenc_ok"},
        {"memmove_unenc.c", "memmove_unenc_ok"},
        {"mkstemp_unenc.c", "mkstemp_unenc_ok"},
        {"chroot_unenc.c", "chroot_unenc_ok"},
        {"popen_unenc.c", "popen_unenc_ok"},
        {"umask_unenc.c", "umask_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover27 sibling unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"anycast_lt_unenc.cpp", "anycast_lt_unenc_bad", "std::any"},
        {"anycast_id_unenc.cpp", "anycast_id_unenc_bad", "std::any"},
        {"stdfs_unenc.cpp", "stdfs_unenc_bad", "std::filesystem"},
        {"filesystem_ns_unenc.cpp", "filesystem_ns_unenc_bad", "std::filesystem"},
        {"regex_match_unenc.cpp", "regex_match_unenc_bad", "std::regex"},
        {"regex_var_unenc.cpp", "regex_var_unenc_bad", "std::regex"},
        {"indirect_lt_unenc.cpp", "indirect_lt_unenc_bad", "indirect"},
        {"polymorphic_lt_unenc.cpp", "polymorphic_lt_unenc_bad", "indirect"},
        {"int_const_unenc.c", "int_const_unenc_bad", "const local"},
        {"for_const_unenc.c", "for_const_unenc_bad", "const local"},
        {"std_bit_cast_unenc.cpp", "std_bit_cast_unenc_bad", "bit_cast"},
        {"function_lt_unenc.cpp", "function_lt_unenc_bad", "std::function"},
        {"std_mdspan_unenc.cpp", "std_mdspan_unenc_bad", "std::mdspan"},
        {"atomic_ref_lt_unenc.cpp", "atomic_ref_lt_unenc_bad", "std::atomic_ref"},
        {"generator_lt_unenc.cpp", "generator_lt_unenc_bad", "std::generator"},
        {"from_chars_bare_unenc.cpp", "from_chars_bare_unenc_bad", "from_chars"},
        {"flat_map_lt_unenc.cpp", "flat_map_lt_unenc_bad", "flat_map"},
        {"flat_set_lt_unenc.cpp", "flat_set_lt_unenc_bad", "flat_set"},
        {"flat_mset_lt_unenc.cpp", "flat_mset_lt_unenc_bad", "flat_multiset"},
        {"flat_mmap_lt_unenc.cpp", "flat_mmap_lt_unenc_bad", "flat_multimap"},
        {"chrono_ns_unenc.cpp", "chrono_ns_unenc_bad", "chrono"},
        {"ranges_views_std_unenc.cpp", "ranges_views_std_unenc_bad", "ranges views"},
        {"hive_lt_unenc.cpp", "hive_lt_unenc_bad", "hive"},
        {"bitset_lt_unenc.cpp", "bitset_lt_unenc_bad", "bitset"},
        {"linalg_ns_unenc.cpp", "linalg_ns_unenc_bad", "linalg"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover27 sibling unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"anycast_lt_unenc.cpp", "anycast_lt_unenc_ok"},
        {"anycast_id_unenc.cpp", "anycast_id_unenc_ok"},
        {"stdfs_unenc.cpp", "stdfs_unenc_ok"},
        {"filesystem_ns_unenc.cpp", "filesystem_ns_unenc_ok"},
        {"regex_match_unenc.cpp", "regex_match_unenc_ok"},
        {"regex_var_unenc.cpp", "regex_var_unenc_ok"},
        {"indirect_lt_unenc.cpp", "indirect_lt_unenc_ok"},
        {"polymorphic_lt_unenc.cpp", "polymorphic_lt_unenc_ok"},
        {"int_const_unenc.c", "int_const_unenc_ok"},
        {"for_const_unenc.c", "for_const_unenc_ok"},
        {"std_bit_cast_unenc.cpp", "std_bit_cast_unenc_ok"},
        {"function_lt_unenc.cpp", "function_lt_unenc_ok"},
        {"std_mdspan_unenc.cpp", "std_mdspan_unenc_ok"},
        {"atomic_ref_lt_unenc.cpp", "atomic_ref_lt_unenc_ok"},
        {"generator_lt_unenc.cpp", "generator_lt_unenc_ok"},
        {"from_chars_bare_unenc.cpp", "from_chars_bare_unenc_ok"},
        {"flat_map_lt_unenc.cpp", "flat_map_lt_unenc_ok"},
        {"flat_set_lt_unenc.cpp", "flat_set_lt_unenc_ok"},
        {"flat_mset_lt_unenc.cpp", "flat_mset_lt_unenc_ok"},
        {"flat_mmap_lt_unenc.cpp", "flat_mmap_lt_unenc_ok"},
        {"chrono_ns_unenc.cpp", "chrono_ns_unenc_ok"},
        {"ranges_views_std_unenc.cpp", "ranges_views_std_unenc_ok"},
        {"hive_lt_unenc.cpp", "hive_lt_unenc_ok"},
        {"bitset_lt_unenc.cpp", "bitset_lt_unenc_ok"},
        {"linalg_ns_unenc.cpp", "linalg_ns_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover28 extra sibling unenc plants are NEEDS-HARNESS never PROVED") {
    struct Plant { const char* file; const char* name; const char* needle; };
    const Plant plants[] = {
        {"pmr_ns_unenc.cpp", "pmr_ns_unenc_bad", "pmr"},
        {"rcu_obj_unenc.cpp", "rcu_obj_lt_unenc_bad", "rcu"},
        {"rcu_sync_unenc.cpp", "rcu_sync_only_unenc_bad", "rcu"},
        {"reflect_caret_unenc.cpp", "reflect_caret_unenc_bad", "reflection"},
        {"jthread_bare_unenc.cpp", "jthread_bare_unenc_bad", "jthread"},
        {"packaged_lt_unenc.cpp", "packaged_lt_unenc_bad", "packaged_task"},
        {"lock_guard_lt_unenc.cpp", "lock_guard_lt_unenc_bad", "mutex"},
        {"cv_bare_unenc.cpp", "cv_bare_unenc_bad", "condition_variable"},
        {"future_lt_unenc.cpp", "future_lt_unenc_bad", "async"},
        {"latch_ctor_unenc.cpp", "latch_ctor_unenc_bad", "latch"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status == std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::PROVED));
        CHECK(findings[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::BOUNDED));
        CHECK(findings[0].status != std::string(prism::laws::FAILED));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK_FALSE(prism::laws::is_proof(findings[0].status));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find(p.needle) != std::string::npos);
    }
}

TEST_CASE("bmc leftover28 extra sibling unenc ok still not NEEDS-HARNESS") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"pmr_ns_unenc.cpp", "pmr_ns_unenc_ok"},
        {"rcu_obj_unenc.cpp", "rcu_obj_lt_unenc_ok"},
        {"rcu_sync_unenc.cpp", "rcu_sync_only_unenc_ok"},
        {"reflect_caret_unenc.cpp", "reflect_caret_unenc_ok"},
        {"jthread_bare_unenc.cpp", "jthread_bare_unenc_ok"},
        {"packaged_lt_unenc.cpp", "packaged_lt_unenc_ok"},
        {"lock_guard_lt_unenc.cpp", "lock_guard_lt_unenc_ok"},
        {"cv_bare_unenc.cpp", "cv_bare_unenc_ok"},
        {"future_lt_unenc.cpp", "future_lt_unenc_ok"},
        {"latch_ctor_unenc.cpp", "latch_ctor_unenc_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
    }
}

TEST_CASE("bmc leftover27 comment-strip plants still prove") {
    struct Plant { const char* file; const char* name; };
    const Plant plants[] = {
        {"nullptr_comment.c", "nullptr_comment_ok"},
        {"ppack_comment.c", "ppack_comment_ok"},
        {"import_comment.cpp", "import_comment_ok"},
    };
    for (const auto& p : plants) {
        auto fn = load_fn(p.file, p.name);
        auto findings = prism::run_bmc({fn}, 8);
        REQUIRE_FALSE(findings.empty());
        CHECK(findings[0].status != std::string(prism::laws::NEEDS_HARNESS));
        CHECK(findings[0].status != std::string(prism::laws::ERROR));
        CHECK((findings[0].status == std::string(prism::laws::PROVED) ||
               findings[0].status == std::string(prism::laws::PROVED_UNBOUNDED)));
        std::string msg = findings[0].message;
        for (char& c : msg)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        CHECK(msg.find("nullptr") == std::string::npos);
        CHECK(msg.find("pragma pack") == std::string::npos);
        CHECK(msg.find("module import") == std::string::npos);
    }
    auto real_null = load_fn("nullptr_c23.c", "nullptr_unenc_bad");
    auto nfind = prism::run_bmc({real_null}, 8);
    REQUIRE_FALSE(nfind.empty());
    CHECK(nfind[0].status == std::string(prism::laws::NEEDS_HARNESS));
    auto real_pack = load_fn("ppack_unenc.c", "ppack_unenc_bad");
    auto pfind = prism::run_bmc({real_pack}, 8);
    REQUIRE_FALSE(pfind.empty());
    CHECK(pfind[0].status == std::string(prism::laws::NEEDS_HARNESS));
    auto real_imp = load_fn("cxx_import.cpp", "import_unenc_bad");
    auto ifind = prism::run_bmc({real_imp}, 8);
    REQUIRE_FALSE(ifind.empty());
    CHECK(ifind[0].status == std::string(prism::laws::NEEDS_HARNESS));
    auto absfn = load_fn("abs_ok.c", "abs_ok");
    auto absf = prism::run_bmc({absfn}, 8);
    REQUIRE_FALSE(absf.empty());
    CHECK(absf[0].status == std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK_FALSE(prism::laws::is_proof(nfind[0].status));
}
#endif

TEST_CASE("run_bmc never silent-empty") {
#ifdef PRISM_HAS_Z3
    auto fn = load_fn("abs_ok.c", "abs_ok");
    auto recs = prism::run_bmc({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == "bmc");
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(recs[0].status.empty());
#else
    auto recs = prism::run_bmc({}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("z3") != std::string::npos);
#endif
}

TEST_CASE("harness POINTER without requires is NEEDS-HARNESS never silent") {
    auto fn = load_fn("null_branch.c", "null_branch");
    auto recs = prism::run_harness_bmc({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == "harness");
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK(recs[0].status != std::string(prism::laws::ERROR));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("honest requires") != std::string::npos);
    auto h = recs[0].extra.find("harness");
    CHECK((h != recs[0].extra.end() && h->second == "false"));
}

TEST_CASE("fuse POINTER is NEEDS-HARNESS never engine=afl") {
    auto fn = load_fn("wp_ptr.c", "wp_ptr_get");
    auto recs = prism::run_fuse({fn}, {}, testdata_root(), 0.05, 1, false);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NEEDS_HARNESS));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    auto it = recs[0].extra.find("engine");
    CHECK((it == recs[0].extra.end() || it->second != "afl"));
}

TEST_CASE("rapid ACSL-only SCALAR is not a silent skip") {
    auto fn = load_fn("acsl_abs.c", "acsl_abs");
    auto recs = prism::run_rapid({fn}, 8);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].stage == "rapid");
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::PROVED_UNBOUNDED));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

TEST_CASE("parse_ltl_file skips hash and slash-slash") {
    auto fn = load_fn("fsm.c", "fsm_step");
    auto recs = prism::run_ltl({fn}, {testdata_root() / "comments_only.ltl"});
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::PROVED));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("no .ltl spec") != std::string::npos);
    for (auto& r : recs) {
        CHECK(r.status == std::string(prism::laws::NOTRUN));
        CHECK_FALSE(prism::laws::is_proof(r.status));
    }
}

TEST_CASE("failed jsonl does not revive stale report.json functions") {
    auto out = journal_tmpdir("resume_stale_fns");
    prism::StageResult cls;
    cls.name = "classify";
    cls.status = "ok";
    prism::journal_append_stage(out, cls);
    REQUIRE(prism::journal_stages_present(out));

    prism::RunReport stale;
    stale.root = "stale";
    prism::FunctionInfo lie;
    lie.file = "stale.c";
    lie.name = "stale_fn";
    lie.kind = "SCALAR";
    lie.line = 1;
    lie.signature = "int stale_fn(int x)";
    lie.body = "return x;";
    stale.functions = {lie};
    prism::StageResult fake_bmc;
    fake_bmc.name = "bmc";
    fake_bmc.status = "ok";
    stale.stages = {cls, fake_bmc};
    stale.save(out / "report.json");

    prism::Config cfg = prism::default_config();
    cfg.root = testdata_root() / "abs_ok.c";
    cfg.out = out;
    cfg.resume = true;
    cfg.llm = false;
    cfg.skip = {"llm", "execute", "repair", "optional", "fuzz", "sanitize", "pbsd",
                "esbmc", "dafny", "ltl", "rapid", "muttest", "diff", "concolic",
                "harness", "wp", "contracts", "interval", "taint", "thread",
                "lints", "warnings", "cppcheck", "bmc"};
    auto report = prism::run_pipeline(cfg);
    bool resumed_report_json = false;
    for (auto& n : report.notes)
        if (n.find("report.json") != std::string::npos) resumed_report_json = true;
    CHECK_FALSE(resumed_report_json);
    bool saw_stale = false;
    for (auto& fn : report.functions)
        if (fn.name == "stale_fn") saw_stale = true;
    CHECK_FALSE(saw_stale);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("fake Catch2/doctest cppcheck is NOTRUN") {
    auto dir = journal_tmpdir("fake_cppcheck");
#ifdef _WIN32
    auto fake = dir / "fake_cppcheck.cmd";
    std::ofstream(fake) << "@echo [doctest] doctest version is \"2.4.11\"\n"
                           "@echo Catch2 v3.5.0\n"
                           "@echo Unknown option: --timeout\n";
#else
    auto fake = dir / "fake_cppcheck";
    {
        std::ofstream o(fake);
        o << "#!/bin/sh\n"
          << "echo '[doctest] doctest version is \"2.4.11\"'\n"
          << "echo 'Catch2 v3.5.0'\n"
          << "echo 'Unknown option: --timeout'\n";
    }
    std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
#endif
    prism::Config cfg = prism::default_config();
    cfg.tools["cppcheck"] = fake;
    auto recs = prism::run_cppcheck({testdata_root() / "abs_ok.c"}, cfg);
    REQUIRE_FALSE(recs.empty());
    CHECK(recs[0].status == std::string(prism::laws::NOTRUN));
    CHECK(recs[0].status != std::string(prism::laws::CLEAN));
    CHECK(recs[0].status != std::string(prism::laws::UNKNOWN));
    CHECK(recs[0].status != std::string(prism::laws::ERROR));
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(recs[0].message.find("not cppcheck") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("execute_cex LLM half down is NOTRUN never CLEAN") {
    auto fn = load_fn("abs_ok.c", "abs_ok");
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = fn.file;
    fail.function = fn.name;
    fail.line = fn.line;
    fail.counterexample = "x=0";
    prism::Config cfg = prism::default_config();
    cfg.llm = true;
    cfg.gguf = "/nonexistent/prism-missing.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = "http://127.0.0.1:1";
    auto recs = prism::execute_cex({fail}, {fn}, cfg);
    REQUIRE_FALSE(recs.empty());
    bool saw_llm_half = false;
    for (auto& r : recs) {
        CHECK(r.status != std::string(prism::laws::PROVED));
        CHECK_FALSE(prism::laws::is_proof(r.status));
        auto half = r.extra.find("half");
        if (r.status == std::string(prism::laws::NOTRUN) && half != r.extra.end() &&
            half->second == "llm") {
            saw_llm_half = true;
            CHECK(r.status != std::string(prism::laws::CLEAN));
        }
    }
    CHECK(saw_llm_half);
}



// ---- polyglot (src/prism/polyglot.cpp; same contract as prism/polyglot.py) ----

namespace {
struct PolyTree {
    std::filesystem::path dir;
    PolyTree() {
        dir = std::filesystem::temp_directory_path() /
              ("prism_pg_cpp_" + std::to_string(std::rand()) + std::to_string(std::rand()));
        std::filesystem::create_directories(dir / "node_modules");
    }
    void put(const std::string& rel, const std::string& text) {
        auto p = dir / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
    ~PolyTree() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

struct PathGuard {
    std::string was;
    bool had = false;
    explicit PathGuard(const char* value) {
        if (const char* v = std::getenv("PATH")) {
            had = true;
            was = v;
        }
#ifndef _WIN32
        setenv("PATH", value, 1);
#endif
    }
    ~PathGuard() {
#ifndef _WIN32
        if (had) setenv("PATH", was.c_str(), 1);
        else unsetenv("PATH");
#endif
    }
};
}  // namespace

TEST_CASE("polyglot builtin scan finds conflict markers and secrets") {
    PolyTree t;
    t.put("a.c", "<<<<<<< HEAD\nint x;\n=======\nint y;\n>>>>>>> b\n");
    t.put("k.ini", "aws = AKIAABCDEFGHIJKLMNOP\n");  // prism:allow
    t.put("node_modules/skip.js", "<<<<<<< never scanned\n");
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    auto out = prism::run_polyglot(t.dir, cfg);
    std::vector<int> marker_lines;
    bool aws = false;
    for (auto& f : out) {
        CHECK(f.stage == "polyglot");
        if (f.cls == "VCS-CONFLICT-MARKER") {
            CHECK(f.status == prism::laws::FAILED);
            CHECK(f.file == "a.c");
            marker_lines.push_back(f.line.value_or(0));
        }
        if (f.cls == "SECRET-AWS-KEY") aws = true;
        CHECK(f.file.find("node_modules") == std::string::npos);
    }
    CHECK(marker_lines == std::vector<int>{1, 5});
    CHECK(aws);
}

#ifndef _WIN32
TEST_CASE("polyglot missing tools are NOTRUN with install, never CLEAN") {
    PolyTree t;
    t.put("a.py", "x = 1\n");
    t.put("b.sh", "echo hi\n");
    t.put("c.rb", "x = 1\n");
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    std::vector<prism::Finding> out;
    {
        PathGuard empty_path("/nonexistent-prism-path");
        out = prism::run_polyglot(t.dir, cfg);
    }
    std::map<std::string, std::string> by_check;
    for (auto& f : out) {
        auto it = f.extra.find("check");
        if (it == f.extra.end()) continue;
        by_check[it->second] = f.status;
        CHECK(f.status == prism::laws::NOTRUN);
        CHECK(!f.extra.at("install").empty());
    }
    for (auto* g : {"python-syntax", "python-lint", "python-types", "shell-syntax", "shell-lint",
                    "ruby-syntax"})
        CHECK_MESSAGE(by_check.contains(g), g);
}
#endif

TEST_CASE("polyglot stage sits between optional and esbmc") {
    std::vector<std::string> order;
    for (const char* const* s = prism::STAGE_ORDER; *s; ++s) order.emplace_back(*s);
    auto it = std::find(order.begin(), order.end(), "polyglot");
    REQUIRE(it != order.end());
    CHECK(*(it - 1) == "optional");
    CHECK(*(it + 1) == "esbmc");
}

// ---------------------------------------------------------------------------
// Law 9: executing code from the scanned tree requires --allow-exec
// (include/prism/sandbox.hpp; Python twin tests/test_exec_safety.py).
// ---------------------------------------------------------------------------
namespace {

std::string extra_or(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

// CommandLineToArgvW / MSVC CRT rules for the arguments after argv[0].
std::vector<std::string> crt_split(const std::string& cl) {
    std::vector<std::string> out;
    std::size_t i = 0;
    const std::size_t n = cl.size();
    while (i < n) {
        while (i < n && (cl[i] == ' ' || cl[i] == '\t')) ++i;
        if (i >= n) break;
        std::string arg;
        bool inq = false;
        while (i < n) {
            char c = cl[i];
            if ((c == ' ' || c == '\t') && !inq) break;
            if (c == '\\') {
                std::size_t k = 0;
                while (i + k < n && cl[i + k] == '\\') ++k;
                if (i + k < n && cl[i + k] == '"') {
                    arg.append(k / 2, '\\');
                    if (k % 2 == 1) {
                        arg += '"';
                        i += k + 1;
                    } else {
                        i += k;
                    }
                } else {
                    arg.append(k, '\\');
                    i += k;
                }
                continue;
            }
            if (c == '"') {
                if (inq && i + 1 < n && cl[i + 1] == '"') {
                    arg += '"';
                    i += 2;
                    continue;
                }
                inq = !inq;
                ++i;
                continue;
            }
            arg += c;
            ++i;
        }
        out.push_back(arg);
    }
    return out;
}

struct ExecTree {
    std::filesystem::path dir;
    ExecTree() {
        dir = std::filesystem::temp_directory_path() /
              ("prism_exec_" + std::to_string(std::rand()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(dir);
    }
    std::filesystem::path put(const std::string& rel, const std::string& text) {
        auto p = dir / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
    ~ExecTree() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

bool has_exec_notrun(const std::vector<prism::Finding>& out) {
    for (auto& f : out)
        if (f.status == prism::laws::NOTRUN && extra_or(f, "reason") == prism::sandbox::EXEC_REASON &&
            f.message.find("--allow-exec") != std::string::npos && !extra_or(f, "install").empty())
            return true;
    return false;
}

}  // namespace

TEST_CASE("windows quoting: CommandLineToArgvW round trip on hostile names") {
    using prism::sandbox::quote_windows_arg;
    CHECK(quote_windows_arg("abc") == "abc");
    CHECK(quote_windows_arg("") == "\"\"");
    CHECK(quote_windows_arg("a b") == "\"a b\"");
    CHECK(quote_windows_arg("a\"b") == "\"a\\\"b\"");
    CHECK(quote_windows_arg("x", true) == "\"x\"");
    CHECK(quote_windows_arg("C:\\my dir\\") == "\"C:\\my dir\\\\\"");
    CHECK(quote_windows_arg("a\\\"b") == "\"a\\\\\\\"b\"");
    CHECK(quote_windows_arg("a\\b c") == "\"a\\b c\"");
    // No cmd.exe in the CreateProcessW path: metacharacters need no quoting.
    CHECK(quote_windows_arg("a&calc&.c") == "a&calc&.c");
    CHECK(prism::sandbox::windows_command_line({"C:\\Program Files\\t.exe", "a b", "c"}) ==
          "\"C:\\Program Files\\t.exe\" \"a b\" c");
    const std::vector<std::string> hostile{
        "a&calc&.c", "x|y", "a^b", "%PATH%.c", "!x!", "<in>", "a\"&calc&\"b", "trail\\",
        "sp ace\\", "\\\\server\\share", "a\\\\\"b", "(x)", "", " ", "\t", "\"", "\\\"",
        "semi;colon", "new\nline"};
    for (const auto& h : hostile) {
        std::vector<std::string> args{"prog.exe", h, "tail"};
        auto split = crt_split(prism::sandbox::windows_command_line(args));
        REQUIRE(split.size() == 3);
        CHECK_MESSAGE(split[1] == h, h);
        CHECK(split[2] == "tail");
    }
}

TEST_CASE("windows quoting: batch targets get cmd.exe quoting or are refused") {
    using prism::sandbox::batch_command_line;
    using prism::sandbox::is_batch_file;
    CHECK(is_batch_file("eslint.cmd"));
    CHECK(is_batch_file("C:\\x\\RUN.BAT"));
    CHECK_FALSE(is_batch_file("prog.exe"));
    CHECK_FALSE(is_batch_file("cmd"));
    auto ok = batch_command_line({"eslint.cmd", "a&calc&.js", "x|y.js"});
    REQUIRE(ok.has_value());
    CHECK(*ok == "cmd.exe /d /s /c \"\"eslint.cmd\" \"a&calc&.js\" \"x|y.js\"\"");
    CHECK_FALSE(batch_command_line({"eslint.cmd", "%PATH%.js"}).has_value());
    CHECK_FALSE(batch_command_line({"eslint.cmd", "!x!.js"}).has_value());
    CHECK_FALSE(batch_command_line({"eslint.cmd", "a\"b.js"}).has_value());
    CHECK_FALSE(batch_command_line({"eslint.cmd", "a\nb.js"}).has_value());
}

TEST_CASE("sandbox: exec NOTRUN row, policy default deny, bwrap argv") {
    CHECK(prism::sandbox::exec_message("fuzz") ==
          "fuzz: executes code from the scanned tree; re-run with --allow-exec (only on code you trust)");
    auto f = prism::sandbox::exec_notrun("sanitize", "sanitize (ASan/UBSan/TSan runs)", {{"tool", "x"}});
    CHECK(f.stage == "sanitize");
    CHECK(f.status == prism::laws::NOTRUN);
    CHECK(extra_or(f, "install") == prism::sandbox::EXEC_INSTALL);
    CHECK(extra_or(f, "reason") == prism::sandbox::EXEC_REASON);
    CHECK(extra_or(f, "tool") == "x");
    CHECK_FALSE(prism::laws::is_proof(f.status));

    CHECK_FALSE(prism::sandbox::allowed());
    {
        prism::sandbox::Policy p(true);
        CHECK(prism::sandbox::allowed());
    }
    CHECK_FALSE(prism::sandbox::allowed());

    auto argv = prism::sandbox::bwrap_argv("/usr/bin/bwrap", {"/s/a.out", "x"}, "/s");
    const std::vector<std::string> want{
        "/usr/bin/bwrap", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs",
        "/tmp", "--bind", "/s", "/s", "--unshare-all", "--die-with-parent", "--", "/s/a.out", "x"};
    CHECK(argv == want);
    auto k = prism::sandbox::kind();
    CHECK((k == "bwrap" || k == "rlimits-only" || k == "none"));
    auto l = prism::sandbox::limits_for(1.0, false);
    CHECK(l.enabled);
    CHECK_FALSE(l.limit_as);
}

TEST_CASE("pipeline: exec stage table and the stage-level --allow-exec row") {
    const auto& t = prism::exec_stages();
    CHECK(t.at("sanitize") == "whole");
    CHECK(t.at("diff") == "whole");
    CHECK(t.at("repair") == "whole");
    for (auto* s : {"optional", "polyglot", "fuzz", "rapid", "muttest", "execute"})
        CHECK_MESSAGE(t.at(s) == "part", s);
    for (auto* s : {"lints", "bmc", "warnings", "concolic", "contracts", "cppcheck", "ltl"})
        CHECK_MESSAGE(!t.contains(s), s);
    prism::Finding held;
    held.stage = "fuzz";
    held.status = prism::laws::CLEAN;
    held.extra["exec"] = prism::laws::NOTRUN;
    auto out = prism::exec_gate_note("fuzz", {held}, "fuzz (compiled harness, AFL++, libFuzzer)");
    CHECK(out.size() == 2);
    CHECK(has_exec_notrun(out));
    CHECK(prism::exec_gate_note("fuzz", out, "fuzz").size() == 2);  // once
    prism::Finding plain;
    plain.status = prism::laws::CLEAN;
    CHECK(prism::exec_gate_note("fuzz", {plain}, "fuzz").size() == 1);
}

TEST_CASE("sanitize: NOTRUN without --allow-exec; never calls an arbitrary void(void)") {
    ExecTree t;
    auto sentinel = t.dir / "SENTINEL";
    auto hostile = t.put("hostile.c",
                         "#include <stdio.h>\n"
                         "void cleanup_everything(void) {\n"
                         "    FILE *f = fopen(\"" + sentinel.generic_string() + "\", \"w\");\n"
                         "    if (f) fclose(f);\n"
                         "}\n");
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    auto out = prism::run_sanitize({hostile}, cfg);
    REQUIRE(out.size() == 1);
    CHECK(has_exec_notrun(out));
    CHECK_FALSE(std::filesystem::exists(sentinel));

    // Even with --allow-exec, an unmarked void(void) is not a target.
    CHECK_FALSE(prism::opted_in_callable(hostile).has_value());
    cfg.allow_exec = true;
    out = prism::run_sanitize({hostile}, cfg);
    for (auto& f : out) CHECK(f.status != prism::laws::CLEAN);
    CHECK_FALSE(std::filesystem::exists(sentinel));
}

TEST_CASE("sanitize: `// prism: run` is the only opt-in") {
    ExecTree t;
    auto marked = t.put("m.c",
                        "static int helper(void) { return 0; }\n"
                        "void cleanup_everything(void) { }\n"
                        "\n"
                        "/* entry point for the sanitizer run\n"
                        " * prism: run */\n"
                        "int\n"
                        "selftest(void)\n"
                        "{\n"
                        "    return helper();\n"
                        "}\n"
                        "void trailing(void) { } // prism: run\n");
    auto fn = prism::opted_in_callable(marked);
    REQUIRE(fn.has_value());
    CHECK(*fn == "selftest");
    auto st = t.put("s.c", "// prism: run\nstatic void only_static(void) { }\n");
    CHECK_FALSE(prism::opted_in_callable(st).has_value());
    auto par = t.put("p.c", "// prism: run\nint takes(int x) { return x; }\n");
    CHECK_FALSE(prism::opted_in_callable(par).has_value());
    std::vector<std::string> lines{"// prism: run", "", "void gap(void) {}"};
    CHECK_FALSE(prism::marked_run(lines, 3));  // a blank line breaks "directly above"
    CHECK(prism::marked_run({"// prism:run", "void f(void) {}"}, 2));
    CHECK(prism::marked_run({"void f(void) { } /* prism: run */"}, 1));
    CHECK_FALSE(prism::marked_run({"// prism: running", "void f(void) {}"}, 2));
}

#ifndef _WIN32
TEST_CASE("polyglot: perl -c / cargo clippy / eslint are NOTRUN without --allow-exec") {
    auto cfg = prism::default_config();
    if (!cfg.which({"perl"})) return;
    ExecTree t;
    auto sentinel = t.dir / "PERL_SENTINEL";
    t.put("evil.pl", "BEGIN { open(my $f, '>', '" + sentinel.generic_string() +
                         "'); close($f); }\nprint 1;\n");
    cfg.root = t.dir;
    auto out = prism::run_polyglot(t.dir, cfg);
    bool held = false;
    for (auto& f : out)
        if (extra_or(f, "check") == "perl-syntax") {
            CHECK(f.status == prism::laws::NOTRUN);
            CHECK(extra_or(f, "reason") == prism::sandbox::EXEC_REASON);
            CHECK(f.message == "perl-syntax (perl): executes code from the scanned tree; "
                               "re-run with --allow-exec (only on code you trust)");
            held = true;
        }
    CHECK(held);
    CHECK_FALSE(std::filesystem::exists(sentinel));
}

TEST_CASE("pipeline: hostile tree runs nothing without --allow-exec") {
    ExecTree t;
    auto sentinel = t.dir / "PIPE_SENTINEL";
    const auto s = sentinel.generic_string();
    t.put("hostile.c",
          "#include <stdio.h>\n"
          "void cleanup_everything(void) { FILE *f = fopen(\"" + s + "\", \"w\"); if (f) fclose(f); }\n"
          "int touch(int x) { FILE *f = fopen(\"" + s + "\", \"w\"); if (f) fclose(f); return x; }\n"
          "int pick_a(int x) { cleanup_everything(); return x; }\n"
          "int pick_b(int x) { return x + 1; }\n");
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    cfg.out = t.dir / "prism-out";
    cfg.llm = false;
    cfg.fuzz_budget = 0.5;
    cfg.fuzz_iters = 16;
    cfg.stages = std::vector<std::string>{"inventory", "classify", "sanitize", "fuzz", "diff"};
    auto report = prism::run_pipeline(cfg);
    CHECK_FALSE(prism::sandbox::allowed());  // policy restored after the run
    std::map<std::string, const prism::StageResult*> by;
    for (auto& st : report.stages) by[st.name] = &st;
    REQUIRE(by.contains("sanitize"));
    CHECK(by["sanitize"]->status == "NOTRUN");
    CHECK(has_exec_notrun(by["sanitize"]->findings));
    REQUIRE(by.contains("fuzz"));
    CHECK(has_exec_notrun(by["fuzz"]->findings));
    REQUIRE(by.contains("diff"));
    CHECK(by["diff"]->status == "NOTRUN");
    CHECK(has_exec_notrun(by["diff"]->findings));
    CHECK_FALSE(std::filesystem::exists(sentinel));
}
#endif

TEST_CASE("config: vendored adapters are never taken from inside the scanned tree") {
    ExecTree t;
    CHECK(prism::path_within(t.dir, t.dir));
    CHECK(prism::path_within(t.dir / "third_party" / "esbmc", t.dir));
    CHECK_FALSE(prism::path_within(t.dir.parent_path(), t.dir));
    CHECK_FALSE(prism::path_within(t.dir.string() + "-sibling", t.dir));
#ifndef _WIN32
    t.put("third_party/SOURCES.md", "x\n");
    auto fake = t.put("third_party/esbmc/bin/esbmc-planted-by-test", "#!/bin/sh\nexit 0\n");
    std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
    auto cfg = prism::default_config();
    cfg.root = t.dir;
    // cwd inside the hostile tree: the planted third_party/ is still refused.
    auto was = std::filesystem::current_path();
    std::filesystem::current_path(t.dir);
    auto hit = cfg.which_adapter("esbmc", {"esbmc-planted-by-test"});
    cfg.allow_exec = true;
    auto trusted = cfg.which_adapter("esbmc", {"esbmc-planted-by-test"});
    std::filesystem::current_path(was);
    CHECK_FALSE(hit.has_value());
    REQUIRE(trusted.has_value());
    CHECK(trusted->filename() == "esbmc-planted-by-test");
#endif
}
