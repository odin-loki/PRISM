// Portable prism-qa doctests (no fork/session/process groups). POSIX-only cases stay in
// tests/cpp/test_qa.cpp.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "../../src/tools/qa/qa.hpp"

#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/shipdocs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return prism::qa::repo_root(); }

struct Tmp {
    fs::path path;
    explicit Tmp(const std::string& prefix) {
        path = fs::temp_directory_path() /
               (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

bool has_line(const std::string& text, const std::string& line) {
    auto ls = lines_of(text);
    return std::find(ls.begin(), ls.end(), line) != ls.end();
}

}  // namespace

TEST_CASE("triage-selfscan buckets the self-scan fixture") {
    auto sarif = prism::qa::load_json(repo() / "tests" / "data" / "selfscan_min" / "report.sarif");
    REQUIRE(sarif);
    const auto text = prism::qa::triage_text(prism::qa::triage_selfscan(*sarif));
    CHECK(has_line(text, "total FAILED: 4"));
    CHECK(has_line(text, "third_party (out of scope): 1"));
    CHECK(has_line(text, "testdata/tests (false alarm corpus): 1"));
    CHECK(has_line(text, "src/prism|gui (manual review): 1"));
    CHECK(has_line(text, ".github (false alarm LANG-LINT): 1"));
    CHECK(has_line(text, "summary: 1 / 3 / 1"));

    CHECK(prism::qa::triage_classify("third_party/z3/a.c", "FAILED", "lints") == "out_of_scope");
    CHECK(prism::qa::triage_classify("x/third_party/a.c", "FAILED", "lints") == "out_of_scope");
    CHECK(prism::qa::triage_classify("testdata_tp/a.c", "FAILED", "lints") == "false_alarm");
    CHECK(prism::qa::triage_classify("tests/a.py", "FAILED", "polyglot") == "false_alarm");
    CHECK(prism::qa::triage_classify("src/gui/a.cpp", "FAILED", "lints") == "false_alarm");
    CHECK(prism::qa::triage_classify("src/prism/a.cpp", "CRASH", "sanitize") == "real");
    CHECK(prism::qa::triage_classify("src/prism/a.cpp", "CRASH", "fuzz") == "false_alarm");
    CHECK(prism::qa::triage_classify("proofs/x.c", "FAILED", "lints") == "real");

    nlohmann::json j = {{"runs",
                         {{{"results",
                            {{{"locations", {{{"physicalLocation", {{"artifactLocation", {{"uri", ".\\third_party\\a.c"}}}}}}}},
                              {"properties", {{"status", "FAILED"}, {"stage", "lints"}}}}}}}}}};
    auto s = prism::qa::triage_selfscan(j);
    CHECK(s.third_party == 1);
    j["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] = "./third_party/a.c";
    s = prism::qa::triage_selfscan(j);
    CHECK(s.third_party == 1);
    CHECK(s.out_of_scope == 1);
}

TEST_CASE("pir-vs-bmc buckets, matrix and hard conflicts") {
    using prism::qa::pvb_bucket;
    const std::string proved = "PROVED", cert = "PROVED-CERTIFIED", unb = "PROVED-UNBOUNDED", ass = "PROVED-ASSUMING",
                      bounded = "BOUNDED", timeout = "TIMEOUT", notrun = "NOTRUN", nh = "NEEDS-HARNESS";
    CHECK(pvb_bucket(nullptr) == "absent");
    for (auto* s : {&proved, &cert, &unb, &ass}) CHECK(pvb_bucket(s) == "PROVED*");
    CHECK(pvb_bucket(&bounded) == "BOUNDED");
    CHECK(pvb_bucket(&nh) == "NEEDS-HARNESS");
    CHECK(pvb_bucket(&timeout) == "UNKNOWN");
    CHECK(pvb_bucket(&notrun) == "UNKNOWN");

    auto f = [](const char* file, const char* fn, const char* st) {
        return nlohmann::json{{"file", file}, {"function", fn}, {"status", st}, {"message", "m"}};
    };
    nlohmann::json report = {
        {"stages",
         {{{"name", "bmc"},
           {"findings",
            {f("a.c", "f", "PROVED"), f("a.c", "f", "FAILED"), f("a.c", "g", "FAILED"), f("a.c", "h", "BOUNDED"),
             f("a.c", "only_bmc", "UNKNOWN"), nlohmann::json{{"file", "a.c"}, {"status", "FAILED"}}}}},
          {{"name", "pir"},
           {"findings",
            {f("a.c", "f", "PROVED-CERTIFIED"), f("a.c", "g", "PROVED"), f("a.c", "h", "PROVED"),
             f("b.c", "only_pir", "FAILED")}}}}}};
    auto r = prism::qa::pir_vs_bmc(report);
    CHECK(r.functions == 5);
    CHECK(r.bmc == 4);
    CHECK(r.pir == 4);
    CHECK(r.matrix[{"PROVED*", "PROVED*"}] == 1);
    CHECK(r.matrix[{"FAILED", "PROVED*"}] == 1);
    CHECK(r.matrix[{"BOUNDED", "PROVED*"}] == 1);
    CHECK(r.matrix[{"UNKNOWN", "absent"}] == 1);
    CHECK(r.matrix[{"absent", "FAILED"}] == 1);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0]["function"] == "g");
    CHECK(r.conflicts[0]["bmc"] == "FAILED");
    CHECK(r.conflicts[0]["pir"] == "PROVED");
    const auto text = prism::qa::pir_vs_bmc_text(r);
    CHECK(has_line(text, "functions: 5 (bmc 4, pir 4)"));
    CHECK(has_line(text, "rows = bmc, columns = pir"));
    CHECK(has_line(text, "same bucket where both report: 1/3"));
    CHECK(has_line(text, "hard conflicts (PROVED* vs FAILED): 1"));
    CHECK(has_line(text, "  a.c::g: bmc=FAILED [m]  | pir=PROVED [m] "));
    CHECK(has_line(text, std::string(15, ' ') + "        PROVED*        BOUNDED         FAILED  NEEDS-HARNESS"
                                                "          ERROR        UNKNOWN         absent"));
    CHECK(has_line(text, "FAILED                       1              0              0              0"
                         "              0              0              0"));
    auto j = prism::qa::pir_vs_bmc_json(r);
    CHECK(j["matrix"]["FAILED|PROVED*"] == 1);
    CHECK(j["conflicts"].size() == 1);
}

TEST_CASE("pir-lean-check parses the checker's rows and prints the total line") {
    auto p = prism::qa::parse_checker_output(
        "agree\tf\tx\n"
        "agree-ext\tg\tx\n"
        "outside\th\tx\tcall @foo not modelled\n"
        "outside\ti\tx\tcall @bar not modelled\n"
        "MISMATCH\tj\tfile.pirl\tblock 2\n"
        "agree\tshort\n"
        "summary: whatever\n");
    CHECK(p.counts["agree"] == 1);
    CHECK(p.counts["agree-ext"] == 1);
    CHECK(p.counts["outside"] == 2);
    CHECK(p.counts["MISMATCH"] == 1);
    CHECK(p.rows.size() == 5);
    const auto line = prism::qa::lean_total_line(p.counts);
    CHECK(line == "total: agree=1 agree-ext=1 agree-reject=0 outside=2 mismatch=1 "
                  "(functions in the proved fragment: 3)");
    std::smatch m;
    REQUIRE(std::regex_search(line, m, std::regex(R"(total: agree=(\d+) .* mismatch=(\d+))")));
    CHECK(m[1] == "1");
    CHECK(m[2] == "1");
    CHECK(std::regex_search(line, std::regex(R"(total: agree=\d+ agree-ext=(\d+))")));
}

TEST_CASE("llvm-sem-vs-lli reads .pirl records, builds input vectors and lli harnesses") {
    auto recs = prism::qa::pirl_records(
        "func add\n"
        "L params 2 x 32 y 32\n"
        "L ret 32\n"
        "L block entry\n"
        "L ret 8\n"
        "end\n"
        "func bad\n"
        "L params 1 p 64\n"
        "L unsupported call @printf\n"
        "end\n"
        "func v\n"
        "L params 0\n"
        "end\n");
    REQUIRE(recs.size() == 2);
    CHECK(recs[0].name == "add");
    CHECK(recs[0].params == std::vector<int>{32, 32});
    CHECK(recs[0].ret == 32);
    CHECK(recs[1].name == "v");
    CHECK(recs[1].params.empty());
    CHECK(recs[1].ret == 0);

    std::mt19937_64 rng(20260923);
    auto vs = prism::qa::input_vectors({8, 8}, 12, rng);
    CHECK(vs.size() == 12);
    CHECK(std::is_sorted(vs.begin(), vs.end()));
    std::set<std::uint64_t> seen;
    for (auto& v : vs) {
        REQUIRE(v.size() == 2);
        CHECK(v[0] <= 255);
        CHECK(v[1] <= 255);
    }
    CHECK(std::set<std::vector<std::uint64_t>>(vs.begin(), vs.end()).size() == vs.size());
    std::mt19937_64 again(20260923);
    CHECK(prism::qa::input_vectors({8, 8}, 12, again) == vs);
    std::mt19937_64 r1(7);
    for (auto& v : prism::qa::input_vectors({32}, 40, r1)) seen.insert(v[0]);
    for (std::uint64_t edge : {0ull, 1ull, 2ull, 7ull, 100ull, 0xFFFFFFFFull, 0xFFFFFFFEull, 0x80000000ull, 0x7FFFFFFFull})
        CHECK(seen.contains(edge));
    std::mt19937_64 r2(1);
    CHECK(prism::qa::input_vectors({}, 12, r2) == std::vector<std::vector<std::uint64_t>>{{}});
    std::mt19937_64 r3(1);
    auto one = prism::qa::input_vectors({1}, 12, r3);
    CHECK(one.size() == 2);

    const auto h = prism::qa::lli_harness("define i32 @add(i32 %x, i32 %y) {\n  ret i32 0\n}\n", "add", {32, 32}, 32,
                                          {4294967295ull, 2});
    CHECK(h.find("%r = call i32 @add(i32 4294967295, i32 2)") != std::string::npos);
    CHECK(h.find("%z = zext i32 %r to i64") != std::string::npos);
    CHECK(h.find("declare i32 @printf(ptr, ...)") != std::string::npos);
    const auto hv = prism::qa::lli_harness("declare i32 @printf(ptr, ...)\n", "g", {}, 0, {});
    CHECK(hv.find("call void @g()") != std::string::npos);
    CHECK(hv.find("@__prism_lli_void") != std::string::npos);
    CHECK(hv.find("declare i32 @printf(ptr, ...)\n@") == std::string::npos);
    CHECK(prism::qa::lli_harness("", "w", {64}, 64, {1}).find("%z = add i64 %r, 0") != std::string::npos);
    const auto hm = prism::qa::lli_harness(
        "declare i8 @__prism.uninit.i8()\ndeclare void @__prism.folded(i32)\ndeclare i32 @ext(i32)\n", "f", {}, 0, {});
    CHECK(hm.find("define i8 @__prism.uninit.i8() {\n  ret i8 zeroinitializer\n}") != std::string::npos);
    CHECK(hm.find("define void @__prism.folded(i32) {\n  ret void\n}") != std::string::npos);
    CHECK(hm.find("declare i32 @ext(i32)") != std::string::npos);
}

TEST_CASE("libc-bounds splits a contract harness file into prelude and harnesses") {
    const std::string text =
        "#include \"harness.h\"\n"
        "#include \"../x/y.h\"\n"
        "\n"
        "/* strlen: returns the length.\n"
        " * second line\n"
        " */\n"
        "int strlen_true(void) {\n"
        "    char s[N];\n"
        "    return 0;\n"
        "}\n"
        "static int sign(int v) { return v < 0 ? -1 : v > 0; }\n"
        "// one line\n"
        "int one_true(void) { return 1; }\n"
        "int fixed_false(void) {\n"
        "    return 0;\n"
        "}\n";
    auto s = prism::qa::split_harness(text);
    CHECK(s.prelude ==
          "#include \"harness.h\"\n#include \"../x/y.h\"\n\n"
          "static int sign(int v) { return v < 0 ? -1 : v > 0; }\n");
    REQUIRE(s.funcs.size() == 3);
    CHECK(s.funcs[0].first == "strlen_true");
    CHECK(s.funcs[0].second ==
          "/* strlen: returns the length.\n * second line\n */\n"
          "int strlen_true(void) {\n    char s[N];\n    return 0;\n}\n");
    CHECK(s.funcs[1].first == "one_true");
    CHECK(s.funcs[1].second == "// one line\nint one_true(void) { return 1; }\n");
    CHECK(s.funcs[2].first == "fixed_false");
    auto none = prism::qa::split_harness("int x;\n");
    CHECK(none.prelude == "int x;\n");
    CHECK(none.funcs.empty());

    const auto suite = repo() / "tests" / "conformance" / "libc-models";
    auto abs = prism::qa::absolutize_includes("#include \"harness.h\"\n#include <string.h>\n", suite);
    CHECK(abs == "#include \"" + fs::weakly_canonical(suite / "harness.h").string() + "\"\n#include <string.h>\n");

    int files = 0;
    for (auto& e : fs::directory_iterator(suite)) {
        if (!e.path().filename().string().ends_with("_contracts.c")) continue;
        ++files;
        auto sp = prism::qa::split_harness(prism::qa::read_text(e.path()));
        CHECK_MESSAGE(std::any_of(sp.funcs.begin(), sp.funcs.end(), [](auto& p) { return p.first.ends_with("_true"); }),
                      e.path().filename().string());
        CHECK(sp.prelude.find("#include \"harness.h\"") != std::string::npos);
    }
    CHECK(files > 0);
}

TEST_CASE("solver-bench discovers the conformance tasks it can score") {
    Tmp t("prism_qa_tasks_");
    const auto suite = t.path / "suite";
    fs::create_directories(suite / "prism" / "cat");
    fs::create_directories(suite / "sv-comp" / "bits");
    std::ofstream(suite / "prism" / "cat" / "a.c") << "int f(int x) { return x; }\n";
    std::ofstream(suite / "prism" / "cat" / "a.yml")
        << "format_version: 1\ninput_files: a.c\nlanguage: C\nproperty: no-oob\nexpected:\n  f: true\n";
    std::ofstream(suite / "prism" / "cat" / "nosrc.yml") << "input_files: missing.c\nexpected:\n  f: true\n";
    std::ofstream(suite / "prism" / "cat" / "noexp.yml") << "input_files: a.c\n";
    std::ofstream(suite / "sv-comp" / "bits" / "b.i") << "int main(void) { return 0; }\n";
    std::ofstream(suite / "sv-comp" / "bits" / "b.yml")
        << "format_version: '2.0'\n\n# comment\ninput_files: 'b.i'\n\nproperties:\n"
           "  - property_file: ../properties/unreach-call.prp\n    expected_verdict: false\n"
           "  - property_file: ../properties/no-overflow.prp\n    expected_verdict: true\n"
           "\noptions:\n  language: C\n  data_model: ILP32\n";
    std::ofstream(suite / "sv-comp" / "bits" / "c.yml")
        << "input_files: ['b.i']\nproperties:\n  - property_file: ../properties/termination.prp\n"
           "    expected_verdict: true\n  - property_file: ../properties/no-overflow.prp\n";
    auto tasks = prism::qa::discover_tasks({suite / "prism", suite / "sv-comp"}, suite);
    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].ident == "prism/cat/a.yml");
    CHECK(tasks[0].source.filename() == "a.c");
    CHECK(tasks[1].ident == "sv-comp/bits/b.yml");
    CHECK(tasks[1].source.filename() == "b.i");
    auto outside = prism::qa::discover_tasks({suite / "prism" / "cat"}, t.path / "elsewhere");
    REQUIRE(outside.size() == 1);
    CHECK(outside[0].ident == "cat/a.yml");

    const auto real = repo() / "tests" / "conformance";
    auto all = prism::qa::discover_tasks({real / "prism", real / "sv-comp"}, real);
    CHECK(all.size() > 100);
    for (auto& task : all) CHECK(fs::exists(task.source));
}

