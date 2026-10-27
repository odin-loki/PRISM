// Doctests for the thread stage (src/prism/stages/thread.cpp).
// RACE-SHARED is a FINDS lint on a file-scope global written from two or
// more functions that do not mention mtx_lock / mtx_timedlock / pthread_mutex,
// and only in a file that starts a thread. No thread API is silence: the
// pipeline stage stays "ok" with zero records, not NOTRUN, NEEDS-HARNESS,
// CLEAN, or a proof. A threaded file whose source cannot be read is NOTRUN
// (Law 7), never a harness request.
//
// tests/cpp/test_thread_taint.cpp already locks race_global.c's writer names,
// the ISO thrd / mtx plants, jthread_race, and the taint stage. Those cases
// are not repeated here.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path planted() { return fs::path(PRISM_SOURCE_DIR) / "testdata" / "planted"; }

struct Tree {
    fs::path base;
    Tree() {
        std::random_device rd;
        base = fs::temp_directory_path() / ("prism_thread_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(src());
    }
    fs::path src() const { return base / "src"; }
    fs::path put(const std::string& name, const std::string& text) const {
        auto p = src() / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
    void copy_plant(const char* name) const { fs::copy_file(planted() / name, src() / name); }
    prism::Config cfg() const {
        auto c = prism::default_config();
        c.root = src();
        c.out = base / "out";
        c.llm = false;
        c.unwind = 2;
        c.stages = std::vector<std::string>{"inventory", "classify", "thread"};
        return c;
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
};

const prism::StageResult& stage_of(const prism::RunReport& r, const std::string& name) {
    for (auto& s : r.stages)
        if (s.name == name) return s;
    FAIL("no stage " << name);
    throw 0;
}

std::vector<prism::Finding> on_file(const fs::path& p) {
    return prism::run_thread(prism::extract_functions(p, p.string()));
}

std::vector<std::string> writers_of(const prism::Finding& f) {
    auto it = f.extra.find("writers");
    REQUIRE(it != f.extra.end());
    auto j = nlohmann::json::parse(it->second);
    REQUIRE(j.is_array());
    return j.get<std::vector<std::string>>();
}

void check_silent(const std::vector<prism::Finding>& hits) {
    std::string why;
    if (!hits.empty()) why = hits[0].status + " " + hits[0].cls + " " + hits[0].message;
    CHECK_MESSAGE(hits.empty(), why);
}

void check_race(const prism::Finding& f, const std::string& global, const std::vector<std::string>& writers) {
    CHECK(f.stage == "thread");
    CHECK(f.cls == "RACE-SHARED");
    CHECK(f.status == prism::laws::FAILED);
    CHECK(f.status != prism::laws::NOTRUN);
    CHECK(f.status != prism::laws::NEEDS_HARNESS);
    CHECK(f.status != prism::laws::CLEAN);
    CHECK(f.status != prism::laws::PROVED);
    CHECK(f.strength == prism::laws::STRENGTH_FINDS);
    CHECK(f.strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.evidence == global);
    auto git = f.extra.find("global");
    REQUIRE(git != f.extra.end());
    CHECK(git->second == global);
    CHECK(f.message.find("global '" + global + "'") != std::string::npos);
    CHECK(f.message.find(std::to_string(writers.size()) + " functions") != std::string::npos);
    CHECK(writers_of(f) == writers);
    REQUIRE_FALSE(writers.empty());
    CHECK(f.function == std::optional<std::string>(writers.front()));
}

}  // namespace

TEST_CASE("thread: an empty function list is silence") {
    check_silent(prism::run_thread({}));
}

TEST_CASE("thread: shared writes with no thread API are silence, not NOTRUN or NEEDS-HARNESS") {
    check_silent(on_file(planted() / "abs_ok.c"));
    Tree t;
    auto p = t.put("shared_quiet.c",
                   "int g;\n"
                   "void a(void) { g = 1; }\n"
                   "void b(void) { g = 2; }\n");
    auto hits = on_file(p);
    check_silent(hits);
    for (auto& f : hits) {
        CHECK(f.status != prism::laws::NOTRUN);
        CHECK(f.status != prism::laws::NEEDS_HARNESS);
        CHECK(f.status != prism::laws::CLEAN);
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }
}

TEST_CASE("thread: race_global.c RACE-SHARED names the global, the writers, and the anchor") {
    auto race_path = planted() / "race_global.c";
    auto race = prism::extract_functions(race_path, race_path.string());
    int anchor_line = -1;
    for (auto& fn : race)
        if (fn.name == "t1") anchor_line = fn.line;
    REQUIRE(anchor_line > 0);

    Tree t;
    auto quiet = t.put("shared_quiet.c",
                       "int g;\n"
                       "void a(void) { g = 1; }\n"
                       "void b(void) { g = 2; }\n");
    auto fns = race;
    for (auto& fn : prism::extract_functions(quiet, quiet.string())) fns.push_back(fn);
    auto hits = prism::run_thread(fns);
    REQUIRE(hits.size() == 1);
    check_race(hits[0], "g", {"t1", "t2"});
    CHECK(hits[0].file == race_path.string());
    CHECK(hits[0].line == anchor_line);
    CHECK(hits[0].cls != "CONC-ATOMICITY");
    CHECK(hits[0].cls != "CONC-DATA-RACE");
}

TEST_CASE("thread: CreateThread writers of static and extern globals are RACE-SHARED") {
    Tree t;
    auto p = t.put("win_race.c",
                   "static int alpha;\n"
                   "extern int beta;\n"
                   "int gamma;\n"
                   "void t1(void) {\n"
                   "    alpha += 1;\n"
                   "    beta = 1;\n"
                   "    gamma[0] = 1;\n"
                   "}\n"
                   "void t2(void) {\n"
                   "    alpha = 2;\n"
                   "    beta += 2;\n"
                   "    gamma[1] = 2;\n"
                   "}\n"
                   "void start(void) { CreateThread(0, 0, 0, 0, 0, 0); }\n");
    auto hits = on_file(p);
    REQUIRE(hits.size() == 3);
    std::vector<std::string> globals;
    for (auto& f : hits) {
        check_race(f, f.evidence, {"t1", "t2"});
        CHECK(f.file == p.string());
        globals.push_back(f.evidence);
    }
    CHECK(globals == std::vector<std::string>{"alpha", "beta", "gamma"});
}

TEST_CASE("thread: std::thread writers are RACE-SHARED; a plant with no shared write is not NEEDS-HARNESS") {
    Tree t;
    auto p = t.put("std_race.cpp",
                   "int g;\n"
                   "void ta(void) { g = 1; }\n"
                   "void tb(void) { g = 2; }\n"
                   "void start(void) {\n"
                   "    std::thread a(ta);\n"
                   "    std::thread b(tb);\n"
                   "}\n");
    auto hits = on_file(p);
    REQUIRE(hits.size() == 1);
    check_race(hits[0], "g", {"ta", "tb"});

    for (const char* stem : {"std_thread.cpp", "sthread_unenc.cpp", "jthread.cpp"}) {
        INFO(stem);
        auto quiet = on_file(planted() / stem);
        check_silent(quiet);
        for (auto& f : quiet) {
            CHECK(f.status != prism::laws::NEEDS_HARNESS);
            CHECK(f.status != prism::laws::NOTRUN);
            CHECK(f.status != prism::laws::CLEAN);
            CHECK_FALSE(prism::laws::is_proof(f.status));
        }
    }
}

TEST_CASE("thread: a thread API only in a comment or a string does not count") {
    Tree t;
    check_silent(on_file(t.put("comment_only.c",
                               "int g;\n"
                               "/* pthread_create(0, 0, 0, 0); */\n"
                               "void a(void) {\n"
                               "    // CreateThread\n"
                               "    g = 1;\n"
                               "}\n"
                               "void b(void) {\n"
                               "    /* std::thread thrd_create */\n"
                               "    g = 2;\n"
                               "}\n")));
    check_silent(on_file(t.put("string_only.c",
                               "int g;\n"
                               "const char *note = \"pthread_create std::thread CreateThread thrd_create\";\n"
                               "void a(void) { g = 1; }\n"
                               "void b(void) { g = 2; }\n")));
}

TEST_CASE("thread: locals and pure reads are not a file-scope race") {
    Tree t;
    check_silent(on_file(t.put("locals.c",
                               "void a(void) {\n"
                               "    int g;\n"
                               "    g = 1;\n"
                               "}\n"
                               "void b(void) {\n"
                               "    int g;\n"
                               "    g = 2;\n"
                               "}\n"
                               "void start(void) { pthread_create(0, 0, 0, 0); }\n")));
    check_silent(on_file(t.put("reads.c",
                               "int g;\n"
                               "void a(void) { int x = g; }\n"
                               "void b(void) { int y = g; }\n"
                               "void start(void) { pthread_create(0, 0, 0, 0); }\n")));
}

TEST_CASE("thread: one mutex-guarded writer leaves a single unsynchronised writer, not a race") {
    Tree t;
    auto hits = on_file(t.put("one_locked.c",
                              "int g;\n"
                              "void a(void) { mtx_lock(0); g = 1; }\n"
                              "void b(void) { g = 2; }\n"
                              "void start(void) { thrd_create(0, 0, 0); }\n"));
    check_silent(hits);
}

TEST_CASE("thread: a missing threaded source is NOTRUN, not NEEDS-HARNESS; a missing quiet source stays silent") {
    prism::FunctionInfo t1;
    t1.file = "no/such/prism_thread_missing_src.c";
    t1.name = "t1";
    t1.line = 4;
    t1.body = "    g = 1;\n";
    prism::FunctionInfo t2 = t1;
    t2.name = "t2";
    t2.line = 5;
    t2.body = "    g = 2;\n";
    prism::FunctionInfo start = t1;
    start.name = "start";
    start.line = 6;
    start.body = "    pthread_create(0, 0, t1, 0);\n";

    prism::FunctionInfo quiet;
    quiet.file = "no/such/prism_thread_quiet_missing.c";
    quiet.name = "only";
    quiet.line = 2;
    quiet.body = "    g = 1;\n";

    auto abs_ok = planted() / "abs_ok.c";
    auto planted_quiet = prism::extract_functions(abs_ok, abs_ok.string());
    REQUIRE_FALSE(planted_quiet.empty());

    std::vector<prism::FunctionInfo> fns{t1, t2, start, quiet};
    fns.insert(fns.end(), planted_quiet.begin(), planted_quiet.end());
    auto hits = prism::run_thread(fns);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].stage == "thread");
    CHECK(hits[0].status == prism::laws::NOTRUN);
    CHECK(hits[0].status != prism::laws::NEEDS_HARNESS);
    CHECK(hits[0].status != prism::laws::CLEAN);
    CHECK(hits[0].status != prism::laws::FAILED);
    CHECK_FALSE(prism::laws::is_proof(hits[0].status));
    CHECK(hits[0].strength == prism::laws::STRENGTH_READS);
    CHECK(hits[0].cls.empty());
    CHECK(hits[0].cls != "RACE-SHARED");
    CHECK_FALSE(hits[0].function);
    CHECK_FALSE(hits[0].line);
    CHECK(hits[0].file == t1.file);
    CHECK(hits[0].message.find("not found") != std::string::npos);
    CHECK(hits[0].message.find("not checked") != std::string::npos);
    CHECK(hits[0].message.find(t1.file) != std::string::npos);
    CHECK(hits[0].message.find(quiet.file) == std::string::npos);
}

