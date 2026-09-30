#pragma once

// prism_ai: offline analytics for PRISM's AI layer (roadmap 3.1 / 9.1 / 9.3 /
// 9.7). Nothing here runs inside the pipeline or changes a verdict.
//
//   prism_ai predict      train, measure and export the solver / bound model
//                         src/prism/solver/predict.cpp loads (predict.cpp)
//   prism_ai replay       the portfolio scheduler replayed on alone-times (sched.cpp)
//   prism_ai e2e          end-to-end A/B of the learned scheduler (e2e.cpp)
//   prism_ai measure      per-feature metrics for docs/AI.md (measure.cpp)
//   prism_ai sample-pairs finding pairs triage merges, for manual review (measure.cpp)
//
// Results are JSON objects in insertion order (nlohmann::ordered_json), the
// order the markdown tables and the model file are written in.

#include "prism/solver_gbdt.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace prism_ai {
using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

// Python-style text of a JSON scalar: ints as ints, floats as repr(), strings
// bare, True / False / None.
std::string py(const ojson& v);

// Deterministic held-out split by source file: sha256(key)[:8] % 1000 < 300.
bool held_out(const std::string& key, double share = 0.3);

std::vector<ojson> load_jsonl(const fs::path& p);

// ------------------------------------------------------------------ scheduler replay (sched.cpp)
namespace sched {

extern const std::vector<std::string> MEMBERS;  // portfolio.cpp add order
inline constexpr double Z3_FIRST_S = 0.15;      // portfolio.cpp kZ3FirstS
inline constexpr double EPS = 1e-3;

enum class State { Ans, Cens, Gave };  // answered / timed out (>= T) / gave up after s
struct Run {
    State state = State::Gave;
    double s = 0.0;
    bool operator==(const Run&) const = default;
};

struct Row {
    std::string key, sha, bucket;
    std::vector<double> x;
    double T = 8.0;
    std::vector<std::pair<std::string, Run>> runs;  // log order
    std::string answer;
    const Run* run(const std::string& m) const;
    bool has(const std::string& m) const { return run(m) != nullptr; }
};

// One row per distinct VC (dedup by sha, in (file, function, vc) order).
std::vector<Row> load(const std::vector<fs::path>& paths);
std::vector<Row> load_records(std::vector<ojson> recs);
std::pair<std::vector<Row>, std::vector<Row>> split(const std::vector<Row>& rows);
std::pair<double, bool> log_target(State st, double s, double T);
double sls_budget(double T);

using Est = std::map<std::string, double>;
struct Plan {
    Est est;
    std::optional<std::string> lead;
    double delay = 0.0;
};
// (wall seconds, winner or "" on a timeout)
std::pair<double, std::string> simulate(const Row& row, const Est& est, const std::optional<std::string>& lead,
                                        double lead_delay, int k, std::optional<double> T = std::nullopt);

double rule_estimate(const std::string& member, const std::vector<double>& x);
std::optional<std::string> fastest(const Row& r);

struct Model {
    std::string name = "model";
    virtual ~Model() = default;
    virtual std::optional<double> seconds(const std::string& member, const std::vector<double>& x) const = 0;
};

struct GbdtModel final : Model {
    std::vector<std::pair<std::string, prism::solver::gbdt::Model>> models;  // MEMBERS order
    GbdtModel(const std::vector<Row>& train, bool censored, int n_trees = 60);
    std::optional<double> seconds(const std::string& member, const std::vector<double>& x) const override;
};

struct KnnModel final : Model {
    explicit KnnModel(const std::vector<Row>& train, int k = 15);
    std::optional<double> seconds(const std::string& member, const std::vector<double>& x) const override;
    std::optional<std::string> winner(const std::vector<double>& x, const std::vector<std::string>& row_members) const;

private:
    int k_;
    std::vector<double> mu_, sd_;
    std::vector<std::pair<std::vector<double>, std::vector<std::pair<std::string, double>>>> pts_;
    mutable std::map<std::vector<double>, std::vector<std::size_t>> cache_;
    const std::vector<std::size_t>& nn(const std::vector<double>& x) const;
};

struct History {
    // bucket -> member -> {n, total seconds (a timeout counts T), wins}
    std::map<std::string, std::map<std::string, std::array<double, 3>>> b;
    explicit History(const std::vector<Row>& train);
};

using PlanFn = std::function<Plan(const Row&)>;
Plan plan_rules(const Row& r);
PlanFn make_plan_history(std::shared_ptr<History> h);
PlanFn make_plan_model(std::shared_ptr<Model> model, bool head_start = true);
PlanFn make_plan_winner(std::shared_ptr<KnnModel> knn);
PlanFn plan_static_first(const std::string& member);

struct Eval {
    ojson summary;  // total_s, timeouts, median_s, p90_s, first_is_fastest
    std::vector<double> walls;
};
Eval evaluate(const std::vector<Row>& rows, const PlanFn& plan, int k);
std::pair<double, double> bootstrap_delta(const std::vector<Row>& rows, const std::vector<double>& a,
                                          const std::vector<double>& b, int n = 1000, std::uint64_t seed = 7);

struct Replay {
    ojson res;  // the measurement (what goes into the model's metrics)
    std::map<std::string, std::shared_ptr<GbdtModel>> gbdt_models;  // "gbdt", "aft"
};
Replay run_all(const std::vector<Row>& rows, const std::vector<int>& ks, int n_trees = 60,
               const std::vector<Row>* repeat = nullptr);
std::pair<std::optional<std::string>, std::string> decide(const ojson& res, const std::vector<int>& ks,
                                                          double min_margin = 0.05);
std::string markdown(const ojson& res);

int replay_main(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace sched

// ------------------------------------------------------------------ predict (predict.cpp)
namespace predict {

extern const std::vector<int> UNWINDS;  // 1, 2, 4, 8, 16
inline constexpr int DEFAULT_UNWIND = 8;
extern const std::vector<int> REPLAY_KS;
extern const std::vector<int> DECIDE_KS;

struct SolverRow {
    std::string key, bucket;
    std::vector<double> x;
    std::vector<std::pair<std::string, double>> times;
    std::optional<double> time(const std::string& m) const;
};
std::vector<SolverRow> solver_rows(const std::vector<ojson>& recs);

struct SolverResult {
    ojson res;  // without the models
    std::vector<std::pair<std::string, prism::solver::gbdt::Model>> models;
    bool enabled() const { return res.value("enabled", false); }
};
SolverResult measure_solver(const std::vector<SolverRow>& rows, int n_trees = 60);

struct BoundRun {
    std::string status;
    double seconds = 0.0;
};
struct BoundRow {
    std::string key, function;
    std::vector<double> x;
    std::map<int, BoundRun> runs;
    std::string final_status;
    int label = 16;
};
std::vector<BoundRow> bound_rows(const std::vector<ojson>& recs);
ojson policy(const std::vector<BoundRow>& rows, const std::function<int(const BoundRow&)>& choose);
int snap(double y);

struct BoundResult {
    ojson res;  // without the model
    std::optional<prism::solver::gbdt::Model> model;
    bool enabled() const { return res.value("enabled", false); }
};
BoundResult measure_bound(const std::vector<BoundRow>& rows, int n_trees = 60,
                          const std::vector<BoundRow>* repeat = nullptr);

struct ReplayDecision {
    sched::Replay replay;
    std::optional<std::string> chosen;
    std::string why;
};
ojson build_model(const SolverResult* solver, const BoundResult* bound, const ReplayDecision* replay = nullptr);
std::string markdown(const ojson& model);

int predict_main(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace predict

// ------------------------------------------------------------------ measure (measure.cpp)
namespace measure {

ojson pairwise(const std::vector<std::vector<std::string>>& groups, const std::map<std::string, std::string>& label);
bool in_split(const std::string& file, const std::string& split);
ojson measure_triage(const fs::path& out, const std::string& split = "all",
                     const std::optional<fs::path>& triage_json = std::nullopt);
ojson measure_ask(const fs::path& out, const fs::path& questions, const std::string& prism);
ojson measure_regress(const fs::path& out);
ojson measure_draft(const fs::path& out);
int measure_main(int argc, char** argv, std::ostream& out, std::ostream& err);
int sample_pairs_main(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace measure

// ------------------------------------------------------------------ e2e (e2e.cpp)
namespace e2e {

struct ProcResult {
    int rc = -1;
    bool timed_out = false;
    std::string out;  // stdout only
};
// Runs argv with the environment changed by `set` (name, value) and `unset`,
// capturing stdout; the process group is killed after `timeout_s`.
ProcResult run_with_env(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& set,
                        const std::vector<std::string>& unset, double timeout_s);
int e2e_main(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace e2e

int main_dispatch(int argc, char** argv, std::ostream& out, std::ostream& err);

}  // namespace prism_ai
