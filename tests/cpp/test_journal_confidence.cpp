// Doctests for the stage journal (stages.jsonl, progress.json,
// functions.json and --resume) and for the confidence product (Law 5:
// visibility x answer x resolution; a scope with no data scores 0, not n/a).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path fresh_dir(const std::string& tag) {
    static int seq = 0;
    auto p = fs::temp_directory_path() / ("prism_jc_" + tag + "_" + std::to_string(++seq));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

prism::StageResult stage(const std::string& name, const std::string& status, int records = 0) {
    prism::StageResult s;
    s.name = name;
    s.status = status;
    s.records = records;
    return s;
}

// A tree with one C function, add, and a Config that runs only `stages`.
struct Tree {
    fs::path dir, root, out;
    explicit Tree(const std::string& tag) {
        dir = fresh_dir(tag);
        root = dir / "src";
        out = dir / "out";
        fs::create_directories(root);
        std::ofstream(root / "add.c", std::ios::binary) << "int add(int x) { return x; }\n";
    }
    prism::Config cfg(std::vector<std::string> stages, bool resume = false) const {
        auto c = prism::default_config();
        c.root = root;
        c.out = out;
        c.llm = false;
        c.unwind = 2;
        c.fuzz_budget = 0.1;
        c.fuzz_iters = 1;
        c.repair_rounds = 1;
        c.stages = std::move(stages);
        c.skip.clear();
        c.resume = resume;
        return c;
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

const prism::StageResult& stage_of(const prism::RunReport& r, const std::string& name) {
    auto it = std::find_if(r.stages.begin(), r.stages.end(), [&](auto& s) { return s.name == name; });
    REQUIRE(it != r.stages.end());
    return *it;
}

bool has_fn(const prism::RunReport& r, const std::string& name) {
    return std::any_of(r.functions.begin(), r.functions.end(), [&](auto& f) { return f.name == name; });
}

bool note_has(const prism::RunReport& r, const std::string& needle) {
    return std::any_of(r.notes.begin(), r.notes.end(),
                       [&](auto& n) { return n.find(needle) != std::string::npos; });
}

}  // namespace

// ---- journal

TEST_CASE("journal: a corrupt line is skipped, the rest is read") {
    auto out = fresh_dir("corrupt");
    std::ofstream(out / prism::STAGES_JSONL, std::ios::binary)
        << "{not json\n{\"name\":\"lints\",\"status\":\"ok\",\"records\":1}\n";
    auto recs = prism::journal_read_stages(out);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].name == "lints");
    CHECK(recs[0].records == 1);
}

TEST_CASE("journal: one malformed function entry discards the list, never a partial one") {
    auto out = fresh_dir("badfn");
    std::ofstream(out / prism::FUNCTIONS_JSON, std::ios::binary)
        << "[\"not an object\", {\"file\":\"a.c\",\"name\":\"bad\",\"line\":\"x\"},"
           " {\"file\":\"a.c\",\"name\":\"add\",\"kind\":\"SCALAR\",\"line\":1,"
           "\"params\":[[\"int\",\"x\"]],\"body\":\"return x;\"}]";
    std::size_t malformed = 0;
    // A partial list would let --resume skip classify and drop functions.
    CHECK(prism::journal_read_functions(out, &malformed).empty());
    CHECK(malformed == 2);
    // A well-formed list comes back whole.
    std::ofstream(out / prism::FUNCTIONS_JSON, std::ios::binary)
        << "[{\"file\":\"a.c\",\"name\":\"add\",\"kind\":\"SCALAR\",\"line\":1,"
           "\"params\":[[\"int\",\"x\"]],\"body\":\"return x;\"}]";
    auto fns = prism::journal_read_functions(out, &malformed);
    CHECK(malformed == 0);
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].name == "add");
    CHECK(fns[0].body == "return x;");
    // A file that is not a list at all is no functions.
    std::ofstream(out / prism::FUNCTIONS_JSON, std::ios::binary) << "{\"name\":\"add\"}";
    CHECK(prism::journal_read_functions(out).empty());
}

