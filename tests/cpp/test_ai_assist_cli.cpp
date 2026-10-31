// Roadmap 9.3 / 9.4 assistant features end to end through their command-line
// entry points (`prism triage | ask | draft | regress`, called in process),
// with a fake llama-server on a local socket for the model paths:
//
//   * the pipeline writes triage.json and a "Clusters" section without
//     changing a status; `prism triage` leaves report.json byte-identical;
//   * `prism ask` prints the structured query with the answer (grammar without
//     a model); a model's grammar-constrained query is validated and audited;
//   * `prism draft` links every claim; a model's unlinked claim is rejected;
//   * `prism regress` writes tests under <out>/regression_tests only, and
//     --run without --allow-exec runs nothing (Law 9);
//   * triage uses the embedding endpoint when PRISM_EMBED_SERVER is set;
//   * the Qt GUI's assistant panel keeps its contract (Qt is not built here,
//     so its source is checked) and assistant_reply answers it.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/ai.hpp"
#include "prism/ai_assist.hpp"
#include "prism/config.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

#ifdef PRISM_HAS_Z3
constexpr bool kHasBmc = true;
#else
constexpr bool kHasBmc = false;  // Law 1: no Z3, no BMC verdicts
#endif

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Sets environment variables for one scope and restores them after.
struct EnvScope {
    std::map<std::string, std::optional<std::string>> saved;
    // MSVC has no setenv/unsetenv; _putenv_s with an empty value removes the variable.
    static void put(const std::string& k, const std::optional<std::string>& v) {
#ifdef _WIN32
        _putenv_s(k.c_str(), v ? v->c_str() : "");
#else
        if (v) ::setenv(k.c_str(), v->c_str(), 1);
        else ::unsetenv(k.c_str());
#endif
    }
    void set(const std::string& k, const std::optional<std::string>& v) {
        if (!saved.count(k)) {
            const char* old = std::getenv(k.c_str());
            saved[k] = old ? std::optional<std::string>(old) : std::nullopt;
        }
        put(k, v);
    }
    ~EnvScope() {
        for (auto& [k, v] : saved) {
            put(k, v);
        }
    }
};

struct CliResult {
    int rc = -1;
    std::string out;
};

// Runs a subcommand entry point with std::cout captured.
CliResult run_cli(int (*fn)(int, char**), std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream cap;
    auto* old = std::cout.rdbuf(cap.rdbuf());
    CliResult r;
    try {
        r.rc = fn(static_cast<int>(argv.size()), argv.data());
    } catch (...) {
        std::cout.rdbuf(old);
        throw;
    }
    std::cout.rdbuf(old);
    r.out = cap.str();
    return r;
}


constexpr const char* kCalc = R"(int add_big(int x) {
    return x + 100;
}

int divide(int a, int b) {
    return a / b;
}

int ok(int x) {
    return x & 1;
}
)";

// One pipeline run over calc.c (inventory, classify, bmc; no model) shared by
// the cases below; the model endpoints point at a closed port.
struct AssistRun {
    fs::path td, src, out;
    std::string report_text;
    EnvScope env;
    AssistRun() {
        td = fs::temp_directory_path() / "prism-assist-cli";
        std::error_code ec;
        fs::remove_all(td, ec);
        src = td / "src";
        out = td / "out";
        fs::create_directories(src);
        std::ofstream(src / "calc.c") << kCalc;
        env.set("PRISM_EMBED_FPK", std::nullopt);
        prism::Config cfg = prism::default_config();
        cfg.root = src;
        cfg.out = out;
        cfg.llm = false;
        cfg.stages = std::vector<std::string>{"inventory", "classify", "bmc"};
        prism::run_pipeline(cfg);
        report_text = slurp(out / "report.json");
    }
};

}  // namespace

