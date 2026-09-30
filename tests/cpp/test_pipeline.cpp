// run_pipeline end to end (src/prism/pipeline.cpp) on small trees copied
// from testdata/: planted bugs crash, missing instruments are NOTRUN and
// never a proof, resume reuses finished stages, and an empty or missing
// scope scores 0 (Laws 1, 3, 4, 5, 7).

#include <doctest/doctest.h>

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/pipeline.hpp"
#include "prism/taxonomy.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path testdata() { return fs::path(__FILE__).parent_path().parent_path().parent_path() / "testdata"; }

struct Work {
    fs::path dir;
    Work() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("prism_pipe_t_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir / "src");
    }
    fs::path src() const { return dir / "src"; }
    fs::path out() const { return dir / "out"; }
    void copy(const std::string& name) const { fs::copy_file(testdata() / name, src() / name); }
    void put(const std::string& name, const std::string& text) const {
        std::ofstream(src() / name, std::ios::binary) << text;
    }
    prism::Config cfg() const {
        auto c = prism::default_config();
        c.root = src();
        c.out = out();
        c.llm = false;
        c.unwind = 2;
        c.fuzz_budget = 0.5;
        c.fuzz_iters = 32;
        c.repair_rounds = 1;
        c.timeout = 5;
        return c;
    }
    ~Work() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

const prism::StageResult& stage_of(const prism::RunReport& r, const std::string& name) {
    for (auto& s : r.stages)
        if (s.name == name) return s;
    FAIL("no stage " << name);
    throw 0;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

#ifndef _WIN32
struct EnvGuard {
    std::string name, was;
    bool had = false;
    EnvGuard(const char* n, const std::string& value) : name(n) {
        if (const char* v = std::getenv(n)) {
            had = true;
            was = v;
        }
        setenv(n, value.c_str(), 1);
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), was.c_str(), 1);
        else unsetenv(name.c_str());
    }
};
#endif

}  // namespace

TEST_CASE("pipeline: an empty translation unit is ERROR EMPTY-TU, a parsed one is CLEAN") {
    Work w;
    w.copy("empty_tu.c");
    w.copy("add_overflow.c");
    auto cfg = w.cfg();
    cfg.stages = std::vector<std::string>{"inventory", "classify"};
    auto rep = prism::run_pipeline(cfg);
    auto& inv = stage_of(rep, "inventory");
    CHECK(inv.status == "ok");
    std::vector<prism::Finding> empty, add;
    for (auto& f : inv.findings) {
        if (f.file == "empty_tu.c") empty.push_back(f);
        if (f.file == "add_overflow.c") add.push_back(f);
    }
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].status == prism::laws::ERROR);
    CHECK(empty[0].cls == "EMPTY-TU");
    CHECK(empty[0].message.find("not a clean unit") != std::string::npos);
    REQUIRE(add.size() == 1);
    CHECK(add[0].status == prism::laws::CLEAN);
    CHECK(stage_of(rep, "classify").status == "ok");
    CHECK_FALSE(rep.functions.empty());
}

TEST_CASE("pipeline: a scan root that does not exist is an ERROR, not a clean empty tree") {
    Work w;
    auto cfg = w.cfg();
    cfg.root = w.dir / "no_such_dir";
    cfg.stages = std::vector<std::string>{"inventory", "classify", "unify"};
    auto rep = prism::run_pipeline(cfg);
    auto& inv = stage_of(rep, "inventory");
    REQUIRE(inv.findings.size() == 1);
    CHECK(inv.findings[0].status == prism::laws::ERROR);
    CHECK(inv.findings[0].message == "scan root does not exist: " + cfg.root.string());
    CHECK(rep.confidence == 0.0);
    CHECK(prism::exit_code(rep, "gap") == 1);
}