TEST_CASE("journal: a malformed functions.json entry reruns classify on resume, with a note") {
    Tree t("resume_badfn");
    std::ofstream(t.root / "sub.c", std::ios::binary) << "int sub(int x) { return -x; }\n";
    auto first = prism::run_pipeline(t.cfg({"inventory", "classify"}));
    REQUIRE(has_fn(first, "add"));
    REQUIRE(has_fn(first, "sub"));
    // Corrupt one entry, as a functions.json from another version might.
    auto j = nlohmann::json::parse(slurp(t.out / prism::FUNCTIONS_JSON));
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 2);
    j[0]["line"] = "not a number";
    std::ofstream(t.out / prism::FUNCTIONS_JSON, std::ios::binary) << j.dump();

    auto second = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(note_has(second, "1 malformed entries; classify rerun"));
    CHECK(has_fn(second, "add"));
    CHECK(has_fn(second, "sub"));
    auto& cls = stage_of(second, "classify");
    CHECK(cls.status == "ok");
    CHECK(cls.records == stage_of(first, "classify").records);
}

TEST_CASE("journal: progress.json is one line naming the last stage") {
    auto out = fresh_dir("progress");
    auto s = stage("bmc", "ok", 7);
    prism::journal_append_stage(out, s);
    auto text = slurp(out / prism::PROGRESS_JSON);
    CHECK(text.find('\n') == std::string::npos);
    auto j = nlohmann::json::parse(text);
    CHECK(j["last"] == "bmc");
    CHECK(j["status"] == "ok");
    CHECK(j["records"] == 7);
    CHECK(j.contains("elapsed"));
}

TEST_CASE("journal: stages_present tells a missing log from a failed one") {
    auto out = fresh_dir("present");
    CHECK_FALSE(prism::journal_stages_present(out));
    prism::journal_reset(out);
    CHECK_FALSE(prism::journal_stages_present(out));
    prism::journal_append_stage(out, stage("inventory", "failed"));
    CHECK(prism::journal_stages_present(out));
    CHECK(prism::journal_completed_ok(out).empty());
}

TEST_CASE("journal: resume from stages.jsonl without report.json keeps classify") {
    Tree t("resume_jsonl");
    auto first = prism::run_pipeline(t.cfg({"inventory", "classify"}));
    CHECK(has_fn(first, "add"));
    REQUIRE(fs::is_regular_file(t.out / "report.json"));
    fs::remove(t.out / "report.json");
    CHECK(fs::is_regular_file(t.out / prism::STAGES_JSONL));
    CHECK(fs::is_regular_file(t.out / prism::FUNCTIONS_JSON));

    auto second = prism::run_pipeline(t.cfg({"inventory", "classify", "lints"}, true));
    CHECK(note_has(second, "stages.jsonl"));
    auto& cls = stage_of(second, "classify");
    CHECK(cls.status == "ok");
    CHECK(has_fn(second, "add"));
    CHECK(cls.records == stage_of(first, "classify").records);
}

TEST_CASE("journal: resume does not skip classify into an empty function list") {
    Tree t("resume_empty");
    fs::create_directories(t.out);
    prism::journal_append_stage(t.out, stage("inventory", "ok", 1));
    prism::journal_append_stage(t.out, stage("classify", "ok", 1));
    auto report = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(has_fn(report, "add"));
    auto& cls = stage_of(report, "classify");
    CHECK(cls.status == "ok");
    CHECK(cls.records >= 1);
}

TEST_CASE("journal: resume reuses a NOTRUN stage but reruns a failed one") {
    {
        Tree t("resume_notrun");
        auto first = prism::run_pipeline(t.cfg({"llm"}));
        auto llm1 = stage_of(first, "llm");
        CHECK(llm1.status == "NOTRUN");
        auto second = prism::run_pipeline(t.cfg({"llm"}, true));
        CHECK(note_has(second, "resumed"));
        auto& llm2 = stage_of(second, "llm");
        CHECK(llm2.status == "NOTRUN");
        CHECK(llm2.records == llm1.records);
    }
    {
        Tree t("resume_failed");
        fs::create_directories(t.out);
        prism::journal_append_stage(t.out, stage("inventory", "failed", 0));
        prism::journal_write_functions(t.out, {});
        auto report = prism::run_pipeline(t.cfg({"inventory"}, true));
        auto& inv = stage_of(report, "inventory");
        CHECK(inv.status == "ok");
        CHECK(inv.records >= 1);
    }
}

