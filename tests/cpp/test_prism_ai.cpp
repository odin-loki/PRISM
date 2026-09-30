// The offline AI analytics (roadmap 9.1 / 9.3 / 9.7): the GBDT trainer
// (src/prism/solver/gbdt.cpp), the portfolio replay and the solver / bound
// prediction measurement of prism_ai (src/tools/prism_ai/), the assistant
// metrics, and the built-in model embedded from predict_default.json.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/solver_gbdt.hpp"
#include "prism/solver_predict.hpp"

#include "prism_ai.hpp"
#include "pycompat.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using prism::solver::gbdt::Model;
using ojson = nlohmann::ordered_json;
namespace sched = prism_ai::sched;
namespace predict = prism_ai::predict;

namespace {

fs::path ai_tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-ai-tool-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

// Same bucket for all rows (so per-bucket history cannot separate them); z3
// is fast on small node counts, cadical on large ones.
std::vector<ojson> solver_logs(int n = 300) {
    std::vector<ojson> rows;
    for (int i = 0; i < n; ++i) {
        const double nodes = 1.5 + (i % 30) / 10.0;  // log10 nodes 1.5 .. 4.4
        const bool fast_z3 = nodes < 3.0;
        rows.push_back(ojson{{"file", "f" + std::to_string(i) + ".c"},
                             {"bucket", "QF_BV|w32|n1k"},
                             {"features", {5.0, nodes, 1.0, 0, 0, 0, 0, 0, 0}},
                             {"runs",
                              {{"z3", {{"kind", "unsat"}, {"wall_s", fast_z3 ? 0.05 : 2.0}}},
                               {"cadical", {{"kind", "unsat"}, {"wall_s", fast_z3 ? 1.0 : 0.1}}}}}});
    }
    return rows;
}

sched::Row row(std::vector<std::pair<std::string, sched::Run>> runs, const std::string& key = "a.c", double T = 8.0) {
    sched::Row r;
    r.key = key;
    r.bucket = "b";
    r.x = {5.0, 2.0, 1.0, 0, 0, 0, 0, 0, 0};
    r.T = T;
    r.runs = std::move(runs);
    r.answer = "unsat";
    return r;
}

constexpr auto Ans = sched::State::Ans;
constexpr auto Cens = sched::State::Cens;
constexpr auto Gave = sched::State::Gave;

int argv_call(int (*fn)(int, char**, std::ostream&, std::ostream&), std::vector<std::string> args, std::string* out = nullptr,
              std::string* err = nullptr) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream o, e;
    int rc = fn(static_cast<int>(argv.size()), argv.data(), o, e);
    if (out) *out = o.str();
    if (err) *err = e.str();
    return rc;
}

}  // namespace

// ------------------------------------------------------------------ GBDT trainer
TEST_CASE("prism_ai gbdt: fits a step and an interaction; JSON round trip; the loader's comparison") {
    std::vector<std::vector<double>> xs;
    std::vector<double> ys;
    for (int a = 0; a < 10; ++a)
        for (int b = 0; b < 4; ++b) {
            xs.push_back({static_cast<double>(a), static_cast<double>(b)});
            ys.push_back((a > 4 ? 5.0 : 0.0) + (b >= 2 && a < 3 ? 2.0 : 0.0));
        }
    Model g;
    g.n_trees = 80;
    g.lr = 0.3;
    g.depth = 3;
    g.min_leaf = 1;
    g.fit(xs, ys);
    CHECK(prism::solver::gbdt::rmse(g, xs, ys) < 0.1);
    const auto j = g.to_json();
    const auto g2 = Model::from_json(ojson::parse(j.dump()));
    for (const auto& x : xs) CHECK(g2.predict(x) == doctest::Approx(g.predict(x)));
    // The trainer's JSON is what the engine's loader evaluates.
    for (const auto& x : xs) CHECK(prism::solver::predict::gbdt_eval(j.dump(), x) == doctest::Approx(g.predict(x)));
    // The C++ loader's semantics: left when x[f] <= t.
    const auto node = prism::solver::gbdt::Tree::from_json(ojson::parse(R"({"f":0,"t":1.0,"l":{"v":-1.0},"r":{"v":1.0}})"));
    CHECK(node.eval({1.0}) == -1.0);
    CHECK(node.eval({1.5}) == 1.0);
    CHECK_THROWS(Model().fit({}, {}));
}