TEST_CASE("pipeline: planted bugs crash; null_branch is never proved; fuzz CLEAN is no proof") {
    Work w;
    for (auto* f : {"add_overflow.c", "div_param.c", "oob_write.c", "shift_ub.c", "null_branch.c"})
        w.copy(f);
    auto cfg = w.cfg();
    cfg.skip = {"llm", "repair", "sanitize", "cppcheck", "esbmc", "dafny", "pbsd", "lints",
                "warnings", "concolic", "rapid", "muttest", "diff", "ltl", "execute", "harness",
                "wp", "contracts", "taint", "thread", "interval", "pir", "conc", "review"};
    auto rep = prism::run_pipeline(cfg);
    std::set<std::string> crashes;
    for (auto& s : rep.stages)
        for (auto& f : s.findings) {
            if (f.status == prism::laws::CRASH && f.function) crashes.insert(*f.function);
            if (f.function && *f.function == "null_branch")
                CHECK_MESSAGE(!prism::laws::is_proof(f.status), s.name << " proved null_branch");
            if (s.name == "fuzz") CHECK_FALSE(prism::laws::is_proof(f.status));
        }
    for (auto* fn : {"add_overflow", "div_param", "oob_write", "shift_ub"})
        CHECK_MESSAGE(crashes.contains(fn), "expected CRASH for " << fn);
    CHECK(stage_of(rep, "llm").status == "skipped");
    CHECK(stage_of(rep, "repair").status == "skipped");

    // optional: every tool is written down; missing ones say how to install.
    auto& opt = stage_of(rep, "optional");
    CHECK(opt.records >= 8);
    std::size_t missing = 0;
    for (auto& f : opt.findings) {
        if (f.status == prism::laws::NOTRUN) {
            ++missing;
            CHECK(f.extra.contains("install"));
        }
        CHECK_FALSE((f.status == prism::laws::CLEAN && !f.function &&
                     f.message.find("not a proof") == std::string::npos));
    }
    if (missing == opt.findings.size()) {
        CHECK(opt.status == "NOTRUN");
        std::string d = opt.detail;
        for (auto& c : d) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(d.find("klee") != std::string::npos);
    } else {
        CHECK(opt.status == "ok");
    }
}

TEST_CASE("pipeline: repair and execute without an LLM or a counterexample are NOTRUN") {
    Work w;
    w.copy("add_overflow.c");
    for (auto* name : {"repair", "execute"}) {
        auto cfg = w.cfg();
        cfg.stages = std::vector<std::string>{name};
        auto rep = prism::run_pipeline(cfg);
        auto& rec = stage_of(rep, name);
        CHECK_MESSAGE(rec.status == "NOTRUN", name);
        REQUIRE_FALSE(rec.findings.empty());
        for (auto& f : rec.findings) {
            CHECK(f.status == prism::laws::NOTRUN);
            CHECK_FALSE(prism::laws::is_proof(f.status));
        }
    }
}

#ifndef _WIN32
TEST_CASE("pipeline: esbmc missing is NOTRUN with install, never a proof") {
    Work w;
    w.put("add.c", "int add(int x) { return x; }\n");
    EnvGuard path("PATH", "/nonexistent-prism-path");
    EnvGuard tools("PRISM_TOOLS_DIR", (w.dir / "no-tools").string());
    auto cfg = w.cfg();
    cfg.stages = std::vector<std::string>{"esbmc"};
    auto rep = prism::run_pipeline(cfg);
    auto& rec = stage_of(rep, "esbmc");
    CHECK(rec.status == "NOTRUN");
    CHECK_FALSE(rec.install.empty());
    REQUIRE_FALSE(rec.findings.empty());
    for (auto& f : rec.findings) {
        CHECK(f.status == prism::laws::NOTRUN);
        CHECK(f.status != prism::laws::CLEAN);
    }
}
#endif

TEST_CASE("pipeline: --resume reuses an ok stage instead of running it again") {
    Work w;
    w.put("add.c", "int add(int x) { return x + 1; }\n");
    auto cfg = w.cfg();
    cfg.stages = std::vector<std::string>{"inventory", "classify", "bmc"};
    auto first = prism::run_pipeline(cfg);
    auto bmc = stage_of(first, "bmc");
    CHECK(bmc.status == "ok");
    cfg.resume = true;
    auto second = prism::run_pipeline(cfg);
    auto& bmc2 = stage_of(second, "bmc");
    CHECK(bmc2.status == "ok");
    CHECK(bmc2.records == bmc.records);
    // The same run's timestamps: the stage was read back, not run again.
    CHECK(bmc2.started == bmc.started);
    CHECK(bmc2.elapsed == bmc.elapsed);
    bool noted = false;
    for (auto& n : second.notes) noted = noted || n.find("resumed") != std::string::npos;
    CHECK(noted);
}