TEST_CASE("ai-assist cli: the pipeline's triage orders only; `prism triage` keeps report.json") {
    if (!kHasBmc) {
        MESSAGE("NOTRUN: PRISM_HAS_Z3 off: the pipeline's bmc stage gives no FAILED rows");
        return;
    }
    AssistRun run;
    REQUIRE(!run.report_text.empty());
    const auto tri = json::parse(slurp(run.out / "triage.json"));
    CHECK(tri["kind"] == "prism-triage");
    CHECK(tri["embedder_note"].get<std::string>().find("NOTRUN") != std::string::npos);
    const auto md = slurp(run.out / "report.md");
    REQUIRE(md.find("## Findings") != std::string::npos);
    REQUIRE(md.find("## Clusters") != std::string::npos);
    CHECK(md.find("## Findings") < md.find("## Clusters"));
    const auto rep = json::parse(run.report_text);
    std::map<std::string, std::string> statuses;
    for (const auto& s : rep["stages"]) {
        std::size_t i = 0;
        for (const auto& f : s["findings"])
            statuses[s["name"].get<std::string>() + "#" + std::to_string(i++)] = f["status"].get<std::string>();
    }
    REQUIRE(!tri["clusters"].empty());
    for (const auto& c : tri["clusters"]) CHECK(c["top_status"] == statuses.at(c["representative"].get<std::string>()));
    auto r = run_cli(prism::ai::triage_main, {run.out.string()});
    CHECK(r.rc == 0);
    CHECK(slurp(run.out / "report.json") == run.report_text);
    CHECK(run_cli(prism::ai::triage_main, {(run.td / "nowhere").string()}).rc == 2);
}

TEST_CASE("ai-assist cli: `prism ask` prints the query with the answer") {
    if (!kHasBmc) {
        MESSAGE("NOTRUN: PRISM_HAS_Z3 off: the pipeline's bmc stage gives no FAILED rows");
        return;
    }
    AssistRun run;
    auto r = run_cli(prism::ai::ask_main,
                     {"failed findings in function divide", "--report", run.out.string(), "--no-llm"});
    CHECK(r.rc == 0);
    CAPTURE(r.out);
    CHECK(r.out.find("query (grammar): {") != std::string::npos);
    CHECK(r.out.find("\"function_glob\":\"divide\"") != std::string::npos);
    CHECK(r.out.find("answer: 1 finding") != std::string::npos);
    auto j = json::parse(
        run_cli(prism::ai::ask_main, {"how many failed", "--report", run.out.string(), "--json", "--no-llm"}).out);
    CHECK(j["translator"] == "grammar");
    CHECK(j["query"]["count_only"] == true);
    CHECK(j["count"].get<int>() >= 2);
    auto e = run_cli(prism::ai::ask_main, {"explain", "bmc#0", "--report", run.out.string(), "--no-llm"});
    CHECK(e.out.find("what") != std::string::npos);
}

#ifndef _WIN32

#endif

TEST_CASE("ai-assist cli: `prism regress` writes under <out> only; --run without --allow-exec runs nothing") {
    if (!kHasBmc) {
        MESSAGE("NOTRUN: PRISM_HAS_Z3 off: the pipeline's bmc stage gives no FAILED rows");
        return;
    }
    AssistRun run;
    auto r = run_cli(prism::ai::regress_main, {"--report", (run.out / "report.json").string()});
    CHECK(r.rc == 0);
    auto m = json::parse(slurp(run.out / "regression_tests" / "manifest.json"));
    std::set<std::string> written;
    for (const auto& t : m["tests"])
        if (t["status"] == "written") written.insert(t["function"].get<std::string>());
    CHECK(written == std::set<std::string>{"add_big", "divide"});
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(run.src)) names.push_back(e.path().filename().string());
    CHECK(names == std::vector<std::string>{"calc.c"});
    run_cli(prism::ai::regress_main, {"--report", (run.out / "report.json").string(), "--run"});
    m = json::parse(slurp(run.out / "regression_tests" / "manifest.json"));
    for (const auto& t : m["tests"])
        if (t.contains("name") && !t["name"].get<std::string>().empty()) CHECK(t["status"] == "NOTRUN");
}