TEST_CASE("prism_ai gbdt: depth is bounded") {
    std::vector<std::vector<double>> xs;
    std::vector<double> ys;
    for (int i = 0; i < 64; ++i) {
        xs.push_back({static_cast<double>(i)});
        ys.push_back(i % 7);
    }
    Model g;
    g.n_trees = 3;
    g.depth = 3;
    g.min_leaf = 1;
    g.fit(xs, ys);
    REQUIRE(!g.trees.empty());
    for (const auto& t : g.trees) CHECK(t.depth() <= 3);
}

TEST_CASE("prism_ai gbdt: censored boosting predicts above the censoring point") {
    std::vector<std::vector<double>> xs;
    std::vector<double> ys;
    std::vector<bool> cens;
    for (int i = 0; i < 40; ++i) {
        const double x = i % 2;
        xs.push_back({x});
        // x=1: the solver always times out at log T = 1.0 (true time unknown, >= 1)
        ys.push_back(x != 0 ? 1.0 : 0.0);
        cens.push_back(x != 0);
    }
    Model plain, aft;
    plain.n_trees = aft.n_trees = 30;
    plain.min_leaf = aft.min_leaf = 1;
    plain.fit(xs, ys);
    aft.fit_censored(xs, ys, cens);
    CHECK(plain.predict({1.0}) <= 1.0 + 1e-9);  // squared loss: a timeout is its time
    CHECK(aft.predict({1.0}) > 1.0);             // censored: at least the timeout
    CHECK(std::abs(aft.predict({0.0})) < 0.05);
    CHECK(Model::from_json(aft.to_json()).predict({1.0}) == aft.predict({1.0}));
}

// ------------------------------------------------------------------ solver / bound prediction
TEST_CASE("prism_ai predict: the solver model beats the history and is enabled") {
    auto res = predict::measure_solver(predict::solver_rows(solver_logs()), 30);
    CAPTURE(res.res.dump());
    REQUIRE(res.enabled());
    const auto& pol = res.res["policies"];
    CHECK(pol["gbdt"]["seconds"].get<double>() < pol["history"]["seconds"].get<double>());
    CHECK(pol["gbdt"]["accuracy"].get<double>() >= 0.95);
    auto model = predict::build_model(&res, nullptr);
    CHECK(model["enabled"] == true);
    CHECK(model["query_features"] == ojson(prism::solver::predict::query_feature_names()));
    CHECK(model["function_features"] == ojson(prism::solver::predict::function_feature_names()));
    std::set<std::string> solvers;
    for (auto it = model["solvers"].begin(); it != model["solvers"].end(); ++it) solvers.insert(it.key());
    CHECK(solvers == std::set<std::string>{"z3", "cadical"});
    CHECK_FALSE(model["metrics"]["solver"].contains("models"));
}

TEST_CASE("prism_ai predict: a noise model stays off") {
    auto logs = solver_logs();
    for (auto& r : logs) {  // times independent of the features: nothing to learn
        const auto f = r["file"].get<std::string>();
        const long long i = std::stoll(f.substr(1, f.size() - 3));
        const long long h = i * 2654435761LL % 97;
        r["runs"]["z3"]["wall_s"] = 0.5 + (h % 10) / 100.0;
        r["runs"]["cadical"]["wall_s"] = 0.5 + (h % 7) / 100.0;
    }
    auto res = predict::measure_solver(predict::solver_rows(logs), 30);
    CHECK_FALSE(res.enabled());
    auto model = predict::build_model(&res, nullptr);
    CHECK(model["enabled"] == false);
    CHECK_FALSE(model.contains("solvers"));
}