TEST_CASE("assurance-check: the assurance package cites only existing artefacts") {
    auto r = prism::qa::assurance_scan(repo() / "docs" / "assurance", repo());
    std::string joined;
    for (auto& e : r.errors) joined += e + "\n";
    CHECK_MESSAGE(r.errors.empty(), joined);
    CHECK(r.counts.files >= 7);
    CHECK(r.counts.theorems > 20);
}

TEST_CASE("assurance-check catches missing artefacts") {
    Tmp t("prism_qa_assurance_");
    std::ofstream(t.path / "bogus.md")
        << "`thm:no_such_theorem_anywhere` `thm:proved_bounded_never_merge` "
           "`thm:Houdini.houdini_sound` `thm:Houdini.bmc_sound`\n"
           "`docs/NO_SUCH.md` `docs/VERDICTS.md#no-such-anchor` `docs/VERDICTS.md#verdict-proved`\n"
           "`tests/cpp/test_main.cpp::NoSuchTestCase` `tests/cpp/test_main.cpp::laws refuse merging proofs`\n"
           "`thm:<Name>` is a placeholder\n";
    auto r = prism::qa::assurance_scan(t.path, repo());
    std::string joined;
    for (auto& e : r.errors) joined += e + "\n";
    CHECK_MESSAGE(r.errors.size() == 5, joined);
    CHECK(joined.find("no_such_theorem_anywhere") != std::string::npos);
    CHECK(joined.find("not in namespace Houdini") != std::string::npos);
    CHECK(joined.find("docs/NO_SUCH.md") != std::string::npos);
    CHECK(joined.find("#no-such-anchor") != std::string::npos);
    CHECK(joined.find("NoSuchTestCase") != std::string::npos);
    CHECK(r.counts.theorems == 4);
    CHECK(r.counts.paths == 5);
    CHECK(r.errors[0].find("bogus.md:1: ") != std::string::npos);

    CHECK(prism::qa::is_path_ref("docs/X.md#a"));
    CHECK(prism::qa::is_path_ref("CMakeLists.txt"));
    CHECK_FALSE(prism::qa::is_path_ref("docs/*.md"));
    CHECK_FALSE(prism::qa::is_path_ref("prism PATH"));
    CHECK_FALSE(prism::qa::is_path_ref("build/prism"));
}

