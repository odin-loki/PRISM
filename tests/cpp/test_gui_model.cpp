// Qt-free model of the PRISM window (include/prism/gui_model.hpp) and the
// `prism --gui` launcher. Headless: nothing here opens a window. The window
// itself is tested offscreen in tests/cpp/test_gui.cpp (PRISM_QT=ON).
//
// Guards: statuses copied verbatim (PROVED / CLEAN / NOTRUN stay distinct),
// CLEAN is never proof-green, the LLM never covers a class, confidence is
// vis x ans x res and no data is 0 (never n/a), a missing display or a
// missing prism_gui is NOTRUN and never runs the pipeline, allow-exec is off
// unless asked.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/gui_model.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/taxonomy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace pg = prism::gui;
namespace laws = prism::laws;

namespace {

std::string S(std::string_view v) { return std::string(v); }

prism::Finding finding(const std::string& stage, std::string_view status, const std::string& cls = "",
                       const std::string& message = "", const std::string& file = "a.c",
                       std::optional<std::string> function = std::string("add"),
                       std::optional<int> line = 3) {
    prism::Finding f;
    f.stage = stage;
    f.status = S(status);
    f.file = file;
    f.function = function;
    f.line = line;
    f.cls = cls;
    f.message = message;
    f.strength = laws::is_proof(status) ? S(laws::STRENGTH_PROVES)
                 : status == laws::NOTRUN ? S(laws::STRENGTH_READS)
                                          : S(laws::STRENGTH_FINDS);
    return f;
}

prism::StageResult stage(const std::string& name, const std::string& status,
                         std::vector<prism::Finding> fs) {
    prism::StageResult s;
    s.name = name;
    s.status = status;
    s.findings = std::move(fs);
    return s;
}

prism::RunReport report_with(double vis, double ans, double res, double conf) {
    prism::RunReport r;
    r.root = "mem";
    r.visibility = vis;
    r.answer = ans;
    r.resolution = res;
    r.confidence = conf;
    return r;
}

// PLAN window of one report: unbounded proof, missing ESBMC, CLEAN fuzz.
prism::RunReport mixed_report() {
    auto r = report_with(1.0, 0.5, 0.5, 0.25);
    r.stages = {
        stage("bmc", "ok", {finding("bmc", laws::PROVED_UNBOUNDED, "INT-SIGNED-OVF",
                                    "k-induction closed; unbounded")}),
        stage("fuzz", "ok", {finding("fuzz", laws::CLEAN, "", "no crash (not a proof)")}),
        stage("esbmc", "NOTRUN",
              {finding("esbmc", laws::NOTRUN, "", "esbmc not on PATH", "", std::nullopt, std::nullopt)}),
    };
    return r;
}

void check_mixed_honesty(const std::vector<pg::FindingRow>& rows) {
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].status == laws::PROVED_UNBOUNDED);
    CHECK(rows[1].status == laws::CLEAN);
    CHECK(rows[2].status == laws::NOTRUN);
    const auto& esbmc = rows[2];
    CHECK(esbmc.stage == "esbmc");
    CHECK_FALSE(laws::is_proof(esbmc.status));
    CHECK(esbmc.status != laws::PROVED);
    CHECK(esbmc.status != laws::CLEAN);
    CHECK(esbmc.file.empty());
    CHECK(esbmc.line.empty());
    CHECK(esbmc.function.empty());
    CHECK(rows[1].stage == "fuzz");
    CHECK_FALSE(laws::is_proof(rows[1].status));
    CHECK(rows[0].stage == "bmc");
    CHECK(laws::is_proof(rows[0].status));
}