TEST_CASE("prism_ai predict: a production log is censored, not trained on") {
    const std::vector<ojson> prod = {ojson::parse(
        R"({"hash": "h1", "bucket": "b", "features": [5, 2, 1, 0, 0, 0, 0, 0, 0], "times": {"z3": 0.1},
            "winner": "z3", "raced": ["z3", "cadical"], "wall_s": 0.1})")};
    auto rows = predict::solver_rows(prod);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].times == std::vector<std::pair<std::string, double>>{{"z3", 0.1}});
    CHECK(rows[0].key == "h1");
    auto res = predict::measure_solver(rows);
    CHECK_FALSE(res.enabled());  // incomplete rows are not training data
    CHECK(res.res["note"] == "not enough complete solver runs to measure");
}

TEST_CASE("prism_ai predict: bound labels and the unwind policy") {
    auto rec = [](int i, std::vector<std::string> statuses) {
        ojson r{{"file", "b" + std::to_string(i) + ".c"},
                {"function", "f"},
                {"features", {1, 2, i % 5, 3, 1, i % 3, 1, 0, 32, 3}},
                {"runs", ojson::array()}};
        for (std::size_t k = 0; k < predict::UNWINDS.size(); ++k)
            r["runs"].push_back(
                {{"unwind", predict::UNWINDS[k]}, {"status", statuses[k]}, {"seconds", 0.01 * predict::UNWINDS[k]}});
        return r;
    };
    auto rows = predict::bound_rows({rec(0, {"FAILED", "FAILED", "FAILED", "FAILED", "FAILED"}),
                                     rec(1, {"BOUNDED", "BOUNDED", "FAILED", "FAILED", "FAILED"}),
                                     rec(2, {"BOUNDED", "BOUNDED", "BOUNDED", "BOUNDED", "BOUNDED"}),
                                     rec(3, {"BOUNDED", "PROVED", "PROVED", "PROVED", "PROVED"}),
                                     rec(4, {"ERROR", "ERROR", "ERROR", "ERROR", "ERROR"})});
    std::vector<int> labels;
    for (const auto& r : rows) labels.push_back(r.label);
    CHECK(labels == std::vector<int>{1, 4, 16, 2});
    CHECK(predict::snap(1.2) == 4);
    CHECK(predict::snap(0.0) == 1);
    auto pol = predict::policy(rows, [](const predict::BoundRow&) { return 8; });
    CHECK(pol["agreement"].get<double>() == 1.0);
    CHECK(pol["failed_functions"].get<int>() == 2);
    auto pol1 = predict::policy(rows, [](const predict::BoundRow&) { return 1; });
    CHECK(pol1["agreement"].get<double>() == 0.5);  // b1 needs unwind 4, b3 needs 2
    // b0 finds it at 1 (0.01 s); b1 misses at 1 and escalates to 16 (0.01 + 0.16 s)
    CHECK(pol1["time_to_first_cex"].get<double>() == doctest::Approx(0.18).epsilon(1e-6));
    CHECK(pol1["bounded"].get<int>() == 3);
}

TEST_CASE("prism_ai predict: the CLI writes the model and the markdown table") {
    auto td = ai_tmp("cli");
    {
        std::ofstream log(td / "solve_runs.jsonl");
        for (const auto& r : solver_logs()) log << r.dump() << "\n";
    }
    std::string out, err;
    CHECK(argv_call(predict::predict_main,
                    {"--solve-log", (td / "solve_runs.jsonl").string(), "--out", (td / "model.json").string(),
                     "--markdown", (td / "m.md").string(), "--trees", "20"},
                    &out, &err) == 0);
    CAPTURE(err);
    std::ifstream mf(td / "model.json");
    auto m = ojson::parse(mf);
    CHECK(m["kind"] == "prism-gbdt");
    CHECK(m["schema"] == 1);
    std::ifstream md(td / "m.md");
    std::string mds((std::istreambuf_iterator<char>(md)), {});
    CHECK(mds.find("solver choice") != std::string::npos);
    CHECK(out == mds);
    // --out is required; an unknown flag is a usage error.
    CHECK(argv_call(predict::predict_main, {"--solve-log", "x"}) == 2);
    CHECK(argv_call(predict::predict_main, {"--bogus"}) == 2);
    CHECK(argv_call(prism_ai::main_dispatch, {"prism_ai", "nope"}) == 2);
}

// ------------------------------------------------------------------ the built-in model
namespace {
namespace builtin {
#include "prism/solver/predict_default.inc"
}
}  // namespace