TEST_CASE("docs: slugs are GitHub's heading anchors") {
    using prism::qa::slug;
    CHECK(slug("Pipeline order is the method") == "pipeline-order-is-the-method");
    CHECK(slug("Running on untrusted code (Law 9)") == "running-on-untrusted-code-law-9");
    CHECK(slug("5. Exit codes and `--fail-on`") == "5-exit-codes-and---fail-on");
    CHECK(slug("See [the plan](PLAN.md) now") == "see-the-plan-now");
    CHECK(slug("Ünïcode — names × 2") == "ünïcode--names--2");
    auto a = prism::qa::anchors_of_text("# A\n## A\n```\n# not a heading\n```\n<a id=\"x-y\"></a>\n### B ##\n");
    CHECK(a == std::set<std::string>{"a", "a-1", "b", "x-y"});
}

TEST_CASE("docs: the flag parser ignores prose") {
    CHECK(prism::qa::flags_of("run perl -c on it\n  --jobs JOBS, -j JOBS  workers\n[--out DIR]") ==
          std::set<std::string>{"--jobs", "-j", "--out"});
    CHECK(prism::qa::flags_of("  --allow-\n      exec  run code") == std::set<std::string>{"--allow-exec"});
    CHECK(prism::qa::documented("--jobs", "use `--jobs N` here"));
    CHECK_FALSE(prism::qa::documented("--job", "use `--jobs N` here"));
    CHECK_FALSE(prism::qa::documented("--jobs", "use --jobs N here"));
}