const pg::TaxonomyRow* tax(const std::vector<pg::TaxonomyRow>& rows, const std::string& id) {
    for (const auto& r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

std::set<std::string> verdicts(const std::vector<pg::TaxonomyRow>& rows) {
    std::set<std::string> v;
    for (const auto& r : rows) v.insert(r.verdict);
    return v;
}

fs::path temp_dir(const std::string& tag) {
    auto d = fs::temp_directory_path() /
             ("prism-gui-" + tag + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

#ifndef _WIN32
void make_file(const fs::path& p, bool exec) {
    std::ofstream(p) << "#!/bin/sh\nexit 0\n";
    ::chmod(p.c_str(), exec ? 0755 : 0644);
}

fs::path self_dir() {
    char buf[4096]{};
    auto n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? fs::path(std::string(buf, static_cast<std::size_t>(n))).parent_path() : fs::path();
}

struct Run {
    int code = -1;
    std::string out;
};

Run run_shell(const std::string& cmd) {
    Run r;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return r;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    int st = ::pclose(p);
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}
#endif

bool has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// Value after `flag` in an argv ("" when absent).
std::string after(const std::vector<std::string>& v, const std::string& flag) {
    auto it = std::find(v.begin(), v.end(), flag);
    return it == v.end() || it + 1 == v.end() ? std::string() : *(it + 1);
}

}  // namespace

TEST_SUITE("gui model") {

TEST_CASE("skip_from_checks: fuzz, repair, optional in that order") {
    using V = std::vector<std::string>;
    CHECK(pg::skip_from_checks(false, false, false) == V{});
    CHECK(pg::skip_from_checks(true, true, true) == V{"fuzz", "repair", "optional"});
    CHECK(pg::skip_from_checks(true, false, false) == V{"fuzz"});
    CHECK(pg::skip_from_checks(false, true, false) == V{"repair"});
    CHECK(pg::skip_from_checks(false, false, true) == V{"optional"});
    CHECK(pg::skip_from_checks(true, false, true) == V{"fuzz", "optional"});
}

TEST_CASE("skip_from_checks: the Config skip list honours the boxes") {
    auto cfg = prism::default_config();
    cfg.skip = pg::skip_from_checks(true, false, true);
    cfg.llm = false;
    CHECK_FALSE(cfg.want("fuzz"));
    CHECK(cfg.want("bmc"));
    CHECK(cfg.want("repair"));
    CHECK_FALSE(cfg.want("optional"));
    CHECK_FALSE(cfg.llm);
}

TEST_CASE("confidence label is the CLI line: vis x ans x res product") {
    auto r = report_with(1.0, 0.5, 0.5, 0.25);
    // `prism PATH` prints numbers with the default stream format ("1", not "1.0").
    CHECK(pg::confidence_label(r) == "confidence 0.25  (vis 1 x ans 0.5 x res 0.5)");
    auto r2 = report_with(0.8, 0.4, 0.25, 0.08);
    CHECK(pg::confidence_label(r2) == "confidence 0.08  (vis 0.8 x ans 0.4 x res 0.25)");
    auto c = pg::confidence_product(r2);
    CHECK(c.visibility == 0.8);
    CHECK(c.answer == 0.4);
    CHECK(c.resolution == 0.25);
    CHECK(c.confidence == 0.08);
    auto c1 = pg::confidence_product(r);
    CHECK(c1.confidence == c1.visibility * c1.answer * c1.resolution);
}

TEST_CASE("confidence 0 is no data, not CLEAN and never n/a") {
    prism::RunReport r;
    r.root = "mem";
    const auto label = pg::confidence_label(r);
    CHECK(label == "confidence 0  (vis 0 x ans 0 x res 0)");
    CHECK(label.find(laws::CLEAN) == std::string::npos);
    CHECK(label.find("n/a") == std::string::npos);
    auto c = pg::confidence_product(r);
    CHECK(c.visibility == 0);
    CHECK(c.answer == 0);
    CHECK(c.resolution == 0);
    CHECK(c.confidence == 0);
}

TEST_CASE("confidence fields: n/a / none / null / NaN / inf are 0, numeric strings are numbers") {
    CHECK(pg::confidence_number(std::string_view("n/a")) == 0);
    CHECK(pg::confidence_number(std::string_view("N/A")) == 0);
    CHECK(pg::confidence_number(std::string_view("None")) == 0);
    CHECK(pg::confidence_number(std::string_view("null")) == 0);
    CHECK(pg::confidence_number(std::string_view("")) == 0);
    CHECK(pg::confidence_number(std::string_view("NaN")) == 0);
    CHECK(pg::confidence_number(std::string_view("inf")) == 0);
    CHECK(pg::confidence_number(std::string_view("abc")) == 0);
    CHECK(pg::confidence_number(std::string_view("0.5x")) == 0);
    CHECK(pg::confidence_number(std::string_view("0.5")) == 0.5);
    CHECK(pg::confidence_number(std::string_view(" 0.25 ")) == 0.25);
    CHECK(pg::confidence_number(std::string_view("+1")) == 1.0);
    CHECK(pg::confidence_number(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(pg::confidence_number(std::numeric_limits<double>::infinity()) == 0);

    auto r = report_with(0, 0, std::numeric_limits<double>::quiet_NaN(), 0);
    auto c = pg::confidence_product(r);
    CHECK(c.resolution == 0);
    const auto label = pg::confidence_label(r);
    CHECK(label.find("nan") == std::string::npos);
    CHECK(label.find("n/a") == std::string::npos);
    CHECK(label.find(laws::CLEAN) == std::string::npos);

    // report.json text: a string "0.5" is 0.5 (a QJsonValue::toDouble would
    // read 0), placeholders and null are 0, a missing key is 0.
    auto j = pg::confidence_from_json(
        R"({"visibility": "0.5", "answer": "n/a", "resolution": null, "confidence": "N/A"})");
    CHECK(j.visibility == 0.5);
    CHECK(j.answer == 0);
    CHECK(j.resolution == 0);
    CHECK(j.confidence == 0);
    auto k = pg::confidence_from_json(R"({"visibility": 1, "answer": 0.5})");
    CHECK(k.visibility == 1);
    CHECK(k.answer == 0.5);
    CHECK(k.resolution == 0);
    auto bad = pg::confidence_from_json("{not json");
    CHECK(bad.visibility == 0);
    CHECK(bad.confidence == 0);
}

TEST_CASE("missing report.json label: 0, not n/a, not CLEAN, not a proof") {
    CHECK(pg::MISSING_REPORT_LABEL == "confidence 0  (report.json missing; not a proof)");
    CHECK(pg::MISSING_REPORT_LABEL.find("n/a") == std::string_view::npos);
    CHECK(pg::MISSING_REPORT_LABEL.find(laws::CLEAN) == std::string_view::npos);
    CHECK(pg::MISSING_REPORT_LABEL.find("PROVED") == std::string_view::npos);
}

TEST_CASE("window constants") {
    CHECK(pg::WINDOW_TITLE == "PRISM — hybrid code testing");
    CHECK(pg::DEFAULT_OUT == "prism-out-gui");
    CHECK(pg::POLL_MS == 400);
    std::vector<std::string> fc(pg::FINDING_COLUMNS.begin(), pg::FINDING_COLUMNS.end());
    CHECK(fc == std::vector<std::string>{"status", "stage", "cls", "file", "line", "function",
                                         "message"});
    std::vector<std::string> sc(pg::STAGE_COLUMNS.begin(), pg::STAGE_COLUMNS.end());
    CHECK(sc == std::vector<std::string>{"stage", "status", "records", "seconds", "note"});
}

TEST_CASE("finding rows: PROVED, CLEAN and NOTRUN stay distinct") {
    auto r = report_with(1.0, 0.5, 0.5, 0.25);
    r.stages = {
        stage("bmc", "ok", {finding("bmc", laws::PROVED, "INT-SIGNED-OVF", "holds under k-induction")}),
        stage("fuzz", "ok", {finding("fuzz", laws::CLEAN, "", "no crash (not a proof)")}),
        stage("esbmc", "NOTRUN",
              {finding("esbmc", laws::NOTRUN, "", "esbmc not on PATH", "", std::nullopt, std::nullopt)}),
    };
    auto rows = pg::finding_rows(r);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].status == laws::PROVED);
    CHECK(rows[1].status == laws::CLEAN);
    CHECK(rows[2].status == laws::NOTRUN);
    CHECK(rows[0].stage == "bmc");
    CHECK(rows[0].cls == "INT-SIGNED-OVF");
    CHECK(rows[0].file == "a.c");
    CHECK(rows[0].line == "3");
    CHECK(rows[0].function == "add");
    CHECK(rows[1].stage == "fuzz");
    CHECK(rows[1].cls.empty());
    CHECK(rows[2].stage == "esbmc");
    CHECK(rows[2].file.empty());
    CHECK(rows[0].message != rows[1].message);
    for (auto col : pg::FINDING_COLUMNS) CHECK(&rows[0].column(col) != &rows[0].id);
    CHECK(rows[0].column("status") == laws::PROVED);
    CHECK(rows[0].column("message") == "holds under k-induction");
    CHECK(pg::confidence_label(r) == "confidence 0.25  (vis 1 x ans 0.5 x res 0.5)");
}

TEST_CASE("finding rows: CLEAN/NOTRUN of inventory, classify, unify are noise") {
    prism::RunReport r;
    r.stages = {
        stage("unify", "ok", {finding("unify", laws::CLEAN, "", "taxonomy 0/0")}),
        stage("inventory", "ok", {finding("inventory", laws::CLEAN, "", "parsed")}),
        stage("classify", "ok", {finding("classify", laws::NOTRUN, "", "skip"),
                                 finding("classify", laws::FAILED, "X", "real")}),
        stage("bmc", "ok", {finding("bmc", laws::PROVED, "INT-DIV-ZERO", "holds")}),
    };
    auto rows = pg::finding_rows(r);
    REQUIRE(rows.size() == 2);
    // A non-noise status on a noise stage is kept; ids count every finding
    // (as `prism ask` / explain number them).
    CHECK(rows[0].status == laws::FAILED);
    CHECK(rows[0].id == "classify#1");
    CHECK(rows[1].status == laws::PROVED);
    CHECK(rows[1].id == "bmc#0");
}

TEST_CASE("finding rows: same vocabulary without Qt, and through report.json") {
    auto r = mixed_report();
    check_mixed_honesty(pg::finding_rows(r));
    CHECK(pg::confidence_label(r).find(laws::CLEAN) == std::string::npos);

    auto dir = temp_dir("roundtrip");
    r.save(dir / "report.json");
    auto loaded = prism::RunReport::load(dir / "report.json");
    REQUIRE(loaded);
    check_mixed_honesty(pg::finding_rows(*loaded));
    CHECK(pg::confidence_label(*loaded) == pg::confidence_label(r));
    auto c = pg::confidence_from_json(slurp(dir / "report.json"));
    CHECK(pg::confidence_label(c) == pg::confidence_label(r));
    fs::remove_all(dir);
}

TEST_CASE("finding rows: stage falls back to the stage name; message cut at 200 chars") {
    prism::RunReport r;
    auto f = finding("", laws::FAILED, "C", "");
    // 250 two-byte characters: the cut is 200 characters, never mid-sequence.
    for (int i = 0; i < 250; ++i) f.message += "\xc3\xa9";
    r.stages = {stage("lints", "ok", {f})};
    auto rows = pg::finding_rows(r);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].stage == "lints");
    CHECK(rows[0].message.size() == 400);
    CHECK(rows[0].message.substr(0, 2) == "\xc3\xa9");
}

TEST_CASE("finding rows: HYPOTHESIS stays HYPOTHESIS, never a proof") {
    prism::RunReport r;
    r.stages = {stage("llm", "ok", {finding("llm", laws::HYPOTHESIS, "INT-SIGNED-OVF", "maybe overflow")})};
    auto rows = pg::finding_rows(r);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == laws::HYPOTHESIS);
    CHECK(rows[0].stage == "llm");
    CHECK_FALSE(laws::is_proof(rows[0].status));
    CHECK(rows[0].status != laws::CLEAN);
    CHECK(rows[0].status != "COVERED");
}

TEST_CASE("taxonomy rows: COVERED / GAP, never CLEAN or a proof status") {
    auto rows = pg::taxonomy_rows(mixed_report());
    REQUIRE_FALSE(rows.empty());
    auto v = verdicts(rows);
    CHECK(v.count("COVERED"));
    CHECK(v.count("GAP"));
    CHECK_FALSE(v.count(S(laws::CLEAN)));
    CHECK_FALSE(v.count(S(laws::PROVED)));
    CHECK_FALSE(v.count(S(laws::PROVED_UNBOUNDED)));
    auto* ovf = tax(rows, "INT-SIGNED-OVF");
    REQUIRE(ovf);
    CHECK(ovf->verdict == "COVERED");
    for (const auto& r : rows)
        CHECK((r.verdict == "COVERED" || r.verdict == "PARTIAL" || r.verdict == "GAP"));
}

TEST_CASE("taxonomy rows: a CLEAN unify line covers nothing") {
    prism::RunReport r;
    r.stages = {stage("unify", "ok",
                      {finding("unify", laws::CLEAN, "", "taxonomy 0/0 COVERED, 0 GAP (not a proof)")})};
    auto rows = pg::taxonomy_rows(r);
    REQUIRE_FALSE(rows.empty());
    for (const auto& t : rows) CHECK((t.verdict == "GAP" || t.verdict == "PARTIAL"));
}

TEST_CASE("taxonomy rows: an LLM hypothesis never covers (Law 4)") {
    prism::RunReport r;
    r.stages = {stage("llm", "ok", {finding("llm", laws::HYPOTHESIS, "INT-SIGNED-OVF", "maybe overflow")})};
    auto rows = pg::taxonomy_rows(r);
    auto* ovf = tax(rows, "INT-SIGNED-OVF");
    REQUIRE(ovf);
    CHECK(ovf->verdict != "COVERED");
    CHECK((ovf->verdict == "PARTIAL" || ovf->verdict == "GAP"));
    CHECK(ovf->best == laws::STRENGTH_READS);
    CHECK_FALSE(verdicts(rows).count("COVERED"));
    CHECK_FALSE(verdicts(rows).count(S(laws::CLEAN)));
}

TEST_CASE("taxonomy rows: a spoofed FAILED/FINDS on the llm stage is still READS") {
    prism::RunReport r;
    auto f = finding("llm", laws::FAILED, "INT-SIGNED-OVF", "spoof");
    f.strength = S(laws::STRENGTH_FINDS);
    r.stages = {stage("llm", "ok", {f})};
    auto rows = pg::taxonomy_rows(r);
    auto* ovf = tax(rows, "INT-SIGNED-OVF");
    REQUIRE(ovf);
    CHECK(ovf->verdict != "COVERED");
    CHECK(ovf->best == laws::STRENGTH_READS);
    CHECK_FALSE(verdicts(rows).count("COVERED"));
}

TEST_CASE("taxonomy rows: an LLM hypothesis cannot un-cover a BMC proof") {
    auto r = mixed_report();
    r.stages.push_back(stage("llm", "ok", {finding("llm", laws::HYPOTHESIS, "INT-SIGNED-OVF", "maybe")}));
    auto rows = pg::taxonomy_rows(r);
    auto* ovf = tax(rows, "INT-SIGNED-OVF");
    REQUIRE(ovf);
    CHECK(ovf->verdict == "COVERED");
    auto fr = pg::finding_rows(r);
    std::set<std::string> st;
    for (const auto& x : fr) st.insert(x.status);
    CHECK(st.count(S(laws::HYPOTHESIS)));
    CHECK(st.count(S(laws::PROVED_UNBOUNDED)));
}

TEST_CASE("taxonomy rows: a planted taxonomy key in report.json is ignored") {
    auto dir = temp_dir("stale-tax");
    std::ofstream(dir / "report.json")
        << R"({"root": "mem", "stages": [], "taxonomy": [)"
        << R"({"id": "INT-SIGNED-OVF", "verdict": "COVERED", "best": "PROVES"}]})";
    auto loaded = prism::RunReport::load(dir / "report.json");
    REQUIRE(loaded);
    auto rows = pg::taxonomy_rows(*loaded);
    auto* ovf = tax(rows, "INT-SIGNED-OVF");
    REQUIRE(ovf);
    CHECK(ovf->verdict == "GAP");
    fs::remove_all(dir);
}

TEST_CASE("refuse_llm_cover demotes READS and only-LLM COVERED") {
    prism::RunReport empty;
    CHECK(prism::refuse_llm_cover(empty, "INT-SIGNED-OVF", "COVERED", S(laws::STRENGTH_READS)) == "PARTIAL");
    CHECK(prism::refuse_llm_cover(empty, "INT-SIGNED-OVF", "GAP", S(laws::STRENGTH_READS)) == "GAP");
    prism::RunReport llm_only;
    llm_only.stages = {stage("llm", "ok", {finding("llm", laws::HYPOTHESIS, "INT-SIGNED-OVF", "maybe")})};
    CHECK(prism::refuse_llm_cover(llm_only, "INT-SIGNED-OVF", "COVERED", S(laws::STRENGTH_PROVES)) ==
          "PARTIAL");
    CHECK(prism::refuse_llm_cover(mixed_report(), "INT-SIGNED-OVF", "COVERED",
                                  S(laws::STRENGTH_PROVES)) == "COVERED");
}

TEST_CASE("palette: CLEAN is blue, never proof-green; COVERED reuses PROVED green") {
    const auto clean = pg::status_background(laws::CLEAN);
    const auto proved = pg::status_background(laws::PROVED);
    const auto unbounded = pg::status_background(laws::PROVED_UNBOUNDED);
    const auto assuming = pg::status_background(laws::PROVED_ASSUMING);
    const auto bounded = pg::status_background(laws::BOUNDED);
    CHECK(clean == "#2a4a6b");
    CHECK(proved == "#2a8148");
    CHECK(unbounded == "#1f6f3a");
    CHECK(bounded == "#6b6b2a");
    CHECK(clean != proved);
    CHECK(clean != unbounded);
    CHECK(clean != assuming);
    CHECK(clean != pg::status_background(laws::NOTRUN));
    CHECK(bounded != clean);
    CHECK(bounded != proved);
    CHECK_FALSE(pg::is_proof_green(clean));
    CHECK(pg::is_proof_green(proved));
    CHECK(pg::is_proof_green(unbounded));
    CHECK(pg::is_proof_green(assuming));
    CHECK_FALSE(pg::is_proof_green(bounded));
    CHECK_FALSE(pg::is_proof_green(""));
    // Neither an LLM status nor NOTRUN is ever painted as a proof.
    CHECK_FALSE(pg::is_proof_green(pg::status_background(laws::HYPOTHESIS)));
    CHECK_FALSE(pg::is_proof_green(pg::status_background(laws::READS)));
    CHECK_FALSE(pg::is_proof_green(pg::status_background(laws::NOTRUN)));
    CHECK(pg::status_background("SOMETHING-ELSE").empty());
    CHECK(pg::taxonomy_background("COVERED") == proved);
    CHECK(pg::taxonomy_background("COVERED") != clean);
    CHECK(pg::taxonomy_background("PARTIAL") == bounded);
    CHECK(pg::taxonomy_background("PARTIAL") != proved);
    CHECK(pg::taxonomy_background("PARTIAL") != clean);
    CHECK(pg::taxonomy_background("GAP") == pg::status_background(laws::NOTRUN));
    CHECK(pg::taxonomy_background(laws::CLEAN) == clean);
    CHECK_FALSE(pg::is_proof_green(pg::taxonomy_background(laws::CLEAN)));
}

TEST_CASE("stage rows: stage / status / records / seconds / note") {
    prism::StageResult s;
    s.name = "bmc";
    s.status = "ok";
    s.records = 7;
    s.elapsed = 1.234;
    s.detail = "3 functions";
    auto r = pg::stage_row(s);
    CHECK(r.stage == "bmc");
    CHECK(r.status == "ok");
    CHECK(r.records == "7");
    CHECK(r.seconds == "1.23");
    CHECK(r.note == "3 functions");
    s.detail.clear();
    s.status = S(laws::NOTRUN);
    s.install = "apt install esbmc";
    s.elapsed = std::numeric_limits<double>::quiet_NaN();
    r = pg::stage_row(s);
    CHECK(r.status == laws::NOTRUN);
    CHECK(r.note == "apt install esbmc");
    CHECK(r.seconds == "0.00");
    CHECK(pg::stage_rows({s, s}).size() == 2);
}

TEST_CASE("live progress: only this run's journal rows, one log line per change") {
    auto row = [](const std::string& n, const std::string& st, int recs, double started) {
        prism::StageResult s;
        s.name = n;
        s.status = st;
        s.records = recs;
        s.started = started;
        return s;
    };
    std::vector<prism::StageResult> journal{row("inventory", "ok", 3, 100), row("lints", "ok", 2, 200)};
    // Without --resume, rows older than the run are the previous run's.
    auto live = pg::live_stages(journal, 150, false);
    REQUIRE(live.size() == 1);
    CHECK(live[0].name == "lints");
    CHECK(pg::live_stages(journal, 150, true).size() == 2);
    CHECK(pg::live_stages(journal, 300, false).empty());

    CHECK(pg::progress_line(journal[1]) == "lints ok (2)");
    pg::ProgressTracker t;
    CHECK_FALSE(t.update({}));
    auto l1 = t.update({journal[0]});
    REQUIRE(l1);
    CHECK(*l1 == "inventory ok (3)");
    CHECK_FALSE(t.update({journal[0]}));
    auto l2 = t.update(journal);
    REQUIRE(l2);
    CHECK(*l2 == "lints ok (2)");
    CHECK_FALSE(t.update(journal));
    auto changed = journal;
    changed[1].records = 5;
    CHECK(t.update(changed) == std::optional<std::string>("lints ok (5)"));
    t.reset();
    CHECK(t.update(changed));
}

TEST_CASE("done summary: confidence line and the NOTRUN stages") {
    auto r = mixed_report();
    auto c = pg::confidence_product(r);
    CHECK(pg::done_summary(c, r.stages) ==
          "done. confidence 0.25  (vis 1 x ans 0.5 x res 0.5). NOTRUN=esbmc");
    r.stages.push_back(stage("fuzz2", "NOTRUN", {}));
    CHECK(pg::done_summary(c, r.stages).ends_with("NOTRUN=esbmc,fuzz2"));
    CHECK(pg::done_summary({}, {}) == "done. confidence 0  (vis 0 x ans 0 x res 0). NOTRUN=none");
}

TEST_CASE("run failure: exit 0 and --fail-on's exit 1 are verdicts; the rest raise a dialog") {
    CHECK_FALSE(pg::run_failure(0, false));
    CHECK_FALSE(pg::run_failure(1, false));
    auto two = pg::run_failure(2, false);
    REQUIRE(two);
    CHECK(two->find("not a clean run") != std::string::npos);
    CHECK(pg::run_failure(3, false));
    auto crash = pg::run_failure(0, true);
    REQUIRE(crash);
    CHECK(crash->find("crashed") != std::string::npos);
    CHECK(crash->find(laws::CLEAN) == std::string::npos);
}

TEST_CASE("missing display: a NOTRUN finding, never CLEAN, never a proof") {
    auto f = pg::missing_display_finding("qt.qpa.plugin");
    CHECK(f.status == laws::NOTRUN);
    CHECK(f.stage == "gui");
    CHECK(f.message.find("NOTRUN") != std::string::npos);
    CHECK(f.message.find("display") != std::string::npos);
    CHECK(f.message.find("qt.qpa.plugin") != std::string::npos);
    CHECK(f.message.find(laws::CLEAN) == std::string::npos);
    CHECK_FALSE(laws::is_proof(f.status));
    CHECK(f.extra.at("install") == "QT_QPA_PLATFORM=offscreen or a real display");
    const auto text = pg::notrun_display_text();
    CHECK(text.starts_with("NOTRUN gui: no display — not a clean window\n"));
    CHECK(text.find("QT_QPA_PLATFORM=offscreen") != std::string::npos);
    CHECK(text.find(laws::CLEAN) == std::string::npos);
    CHECK(text.find("confidence") == std::string::npos);
}

TEST_CASE("display_available: DISPLAY, WAYLAND_DISPLAY or offscreen") {
#ifndef _WIN32
    CHECK_FALSE(pg::display_available(nullptr, nullptr, nullptr));
    CHECK_FALSE(pg::display_available("", "", nullptr));
    CHECK_FALSE(pg::display_available(nullptr, nullptr, "xcb"));
    CHECK(pg::display_available(":0", nullptr, nullptr));
    CHECK(pg::display_available(nullptr, "wayland-0", nullptr));
    CHECK(pg::display_available(nullptr, nullptr, "offscreen"));
#else
    CHECK(pg::display_available(nullptr, nullptr, nullptr));
#endif
}

TEST_CASE("run args: GUI budgets, prism-out-gui, LLM on, allow-exec off by default") {
    pg::RunOptions o;
    o.path = "src";
    o.out = "prism-out-gui";
    auto a = pg::gui_run_args(o);
    CHECK(a[0] == "src");
    CHECK(after(a, "--out") == "prism-out-gui");
    CHECK_FALSE(has(a, "--no-llm"));
    CHECK_FALSE(has(a, "--allow-exec"));  // Law 9: off unless asked
    CHECK_FALSE(has(a, "--resume"));
    CHECK_FALSE(has(a, "--skip"));
    CHECK(after(a, "--fuzz-budget") == "4");
    CHECK(after(a, "--fuzz-iters") == "256");
    CHECK(after(a, "--repair-rounds") == "1");

    o.llm = false;
    o.resume = true;
    o.allow_exec = true;
    o.skip_fuzz = true;
    o.skip_optional = true;
    o.extra_skip = {"fuzz", "bmc"};
    o.extra = {"--fuzz-budget", "30", "--pir-drafts"};
    a = pg::gui_run_args(o);
    CHECK(has(a, "--no-llm"));
    CHECK(has(a, "--resume"));
    CHECK(has(a, "--allow-exec"));
    CHECK(after(a, "--skip") == "fuzz,optional,bmc");
    // A budget given at launch wins; the GUI default is not passed twice.
    CHECK(std::count(a.begin(), a.end(), "--fuzz-budget") == 1);
    CHECK(after(a, "--fuzz-budget") == "30");
    CHECK(after(a, "--fuzz-iters") == "256");
    CHECK(has(a, "--pir-drafts"));
}

TEST_CASE("launch parsing: boxes, --out, pass-through flags and ignored flags") {
    auto none = pg::parse_launch({});
    CHECK_FALSE(none.from_cli);
    CHECK(none.path.empty());
    CHECK_FALSE(none.allow_exec);
    CHECK_FALSE(none.no_llm);

    auto l = pg::parse_launch({"--gui", "--unwind", "5", "src", "--no-llm", "--skip",
                               "fuzz, optional,bmc", "--allow-exec", "--resume", "--pir-drafts",
                               "--out=o", "--jobs=4", "--version", "--pir-vcs", "x.c", "other"});
    CHECK(l.from_cli);
    CHECK(l.path == "src");  // "5" is --unwind's value, not the path
    CHECK(l.no_llm);
    CHECK(l.skip_fuzz);
    CHECK_FALSE(l.skip_repair);
    CHECK(l.skip_optional);
    CHECK(l.extra_skip == std::vector<std::string>{"bmc"});
    CHECK(l.allow_exec);
    CHECK(l.resume);
    CHECK(l.out == "o");
    CHECK(l.extra == std::vector<std::string>{"--unwind", "5", "--pir-drafts", "--jobs", "4"});
    CHECK(has(l.ignored, "--version"));
    CHECK(has(l.ignored, "--pir-vcs x.c"));
    CHECK(has(l.ignored, "other (second path)"));

    auto g = pg::parse_launch({"--gui"});
    CHECK(g.from_cli);
    auto dangling = pg::parse_launch({"src", "--stage"});
    CHECK(has(dangling.ignored, "--stage (no value)"));
    CHECK(pg::flag_takes_value("--out"));
    CHECK(pg::flag_takes_value("-j"));
    CHECK_FALSE(pg::flag_takes_value("--allow-exec"));
}

TEST_CASE("prism --gui forwards every argument but --gui") {
    auto a = pg::gui_forward_args("/opt/prism_gui", {"src", "--gui", "--out", "o", "--no-llm"});
    CHECK(a == std::vector<std::string>{"/opt/prism_gui", "src", "--out", "o", "--no-llm"});
    CHECK(pg::gui_forward_args("g", {"--gui"}) == std::vector<std::string>{"g"});
}

TEST_CASE("prism --gui: missing prism_gui is NOTRUN (exit 0), a spawn failure is ERROR") {
    const auto t = pg::notrun_missing_gui_text();
    CHECK(t.find("NOTRUN gui: prism_gui not found — not a clean window") != std::string::npos);
    CHECK(t.find("install: build prism_gui with WSL clang++ Qt6 Widgets (never MinGW)") !=
          std::string::npos);
    CHECK(t.find(laws::CLEAN) == std::string::npos);
    const auto e = pg::error_spawn_gui_text("execv: No such file");
    CHECK(e.find("ERROR gui: failed to spawn prism_gui — not a clean window") != std::string::npos);
    CHECK(e.find("execv: No such file") != std::string::npos);
    CHECK(e.find(laws::CLEAN) == std::string::npos);
#ifdef _WIN32
    CHECK(pg::gui_binary_name() == "prism_gui.exe");
#else
    CHECK(pg::gui_binary_name() == "prism_gui");
#endif
}

#ifndef _WIN32
TEST_CASE("find_prism_gui: beside the binary first, then PATH; not executable is not found") {
    auto root = temp_dir("find");
    auto self = root / "self";
    auto p1 = root / "p1";
    auto p2 = root / "p2";
    for (auto& d : {self, p1, p2}) fs::create_directories(d);
    const std::string path_env = p1.string() + ":" + p2.string();
    CHECK(pg::find_prism_gui({self}, path_env).empty());
    make_file(p1 / "prism_gui", false);  // not executable
    CHECK(pg::find_prism_gui({self}, path_env).empty());
    make_file(p2 / "prism_gui", true);
    CHECK(pg::find_prism_gui({self}, path_env) == p2 / "prism_gui");
    make_file(self / "prism_gui", true);
    CHECK(pg::find_prism_gui({fs::path(), self}, path_env) == self / "prism_gui");
    CHECK(pg::find_prism_gui({}, "") .empty());
    fs::remove_all(root);
}

TEST_CASE("prism --gui without prism_gui: NOTRUN, exit 0, the pipeline never runs") {
    const auto prism = self_dir() / "prism";
    if (!fs::exists(prism)) {
        MESSAGE("NOTRUN: prism binary not beside prism_tests");
        return;
    }
    auto root = temp_dir("cli");
    fs::copy_file(prism, root / "prism");
    ::chmod((root / "prism").c_str(), 0755);
    fs::create_directories(root / "src");
    std::ofstream(root / "src" / "a.c") << "int f(int a, int b) { return a / b; }\n";
    auto r = run_shell("cd '" + root.string() + "' && PATH=/nonexistent ./prism src --gui --out out");
    CHECK(r.code == 0);
    CHECK(r.out.find("NOTRUN gui: prism_gui not found — not a clean window") != std::string::npos);
    CHECK(r.out.find("confidence") == std::string::npos);
    CHECK(r.out.find(" CLEAN") == std::string::npos);
    CHECK_FALSE(fs::exists(root / "out" / "report.json"));

    // Beside the binary: the forwarded argv reaches prism_gui without --gui.
    std::ofstream(root / "prism_gui") << "#!/bin/sh\nprintf '%s|' \"$@\"\nexit 7\n";
    ::chmod((root / "prism_gui").c_str(), 0755);
    r = run_shell("cd '" + root.string() + "' && PATH=/nonexistent ./prism src --gui --out out --no-llm");
    CHECK(r.code == 7);  // prism_gui's own exit code
    CHECK(r.out == "src|--out|out|--no-llm|");
    CHECK_FALSE(fs::exists(root / "out" / "report.json"));

    // Found but cannot be started (bad interpreter): ERROR, exit 2, no scan.
    std::ofstream(root / "prism_gui") << "#!/nonexistent/interpreter\n";
    ::chmod((root / "prism_gui").c_str(), 0755);
    r = run_shell("cd '" + root.string() + "' && PATH=/nonexistent ./prism src --gui --out out");
    CHECK(r.code == 2);
    CHECK(r.out.find("ERROR gui: failed to spawn prism_gui — not a clean window") != std::string::npos);
    CHECK(r.out.find("execv:") != std::string::npos);
    CHECK(r.out.find("confidence") == std::string::npos);
    CHECK_FALSE(fs::exists(root / "out" / "report.json"));

    auto help = run_shell("'" + prism.string() + "' --help");
    CHECK(help.code == 0);
    CHECK(help.out.find("--gui") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("prism_gui without a display: NOTRUN, exit 0, no window") {
    const auto gui = self_dir() / "prism_gui";
    if (!fs::exists(gui)) {
        MESSAGE("NOTRUN: prism_gui not built (PRISM_QT=OFF or Qt 6 Widgets missing)");
        return;
    }
    auto r = run_shell("env -u DISPLAY -u WAYLAND_DISPLAY -u QT_QPA_PLATFORM '" + gui.string() + "'");
    CHECK(r.code == 0);
    CHECK(r.out.find("NOTRUN gui: no display — not a clean window") != std::string::npos);
    CHECK(r.out.find("QT_QPA_PLATFORM=offscreen") != std::string::npos);
    CHECK(r.out.find(" CLEAN") == std::string::npos);
}

TEST_CASE("prism_gui smoke: an empty findings table is a failure (exit 3), never a pass") {
    const auto gui = self_dir() / "prism_gui";
    if (!fs::exists(gui) || !fs::exists(self_dir() / "prism")) {
        MESSAGE("NOTRUN: prism_gui or prism not built");
        return;
    }
    auto root = temp_dir("smoke");
    fs::create_directories(root / "empty");
    // --smoke-screenshot is taken out of argv before the window parses it,
    // so it is never mistaken for the scan path or a CLI flag.
    auto r = run_shell("cd '" + root.string() + "' && QT_QPA_PLATFORM=offscreen '" + gui.string() +
                       "' empty --no-llm --stage inventory --out out --smoke-screenshot shot.png");
    CHECK(r.code == 3);
    CHECK(r.out.find("gui smoke: 0 finding rows") != std::string::npos);
    CHECK(fs::exists(root / "out" / "report.json"));
    fs::remove_all(root);
}
#endif

TEST_CASE("prism main: --gui returns before run_pipeline (source contract)") {
    const auto main_cpp = slurp(repo_root() / "src" / "prism" / "main.cpp");
    CHECK(main_cpp.find("if (std::strcmp(argv[i], \"--gui\") == 0) return launch_gui(argc, argv);") !=
          std::string::npos);
    auto helpers = main_cpp.substr(0, main_cpp.find("int main("));
    CHECK(helpers.find("run_pipeline") == std::string::npos);
    const auto launch = main_cpp.substr(main_cpp.find("launch_gui"));
    CHECK(launch.find("notrun_missing_gui") != std::string::npos);
    CHECK(launch.find("spawn_prism_gui") != std::string::npos);
    CHECK(launch.find("run_pipeline") == std::string::npos);
    const auto cli = slurp(repo_root() / "src" / "prism" / "cli.cpp");
    CHECK(cli.find("a == \"--gui\"") != std::string::npos);
    CHECK(cli.find("cfg.gui = true") != std::string::npos);
}

TEST_CASE("CI runs the headless window smoke") {
    const auto ci = slurp(repo_root() / ".github" / "workflows" / "ci.yml");
    REQUIRE_FALSE(ci.empty());
    CHECK(ci.find("-DPRISM_QT=ON") != std::string::npos);
    CHECK(ci.find("--smoke-screenshot") != std::string::npos);
    CHECK(ci.find("QT_QPA_PLATFORM=offscreen") != std::string::npos);
    CHECK(ci.find("prism_gui_tests") != std::string::npos);
}

}  // TEST_SUITE