TEST_CASE("prism_ai builtin model: embedded from predict_default.json; pieces fit a literal") {
    std::string txt;
    for (const char* part : builtin::kDefaultModelParts) {
        CHECK(std::string(part).size() <= 12000);
        txt += part;
    }
    std::ifstream in(repo_root() / "src" / "prism" / "solver" / "predict_default.json");
    REQUIRE(in);
    const auto file = ojson::parse(in);
    const auto m = ojson::parse(txt);
    CHECK(m == file);
    CHECK(m["kind"] == "prism-gbdt");
    CHECK(m["schema"] == 1);
    CHECK(m["query_features"] == ojson(prism::solver::predict::query_feature_names()));
    CHECK(m["function_features"] == ojson(prism::solver::predict::function_feature_names()));
}

TEST_CASE("prism_ai builtin model: enabled only with a measured win") {
    std::ifstream in(repo_root() / "src" / "prism" / "solver" / "predict_default.json");
    REQUIRE(in);
    const auto m = ojson::parse(in);
    if (!m["enabled"].get<bool>()) return;
    const auto& rp = m["metrics"]["replay"];
    REQUIRE(rp["chosen"].is_string());
    const auto chosen = rp["chosen"].get<std::string>();
    CHECK_FALSE(m.contains("bound"));  // the unwind model did not win (it lost a verdict)
    for (int k : predict::DECIDE_KS) {
        const auto& per = rp["k"][std::to_string(k)];
        double noise = 0.0;
        for (auto it = rp["noise"].begin(); it != rp["noise"].end(); ++it)
            if (it.key() != "queries") noise = std::max(noise, it.value()["rel"].get<double>());
        const double best = std::min(per["rules"]["total_s"].get<double>(), per["history"]["total_s"].get<double>());
        CHECK(per[chosen]["total_s"].get<double>() < (1 - noise) * best);
        CHECK(per[chosen]["timeouts"].get<int>() <= per["rules"]["timeouts"].get<int>());
    }
    for (auto it = m["metrics"]["end_to_end"].begin(); it != m["metrics"]["end_to_end"].end(); ++it) {
        CHECK(it.value()["disagreements"] == 0);
        CHECK(it.value()["model"]["total_s"].get<double>() < it.value()["rules"]["total_s"].get<double>());
    }
}

// ------------------------------------------------------------------ scheduler replay
TEST_CASE("prism_ai replay: the rules replay matches the portfolio order") {
    auto r = row({{"z3", {Ans, 1.0}}, {"cadical", {Ans, 0.2}}});
    auto p = sched::plan_rules(r);
    // Two cores: Z3 alone for 0.15 s, then CaDiCaL joins and answers at 0.35 s.
    CHECK(sched::simulate(r, p.est, p.lead, p.delay, 2) == std::make_pair(0.35, std::string("cadical")));
    // One core: Z3 holds it until it answers.
    CHECK(sched::simulate(r, p.est, p.lead, p.delay, 1) == std::make_pair(1.0, std::string("z3")));
}

TEST_CASE("prism_ai replay: a censored member holds its core until the timeout") {
    auto r = row({{"z3", {Cens, 8.0}}, {"kissat", {Ans, 0.5}}});
    auto p = sched::plan_rules(r);
    CHECK(sched::simulate(r, p.est, p.lead, p.delay, 1) == std::make_pair(8.0, std::string()));  // a timeout
    CHECK(sched::simulate(r, p.est, std::string("kissat"), 0.15, 1) == std::make_pair(0.5, std::string("kissat")));
    CHECK(sched::simulate(r, p.est, p.lead, p.delay, 2) == std::make_pair(0.65, std::string("kissat")));
}

TEST_CASE("prism_ai replay: a member that gives up frees its core") {
    auto r = row({{"z3", {Ans, 2.0}}, {"bitwuzla", {Gave, 0.01}}, {"sls", {Gave, 0.25}}});
    const sched::Est est{{"bitwuzla", 0.0}, {"sls", 0.1}, {"z3", 1.0}};
    // bitwuzla fails at 0.01, the walker keeps its core for its budget (max(1 s, 10% T)).
    CHECK(sched::simulate(r, est, std::string("bitwuzla"), 0.0, 1) == std::make_pair(3.01, std::string("z3")));
}