TEST_CASE("journal: without stages.jsonl, resume falls back to report.json") {
    Tree t("resume_report");
    auto first = prism::run_pipeline(t.cfg({"inventory", "classify"}));
    REQUIRE(fs::is_regular_file(t.out / prism::STAGES_JSONL));
    fs::remove(t.out / prism::STAGES_JSONL);
    REQUIRE(fs::is_regular_file(t.out / "report.json"));
    auto second = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK(note_has(second, "report.json"));
    CHECK(has_fn(second, "add"));
    auto& cls = stage_of(second, "classify");
    CHECK(cls.status == "ok");
    CHECK(cls.records == stage_of(first, "classify").records);
}

TEST_CASE("journal: a failed stages.jsonl does not revive a stale report.json") {
    Tree t("resume_stale");
    fs::create_directories(t.out);
    prism::RunReport stale;
    stale.root = t.root.string();
    prism::FunctionInfo lie;
    lie.file = "stale.c";
    lie.name = "stale_fn";
    lie.kind = "SCALAR";
    lie.line = 1;
    lie.signature = "int stale_fn(void)";
    stale.functions = {lie};
    stale.stages = {stage("inventory", "ok", 99), stage("classify", "ok", 99)};
    stale.save(t.out / "report.json");
    prism::journal_append_stage(t.out, stage("inventory", "failed", 0));
    REQUIRE(prism::journal_stages_present(t.out));
    CHECK(prism::journal_completed_ok(t.out).empty());

    auto report = prism::run_pipeline(t.cfg({"inventory", "classify"}, true));
    CHECK_FALSE(note_has(report, "report.json"));
    auto& inv = stage_of(report, "inventory");
    CHECK(inv.status == "ok");
    CHECK(inv.records != 99);
    CHECK(has_fn(report, "add"));
    CHECK_FALSE(has_fn(report, "stale_fn"));
}

// ---- confidence

namespace {

prism::FunctionInfo cfn(const std::string& name, const std::string& kind = "SCALAR") {
    prism::FunctionInfo f;
    f.file = "a.c";
    f.name = name;
    f.kind = kind;
    f.line = 1;
    f.signature = "int " + name + "()";
    return f;
}

prism::Finding bmc_row(std::string_view status, std::optional<std::string> function = "add",
                       const std::string& cex = "") {
    prism::Finding f;
    f.stage = "bmc";
    f.status = std::string(status);
    f.file = "a.c";
    f.function = std::move(function);
    f.line = 1;
    f.cls = "INT-SIGNED-OVF";
    f.message = f.status;
    f.strength = std::string(prism::laws::STRENGTH_PROVES);
    f.counterexample = cex;
    return f;
}

prism::RunReport report_of(std::vector<prism::FunctionInfo> fns, std::optional<std::vector<prism::Finding>> rows) {
    prism::RunReport r;
    r.root = "x";
    r.functions = std::move(fns);
    if (rows) {
        prism::StageResult s = stage("bmc", "ok", static_cast<int>(rows->size()));
        s.findings = *rows;
        r.stages.push_back(s);
    }
    return r;
}

void check_score(const prism::verdict::Score& s, double v, double a, double r, double c) {
    CHECK(s.visibility == v);
    CHECK(s.answer == a);
    CHECK(s.resolution == r);
    CHECK(s.confidence == c);
}

const char* kEmptyNote = "confidence 0: no functions parsed (no data, not clean)";

}  // namespace

TEST_CASE("confidence: an empty scope is 0 on every factor, even with stray bmc rows") {
    prism::RunReport empty;
    empty.root = "empty";
    check_score(prism::confidence_score(empty), 0, 0, 0, 0);
    check_score(prism::confidence_score(report_of({}, std::vector{bmc_row(prism::laws::PROVED)})), 0, 0, 0, 0);
}

TEST_CASE("confidence: apply zeroes stale fields and writes the note once, never n/a") {
    prism::RunReport r;
    r.root = "empty";
    r.visibility = r.answer = r.resolution = r.confidence = 1.0;
    prism::apply_confidence(r);
    CHECK(r.visibility == 0.0);
    CHECK(r.answer == 0.0);
    CHECK(r.resolution == 0.0);
    CHECK(r.confidence == 0.0);
    CHECK(r.notes == std::vector<std::string>{kEmptyNote});
    prism::apply_confidence(r);
    CHECK(std::count(r.notes.begin(), r.notes.end(), kEmptyNote) == 1);
    std::string blob;
    for (auto& n : r.notes) blob += n + " ";
    std::string low = blob;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(low.find("n/a") == std::string::npos);
    CHECK(blob.find(prism::laws::CLEAN) == std::string::npos);
}