TEST_CASE("ai-assist cli: the assurance template draft links every claim to the report or a theorem") {
    AssistRun run;
    auto r = run_cli(prism::ai::draft_main, {"--report", run.out.string(), "--kind", "assurance", "--proofs",
                                             (repo_root() / "proofs").string(), "--no-llm"});
    CHECK(r.rc == 0);
    auto d = json::parse(slurp(run.out / "draft_assurance.json"));
    CHECK(d["rejected"].empty());
    for (const auto& s : d["sections"])
        for (const auto& c : s["claims"]) CHECK(!c["links"].empty());
    CHECK(d["theorems_indexed"].get<int>() > 20);
    CHECK(slurp(run.out / "draft_assurance.md").find("[theorem:Prism.proved_bounded_never_merge]") != std::string::npos);
}

TEST_CASE("ai-assist gui: the Qt assistant panel keeps its contract; assistant_reply answers it") {
    // Qt is not built in every configuration: the panel's source contract.
    const auto cpp = slurp(repo_root() / "src" / "gui" / "MainWindow.cpp");
    const auto hdr = slurp(repo_root() / "src" / "gui" / "MainWindow.hpp");
    REQUIRE(!cpp.empty());
    CHECK(hdr.find("void onAsk();") != std::string::npos);
    CHECK(cpp.find("prism::ai::assistant_reply") != std::string::npos);
    const auto on_ask = cpp.find("void MainWindow::onAsk");
    REQUIRE(on_ask != std::string::npos);
    CHECK(cpp.find("cfg.resume = true;", on_ask) != std::string::npos);
    CHECK(cpp.find("NOTRUN assistant") != std::string::npos);
    CHECK(cpp.find("cellDoubleClicked") != std::string::npos);
    // What the panel shows for a question: the structured query and the findings.
    const auto dir = fs::temp_directory_path() / "prism-assist-gui";
    fs::create_directories(dir);
    std::ofstream(dir / "report.json", std::ios::binary) << R"({"root": "x", "stages": [{"name": "bmc", "status": "ok", "findings": [
        {"stage": "bmc", "status": "FAILED", "file": "a.c", "function": "f", "line": 1, "cls": "INT-DIV-ZERO",
         "message": "div0", "strength": "PROVES", "evidence": "", "counterexample": "x=0", "extra": {}}]}]})";
    auto rep = prism::RunReport::load(dir / "report.json");
    REQUIRE(rep);
    const auto reply = prism::ai::assistant_reply(*rep, "failed division", false);
    CAPTURE(reply);
    CHECK(reply.find("query (grammar)") != std::string::npos);
    CHECK(reply.find("bmc#0 FAILED a.c:1") != std::string::npos);
}