TEST_CASE("prism_ai replay: load dedups by VC and splits by file") {
    auto td = ai_tmp("load");
    {
        std::ofstream f(td / "s.jsonl");
        for (auto [file, sha] : {std::pair{"b.c", "s1"}, {"a.c", "s1"}, {"a.c", "s2"}})
            f << ojson{{"file", file},
                       {"function", "f"},
                       {"vc", "p"},
                       {"sha", sha},
                       {"bucket", "b"},
                       {"timeout_s", 8.0},
                       {"features", {5, 2, 1, 0, 0, 0, 0, 0, 0}},
                       {"runs",
                        {{"z3", {{"kind", "unsat"}, {"wall_s", 0.1}}},
                         {"kissat", {{"kind", "timeout"}, {"wall_s", 8.0}}},
                         {"sls", {{"kind", "unknown"}, {"wall_s", 0.25}}}}}}
                     .dump()
              << "\n";
        f << "not json\n";
    }
    auto rows = sched::load({td / "s.jsonl"});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "a.c");
    CHECK(rows[0].sha == "s1");
    CHECK(rows[1].key == "a.c");
    CHECK(rows[1].sha == "s2");
    const std::vector<std::pair<std::string, sched::Run>> want = {
        {"z3", {Ans, 0.1}}, {"kissat", {Cens, 8.0}}, {"sls", {Gave, 0.25}}};
    CHECK(rows[0].runs == want);
    CHECK(rows[0].answer == "unsat");
    std::vector<sched::Row> many;
    for (int i = 0; i < 200; ++i) many.push_back(row({}, "f" + std::to_string(i) + ".c"));
    auto [train, test] = sched::split(many);
    CHECK_FALSE(train.empty());
    CHECK_FALSE(test.empty());
    std::set<std::string> tk;
    for (const auto& r : train) tk.insert(r.key);
    for (const auto& r : test) CHECK(tk.count(r.key) == 0);
}

TEST_CASE("prism_ai replay: noise-only data enables nothing") {
    std::vector<sched::Row> rows;
    for (int i = 0; i < 300; ++i) {
        const long long h = i * 2654435761LL % 97;
        auto r = row({{"z3", {Ans, 0.05 + (h % 10) / 1000.0}}, {"cadical", {Ans, 0.05 + (h % 7) / 1000.0}}},
                     "f" + std::to_string(i) + ".c");
        r.sha = "s" + std::to_string(i);
        rows.push_back(r);
    }
    auto res = sched::run_all(rows, {1, 2}, 10);
    auto [chosen, why] = sched::decide(res.res, {1, 2});
    CHECK_MESSAGE(!chosen, why);
}

TEST_CASE("prism_ai replay: a learnable split is found and exported") {
    std::vector<sched::Row> rows;
    for (int i = 0; i < 300; ++i) {
        const bool big = i % 2 == 0;
        sched::Row r;
        r.key = "f" + std::to_string(i) + ".c";
        r.sha = "s" + std::to_string(i);
        r.bucket = "b";
        r.x = {5.0, big ? 4.0 : 1.5, 1.0, 0, 0, 0, 0, 0, 0};
        r.T = 8.0;
        r.runs = {{"z3", big ? sched::Run{Cens, 8.0} : sched::Run{Ans, 0.01}},
                  {"kissat", big ? sched::Run{Ans, 0.3} : sched::Run{Ans, 0.2}}};
        r.answer = "unsat";
        rows.push_back(r);
    }
    auto res = sched::run_all(rows, {1, 2}, 20);
    auto [chosen, why] = sched::decide(res.res, {1});
    REQUIRE_MESSAGE(chosen, why);  // one core: rules put Z3 first and time out on every big query
    CHECK(res.res["k"]["1"][*chosen]["timeouts"].get<int>() < res.res["k"]["1"]["rules"]["timeouts"].get<int>());
    predict::ReplayDecision d{res, chosen, ""};
    auto model = predict::build_model(nullptr, nullptr, &d);
    CHECK(model["enabled"] == true);
    std::set<std::string> solvers;
    for (auto it = model["solvers"].begin(); it != model["solvers"].end(); ++it) solvers.insert(it.key());
    CHECK(solvers == std::set<std::string>{"z3", "kissat"});
    CHECK(model["head_start"] == !chosen->ends_with("-order"));
    CHECK(model["metrics"]["replay"]["chosen"] == *chosen);
    const auto md = predict::markdown(model);
    CHECK(md.find("replay decision: " + *chosen) != std::string::npos);
    CHECK(md.find("| rules | ") != std::string::npos);
}