TEST_CASE("docs: the user guide lists the stage order") {
    const auto guide = prism::qa::read_text(repo() / "docs" / "USER_GUIDE.md");
    std::vector<std::string> want;
    for (auto* s : prism::STAGE_ORDER)
        if (s) want.emplace_back(s);
    CHECK(prism::qa::stage_order_listed(guide) == want);
}

TEST_CASE("docs: every Markdown anchor link resolves") {
    auto bad = prism::qa::anchor_errors(repo());
    std::string joined;
    for (auto& b : bad) joined += b + "\n";
    CHECK_MESSAGE(bad.empty(), joined);
}

TEST_CASE("docs: every verdict has the VERDICTS.md anchor reports link to") {
    const auto anchors = prism::qa::anchors_of(repo() / "docs" / "VERDICTS.md");
    using namespace prism::laws;
    for (auto v : {PROVED_CERTIFIED, PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING, BOUNDED, FAILED, UNKNOWN, TIMEOUT,
                   ERROR, NOFUNC, NOTRUN, NEEDS_HARNESS, CRASH, CLEAN, NOSEED, SANFAIL, HYPOTHESIS, READS}) {
        CHECK(is_status(v));
        std::string a = "verdict-";
        for (char c : v) a += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(prism::verdict_anchor(v) == a);
        CHECK_MESSAGE(anchors.contains(a), a);
    }
}