// ---- FORGE-era replacements for the llama-server fakes (in-process model seams) ----
namespace {
// The assistant model as an injected ai::ModelBackend: answers by the GBNF it is
// constrained with (the ask grammar has "statuses", the draft grammar "links").
class ScriptedModel final : public prism::ai::ModelBackend {
public:
    std::string name() const override { return "forge:test"; }
    std::string model_sha256() const override { return "unknown"; }
    prism::ai::ModelReply complete(const prism::ai::ModelRequest& req) override {
        ++calls;
        if (req.grammar_text.find("statuses") != std::string::npos)
            return {json{{"stages", {"bmc"}}, {"statuses", {"FAILED"}}, {"cls", {"DIV"}}, {"file_glob", ""},
                         {"function_glob", ""}, {"text", ""}, {"group_by", ""}, {"count_only", false}}
                        .dump(),
                    ""};
        if (req.grammar_text.find("links") != std::string::npos)
            return {json::array({{{"section", "Defects"},
                                  {"text", "Dividing by a zero parameter is undefined."},
                                  {"links", {"verdict:bmc#1"}}},
                                 {{"section", "Defects"}, {"text", "Everything else is proved."}, {"links", {"stage:bmc"}}}})
                        .dump(),
                    ""};
        return {"{}", ""};
    }
    int calls = 0;
};
struct ModelScope {
    std::shared_ptr<ScriptedModel> m = std::make_shared<ScriptedModel>();
    ModelScope() { prism::ai::set_session_backend_for_testing(m); }
    ~ModelScope() { prism::ai::set_session_backend_for_testing(nullptr); }
};
class ScriptedEmbedder final : public prism::ai::Embedder {
public:
    std::string name() const override { return "forge:test-embed.fpk"; }
    std::optional<std::vector<double>> embed(const std::string& text) override {
        ++calls;
        return text.find("div") != std::string::npos ? std::vector<double>{1.0, 0.0} : std::vector<double>{0.0, 1.0};
    }
    int calls = 0;
};
}  // namespace

TEST_CASE("ai-assist cli: a model's query is validated and audited; its unlinked draft claim is rejected") {
    if (!kHasBmc) {
        MESSAGE("NOTRUN: PRISM_HAS_Z3 off: no FAILED rows to query");
        return;
    }
    AssistRun run;
    ModelScope model;
    auto a = run_cli(prism::ai::ask_main,
                     {"division problems from model checking", "--report", run.out.string(), "--json"});
    CAPTURE(a.out);
    auto j = json::parse(a.out);
    CHECK(j["translator"].get<std::string>().rfind("llm:", 0) == 0);
    std::vector<std::string> fns;
    for (const auto& m : j["matches"]) fns.push_back(m["function"].get<std::string>());
    CHECK(fns == std::vector<std::string>{"divide"});
    std::vector<json> ask;
    std::istringstream al(slurp(run.out / "ai_audit.jsonl"));
    for (std::string l; std::getline(al, l);)
        if (l.find_first_not_of(" \t\r") != std::string::npos) {
            auto x = json::parse(l);
            if (x["feature"] == "ask") ask.push_back(x);
        }
    REQUIRE(!ask.empty());
    CHECK(ask.back()["checker_result"] == "accepted");
    CHECK(ask.back()["verdict_effect"] == "none");
    auto d = run_cli(prism::ai::draft_main, {"--report", run.out.string(), "--out", (run.out / "d.md").string()});
    CHECK(d.rc == 0);
    auto dj = json::parse(slurp(run.out / "d.json"));
    CHECK(dj["author"].get<std::string>().rfind("llm:", 0) == 0);
    CHECK(dj["rejected"].size() == 1);
    const auto dmd = slurp(run.out / "d.md");
    CHECK(dmd.substr(0, dmd.find("## Rejected")).find("Everything else is proved") == std::string::npos);
    CHECK(slurp(run.out / "report.json") == run.report_text);
    CHECK(model.m->calls >= 2);
}

TEST_CASE("ai-assist cli: triage uses the FORGE embedder when one is configured") {
    if (!kHasBmc) {
        MESSAGE("NOTRUN: PRISM_HAS_Z3 off: no findings to embed");
        return;
    }
    AssistRun run;
    auto emb = std::make_shared<ScriptedEmbedder>();
    prism::ai::set_embedder_for_testing(emb);
    const auto dir = run.td / "copy";
    fs::create_directories(dir);
    std::ofstream(dir / "report.json", std::ios::binary) << run.report_text;
    auto r = run_cli(prism::ai::triage_main, {dir.string()});
    prism::ai::set_embedder_for_testing(nullptr);
    CHECK(r.rc == 0);
    const auto tri = json::parse(slurp(dir / "triage.json"));
    CHECK(tri["embedder"].get<std::string>().find("forge:") != std::string::npos);
    CHECK(emb->calls >= 1);
}