// ------------------------------------------------------------------ measurement
TEST_CASE("prism_ai measure: pairwise precision / recall / F1") {
    const std::map<std::string, std::string> label{{"a", "x"}, {"b", "x"}, {"c", "y"}};
    CHECK(prism_ai::measure::pairwise({{"a", "b"}, {"c"}}, label)["f1"].get<double>() == 1.0);
    CHECK(prism_ai::measure::pairwise({{"a"}, {"b"}, {"c"}}, label)["recall"].get<double>() == 0.0);
    auto r = prism_ai::measure::pairwise({{"a", "b", "c"}}, label);
    CHECK(r["precision"].get<double>() == doctest::Approx(1.0 / 3).epsilon(1e-3));
    CHECK(r["groups"] == 1);
}

TEST_CASE("prism_ai measure: triage, regress and draft read the run's files; sample-pairs is seeded") {
    auto td = ai_tmp("measure");
    auto f = [](const char* file, int line, const char* cls, const char* fn, const char* msg) {
        return ojson{{"status", "FAILED"}, {"file", file}, {"line", line}, {"cls", cls}, {"function", fn}, {"message", msg}};
    };
    ojson rep{{"root", td.string()},
              {"stages",
               {{{"name", "bmc"},
                 {"findings",
                  {f("a.c", 1, "INT-DIV-ZERO", "g", "div"), f("a.c", 2, "INT-DIV-ZERO", "g", "div again"),
                   f("b.c", 9, "MEM-OOB-READ", "h", "oob")}}},
                {{"name", "pir"}, {"findings", {f("a.c", 1, "INT-DIV-ZERO", "g", "div")}}}}}};
    std::ofstream(td / "report.json") << rep.dump();
    ojson tri{{"kind", "prism-triage"},
              {"embedder", "tfidf"},
              {"clusters",
               {{{"id", 0}, {"members", {"bmc#0", "bmc#1", "pir#0"}}}, {{"id", 1}, {"members", {"bmc#2"}}}}}};
    std::ofstream(td / "triage.json") << tri.dump();
    auto t = prism_ai::measure::measure_triage(td);
    CHECK(t["findings"] == 4);
    CHECK(t["labels"] == 2);
    CHECK(t["triage"]["f1"].get<double>() == 1.0);
    CHECK(t["baseline_file_line_cls"]["recall"].get<double>() < 1.0);  // the exact key misses the line-2 duplicate
    CHECK(t["embedder"] == "tfidf");

    fs::create_directories(td / "regression_tests");
    std::ofstream(td / "regression_tests" / "manifest.json")
        << R"({"tests": [{"status": "reproduces"}, {"status": "written"}, {"status": "unsupported"}]})";
    auto rg = prism_ai::measure::measure_regress(td);
    CHECK(rg["tests"] == 2);
    CHECK(rg["reproduce_share"].get<double>() == 0.5);

    std::ofstream(td / "draft_report.json")
        << R"({"sections": [{"claims": [{"links": ["verdict:bmc#0"]}, {"links": []}]}], "rejected": [1], "author": "template"})";
    auto dr = prism_ai::measure::measure_draft(td);
    CHECK(dr["draft_report"]["claims"] == 2);
    CHECK(dr["draft_report"]["linked"] == 1);
    CHECK(dr["draft_report"]["rejected"] == 1);
    CHECK(dr["draft_report"]["theorems_indexed"].is_null());

    std::string out1, out2;
    CHECK(argv_call(prism_ai::measure::sample_pairs_main, {td.string(), "--n", "5", "--seed", "1"}, &out1) == 0);
    CHECK(argv_call(prism_ai::measure::sample_pairs_main, {td.string(), "--n", "5", "--seed", "1"}, &out2) == 0);
    CHECK(out1 == out2);
    // bmc#0 / pir#0 share file, line and class: only the pairs with bmc#1 are "added" by triage.
    CHECK(out1.rfind("2 added pairs in 1 clusters; sample 1\n", 0) == 0);
    std::string m;
    CHECK(argv_call(prism_ai::measure::measure_main, {"triage", td.string()}, &m) == 0);
    CHECK(ojson::parse(m) == t);
    CHECK(argv_call(prism_ai::measure::measure_main, {"ask", td.string()}) == 2);  // needs a questions file
}