TEST_CASE("confidence: no bmc stage scores 0") {
    auto r = report_of({cfn("add")}, std::nullopt);
    check_score(prism::confidence_score(r), 1.0, 0.0, 0.0, 0.0);
    prism::apply_confidence(r);
    CHECK(r.confidence == 0.0);
}

TEST_CASE("confidence: every function NEEDS-HARNESS is an empty answer scope, 0") {
    auto r = report_of({cfn("walk", "POINTER")}, std::vector{bmc_row(prism::laws::NEEDS_HARNESS, "walk")});
    check_score(prism::confidence_score(r), 1.0, 0.0, 0.0, 0.0);
}

TEST_CASE("confidence: a row without a function name does not answer for add") {
    check_score(prism::confidence_score(report_of({cfn("add")}, std::vector{bmc_row(prism::laws::PROVED, std::nullopt)})),
                1.0, 0.0, 0.0, 0.0);
    auto r2 = report_of({cfn("add")}, std::vector{bmc_row(prism::laws::PROVED, "add")});
    check_score(prism::confidence_score(r2), 1.0, 1.0, 1.0, 1.0);
    prism::apply_confidence(r2);
    CHECK(r2.confidence == 1.0);
    CHECK(r2.notes.empty());
}

TEST_CASE("confidence: FAILED resolves only with a counterexample or a non-empty oracle") {
    check_score(prism::confidence_score(report_of({cfn("add")}, std::vector{bmc_row(prism::laws::FAILED, "add", "x=1")})),
                1.0, 1.0, 1.0, 1.0);
    auto no_cex = prism::confidence_score(report_of({cfn("add")}, std::vector{bmc_row(prism::laws::FAILED)}));
    CHECK(no_cex.answer == 1.0);
    CHECK(no_cex.resolution == 0.0);
    CHECK(no_cex.confidence == 0.0);
    // An oracle/read key with an empty value is not an answer.
    auto empty_oracle = bmc_row(prism::laws::FAILED);
    empty_oracle.extra["oracle"] = "";
    empty_oracle.extra["read"] = "";
    CHECK(prism::confidence_score(report_of({cfn("add")}, std::vector{empty_oracle})).resolution == 0.0);
    auto oracle = bmc_row(prism::laws::FAILED);
    oracle.extra["oracle"] = "x=1 divides by zero";
    CHECK(prism::confidence_score(report_of({cfn("add")}, std::vector{oracle})).resolution == 1.0);
}

TEST_CASE("confidence: scores round to 4 decimals, ties to even on the exact value") {
    CHECK(prism::round4(1.0 / 3.0) == 0.3333);
    CHECK(prism::round4(2.0 / 3.0) == 0.6667);
    CHECK(prism::round4(0.0) == 0.0);
    CHECK(prism::round4(1.0) == 1.0);
    // 0.00005 is stored just above 5e-5 and 0.00015 just below 1.5e-4: the
    // exact binary value decides, not a scaled product.
    CHECK(prism::round4(0.00005) == 0.0001);
    CHECK(prism::round4(0.00015) == 0.0001);
    // 1/3 of the functions answered, through apply.
    auto r = report_of({cfn("add"), cfn("sub"), cfn("mul")}, std::vector{bmc_row(prism::laws::PROVED, "add")});
    prism::apply_confidence(r);
    CHECK(r.answer == 0.3333);
    CHECK(r.confidence == 0.3333);
}