TEST_CASE("thread: pipeline stage is ok with no records when the tree starts no threads") {
    Tree t;
    t.copy_plant("abs_ok.c");
    t.put("shared_quiet.c",
          "int g;\n"
          "void a(void) { g = 1; }\n"
          "void b(void) { g = 2; }\n");
    auto rep = prism::run_pipeline(t.cfg());
    auto& th = stage_of(rep, "thread");
    CHECK(th.status == "ok");
    CHECK(th.status != "NOTRUN");
    CHECK(th.status != "skipped");
    CHECK(th.status != "failed");
    CHECK(th.records == 0);
    CHECK(th.findings.empty());
    CHECK(th.detail.empty());
}

TEST_CASE("thread: pipeline stage records the planted race_global.c RACE-SHARED and stays ok") {
    Tree t;
    t.copy_plant("race_global.c");
    t.put("shared_quiet.c",
          "int g;\n"
          "void a(void) { g = 1; }\n"
          "void b(void) { g = 2; }\n");
    auto rep = prism::run_pipeline(t.cfg());
    auto& th = stage_of(rep, "thread");
    CHECK(th.status == "ok");
    CHECK(th.status != "NOTRUN");
    CHECK(th.records == 1);
    REQUIRE(th.findings.size() == 1);
    check_race(th.findings[0], "g", {"t1", "t2"});
    CHECK(th.findings[0].file == "race_global.c");
}