// ------------------------------------------------------------------ reproducibility helpers
TEST_CASE("prism_ai pycompat: the seeded draws, rounding and float text of the first implementation") {
    prism_ai::PyRandom r1(1);
    std::vector<int> x{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    r1.shuffle(x);
    CHECK(x == std::vector<int>{6, 8, 9, 7, 5, 3, 0, 4, 1, 2});
    prism_ai::PyRandom r7(7);
    std::vector<std::uint64_t> d;
    for (int i = 0; i < 8; ++i) d.push_back(r7.randbelow(100));
    CHECK(d == std::vector<std::uint64_t>{41, 19, 50, 83, 6, 9, 68, 12});
    prism_ai::PyRandom big((1ULL << 40) + 5);
    CHECK(big.getrandbits(32) == 2166296868u);
    CHECK(big.getrandbits(32) == 2220160828u);
    prism_ai::PyRandom r0(0);
    std::vector<std::uint64_t> d0;
    for (int i = 0; i < 10; ++i) d0.push_back(r0.randbelow(3));
    CHECK(d0 == std::vector<std::uint64_t>{1, 1, 0, 1, 2, 1, 1, 1, 1, 1});

    using prism_ai::py_repr;
    CHECK(py_repr(0.1) == "0.1");
    CHECK(py_repr(1.0) == "1.0");
    CHECK(py_repr(1e-05) == "1e-05");
    CHECK(py_repr(0.0001) == "0.0001");
    CHECK(py_repr(123456789012345678.0) == "1.2345678901234568e+17");
    CHECK(py_repr(1e16) == "1e+16");
    CHECK(py_repr(9999999999999998.0) == "9999999999999998.0");
    CHECK(py_repr(-0.35) == "-0.35");
    CHECK(py_repr(1.0 / 3) == "0.3333333333333333");
    CHECK(py_repr(5e-324) == "5e-324");
    using prism_ai::py_round;
    CHECK(py_round(2.675, 2) == 2.67);
    CHECK(py_round(0.125, 2) == 0.12);
    CHECK(py_round(0.375, 2) == 0.38);
    CHECK(py_round(1.0005, 3) == 1.0);
    CHECK(py_round(2.5, 0) == 2.0);
    CHECK(prism_ai::py_percent(0.125, 1) == "12.5%");
    CHECK(prism_ai::py_signed_fixed(-0.0004 * 100, 1) == "-0.0");
}

#ifndef _WIN32
TEST_CASE("prism_ai e2e: child processes get their own environment; a stall is killed") {
    ::setenv("PRISM_AI_TEST_DROP", "gone", 1);
    auto r = prism_ai::e2e::run_with_env({"/bin/sh", "-c", "echo \"$PRISM_AI_TEST_SET:${PRISM_AI_TEST_DROP:-unset}\""},
                                         {{"PRISM_AI_TEST_SET", "yes"}}, {"PRISM_AI_TEST_DROP"}, 30.0);
    ::unsetenv("PRISM_AI_TEST_DROP");
    CHECK(r.rc == 0);
    CHECK_FALSE(r.timed_out);
    CHECK(r.out == "yes:unset\n");
    const auto t0 = std::chrono::steady_clock::now();
    auto s = prism_ai::e2e::run_with_env({"/bin/sh", "-c", "sleep 30 & sleep 30"}, {}, {}, 0.5);
    CHECK(s.timed_out);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0);
    auto missing = prism_ai::e2e::run_with_env({"/nonexistent/prism"}, {}, {}, 5.0);
    CHECK(missing.rc == 127);
}
#endif