TEST_CASE("confidence: an exact 4th-decimal tie rounds half to even") {
    // 1/32 is 0.03125 exactly, a tie at the 4th decimal. std::round(x*1e4)/1e4
    // is half away from zero and publishes 0.0313. The published value follows
    // Python round(x, 4): half to even on the exact decimal of the double, 0.0312.
    // The literal 1.00005 is not that tie. It is stored slightly above 1.00005,
    // so both rules publish 1.0001. 3.5/10000 is a double just under 0.00035
    // whose scaled product lands on 3.5; std::round publishes 0.0004 and the
    // exact decimal publishes 0.0003.
    const double tie = 1.0 / 32.0;
    CHECK(tie == 0.03125);
    CHECK(std::round(tie * 10000.0) / 10000.0 == 0.0313);
    CHECK(prism::round4(tie) == 0.0312);
    CHECK(prism::round4(1.00005) == 1.0001);
    const double under = 3.5 / 10000.0;
    CHECK(under * 10000.0 == 3.5);
    CHECK(std::round(under * 10000.0) / 10000.0 == 0.0004);
    CHECK(prism::round4(under) == 0.0003);

    std::vector<prism::FunctionInfo> fns;
    fns.reserve(32);
    for (int i = 0; i < 32; ++i) fns.push_back(cfn("f" + std::to_string(i)));
    auto r = report_of(std::move(fns), std::vector{bmc_row(prism::laws::PROVED, "f0")});
    auto raw = prism::confidence_score(r);
    CHECK(raw.visibility == 1.0);
    CHECK(raw.answer == doctest::Approx(1.0 / 32.0));
    CHECK(raw.resolution == 1.0);
    CHECK(raw.confidence == doctest::Approx(1.0 / 32.0));
    prism::apply_confidence(r);
    CHECK(r.visibility == 1.0);
    CHECK(r.answer == 0.0312);
    CHECK(r.resolution == 1.0);
    CHECK(r.confidence == 0.0312);
}

TEST_CASE("confidence: the empty-scope report.md shows 0 and never n/a") {
    prism::RunReport r;
    r.root = "empty";
    prism::apply_confidence(r);
    CHECK(r.confidence == 0.0);
    auto dir = fresh_dir("md");
    prism::write_report_md(r, dir / "report.md");
    auto text = slurp(dir / "report.md");
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(text.find("**0**") != std::string::npos);
    CHECK(text.find("n/a") == std::string::npos);
}

// ---- functions.json: a partial list is not a successful read

namespace {

void check_roundtrip_fields(const prism::FunctionInfo& got, const prism::FunctionInfo& want) {
    CHECK(got.file == want.file);
    CHECK(got.name == want.name);
    CHECK(got.kind == want.kind);
    CHECK(got.line == want.line);
    CHECK(got.signature == want.signature);
    CHECK(got.params == want.params);
    CHECK(got.return_type == want.return_type);
    CHECK(got.is_static == want.is_static);
    CHECK(got.body == want.body);
    CHECK(got.span == want.span);
    CHECK(got.body_line == want.body_line);
    CHECK(got.body_col == want.body_col);
}

prism::FunctionInfo journal_fn(const std::string& name, int line, const std::string& body, bool is_static) {
    auto f = cfn(name);
    f.line = line;
    f.signature = "int " + name + "(int x)";
    f.params.emplace_back("int", "x");
    f.return_type = "int";
    f.is_static = is_static;
    f.body = body;
    f.span = {line, line + 30};
    f.body_line = line;
    f.body_col = 12;
    return f;
}

}  // namespace

TEST_CASE("journal: a file of only valid functions is returned") {
    auto out = fresh_dir("fns_ok");
    auto add = journal_fn("add", 4, "return x;", false);
    auto sub = journal_fn("sub", 8, "return -x;", true);
    prism::journal_write_functions(out, {add, sub});
    std::size_t malformed = 7;
    auto fns = prism::journal_read_functions(out, &malformed);
    CHECK(malformed == 0);
    REQUIRE(fns.size() == 2);
    check_roundtrip_fields(fns[0], add);
    check_roundtrip_fields(fns[1], sub);
}

TEST_CASE("journal: one valid function and one non-object is an empty read") {
    auto out = fresh_dir("fns_nonobj");
    auto add = journal_fn("add", 4, "return x;", false);
    prism::journal_write_functions(out, {add});
    std::size_t malformed = 7;
    auto before = prism::journal_read_functions(out, &malformed);
    CHECK(malformed == 0);
    REQUIRE(before.size() == 1);
    check_roundtrip_fields(before[0], add);

    // Append one non-object beside that valid entry.
    auto j = nlohmann::json::parse(slurp(out / prism::FUNCTIONS_JSON));
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 1);
    REQUIRE(j[0].is_object());
    j.push_back(nlohmann::json("not an object"));
    {
        std::ofstream f(out / prism::FUNCTIONS_JSON, std::ios::binary);
        REQUIRE(f);
        f << j.dump();
    }
    malformed = 0;
    auto fns = prism::journal_read_functions(out, &malformed);
    // A partial list would let --resume skip classify and drop this function.
    CHECK(fns.empty());
    CHECK(malformed > 0);
    CHECK(malformed == 1);
}