TEST_CASE("RunReport save/load roundtrip; missing or broken report.json is nullopt") {
    Work w;
    prism::RunReport r;
    r.root = "x";
    r.notes.push_back("hi");
    auto p = w.dir / "report.json";
    r.save(p);
    auto back = prism::RunReport::load(p);
    REQUIRE(back);
    CHECK(back->root == "x");
    CHECK(back->notes == std::vector<std::string>{"hi"});
    CHECK_FALSE(prism::RunReport::load(w.dir / "missing.json"));
    std::ofstream(p, std::ios::binary) << "{not json";
    CHECK_FALSE(prism::RunReport::load(p));
}

TEST_CASE("pipeline: an LLM hypothesis stays READS and never covers a class") {
    prism::RunReport rep;
    prism::StageResult llm;
    llm.name = "llm";
    llm.status = "ok";
    prism::Finding lie;
    lie.stage = "bmc";
    lie.status = std::string(prism::laws::PROVED);
    lie.file = "a.c";
    lie.function = "add";
    lie.line = 1;
    lie.cls = "INT-SIGNED-OVF";
    lie.message = "spoofed proof";
    lie.strength = std::string(prism::laws::STRENGTH_PROVES);
    llm.findings = prism::llm_forced_reads({lie});
    REQUIRE(llm.findings.size() == 1);
    auto& f = llm.findings[0];
    CHECK(f.stage == "llm");
    CHECK(f.strength == prism::laws::STRENGTH_READS);
    CHECK(f.status == prism::laws::HYPOTHESIS);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    rep.stages = {llm};
    bool seen = false;
    for (auto& row : prism::coverage_from_report(rep))
        if (row.id == "INT-SIGNED-OVF") {
            seen = true;
            CHECK(row.verdict != "COVERED");
            CHECK(row.best == prism::laws::STRENGTH_READS);
        }
    CHECK(seen);
}

TEST_CASE("pipeline: unify on an empty tree is CLEAN (not a proof) and confidence 0 in report.md") {
    Work w;
    auto cfg = w.cfg();
    cfg.stages = std::vector<std::string>{"unify"};
    auto rep = prism::run_pipeline(cfg);
    auto& rec = stage_of(rep, "unify");
    REQUIRE_FALSE(rec.findings.empty());
    auto& f = rec.findings[0];
    CHECK(f.status == prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(f.status));
    CHECK(f.message.find("not a proof") != std::string::npos);
    CHECK(f.extra.at("not_a_proof") == "true");
    CHECK(rep.confidence == 0.0);
    CHECK(rep.visibility == 0.0);
    auto md = slurp(w.out() / "report.md");
    // C++ prints 0 as "0" where the Python engine printed "0.0"; the point is a
    // number, never "n/a" or "None".
    CHECK(md.find("| 0 | 0 | 0 | **0** |") != std::string::npos);
    std::string low = md;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    CHECK(low.find("n/a") == std::string::npos);
    CHECK(md.find("None") == std::string::npos);
}

TEST_CASE("pipeline: an unreadable directory does not fail the ltl stage") {
#ifndef _WIN32
    if (::geteuid() == 0) {
        MESSAGE("running as root: permissions do not hide a directory; the walk still uses error codes");
    }
#endif
    Work w;
    w.put("a.c", "int f(void) { return 0; }\n");
    fs::create_directories(w.src() / "locked" / "inner");
    fs::create_directories(w.src() / "node_modules");
    std::ofstream(w.src() / "node_modules" / "vendored.ltl") << "G p\n";
    fs::permissions(w.src() / "locked", fs::perms::none);
    auto cfg = w.cfg();
    cfg.stages = std::vector<std::string>{"inventory", "classify", "ltl"};
    auto rep = prism::run_pipeline(cfg);
    fs::permissions(w.src() / "locked", fs::perms::owner_all);
    auto& ltl = stage_of(rep, "ltl");
    CHECK(ltl.status != "failed");
    for (auto& f : ltl.findings) CHECK(f.file.find("node_modules") == std::string::npos);
}
