#pragma once

// Solver and bound prediction (roadmap 9.1 "small gradient-boosted model
// trained on PRISM's own logs", 9.3 "solver and bound prediction").
//
// The model is a tiny GBDT (depth-3 regression trees) trained offline by
// tools/prism_ai/predict.py on:
//   * <cache>/solve_log.jsonl — one line per solver query: the query's
//     features and the seconds each member needed (log_query below);
//   * bound logs — per function: features and the verdict at each unwind.
// It is exported as JSON and loaded here. It only changes WHICH solver gets
// the head start (and, when wired, which unwind is tried first); every
// answer is still decided and checked by the solver, so a wrong prediction
// costs time, never soundness.
//
// Off by default (roadmap 9.7): the model file must say "enabled": true,
// which tools/prism_ai/predict.py writes only when the held-out measurement
// beats the baseline, and PRISM_SOLVER_PREDICT=0 switches it off.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef PRISM_HAS_Z3
#include "prism/solver.hpp"
#endif

namespace prism::pir {
struct Function;
}

namespace prism::solver::predict {

// Feature names, in vector order (the trainer reads them from the model file
// and refuses a model whose names differ).
const std::vector<std::string>& query_feature_names();
const std::vector<std::string>& function_feature_names();

// Model file: $PRISM_SOLVER_MODEL, else <cache_dir>/predict_model.json.
std::filesystem::path model_path(const std::filesystem::path& cache_dir);
// Loaded, schema-valid and enabled (cached per path; reloaded when the file changes).
bool enabled(const std::filesystem::path& cache_dir);
// Predicted seconds for `member` on a query with these features; nullopt when
// disabled or the model has no target for the member.
std::optional<double> seconds(const std::filesystem::path& cache_dir, const std::string& member,
                              const std::vector<double>& features);
// Evaluates a GBDT JSON target {"base", "lr", "trees"} (exposed for tests).
double gbdt_eval(const std::string& target_json, const std::vector<double>& x);

// Bound prediction: the smallest unwind predicted to reach the verdict of a
// large unwind; clamped to [1, cap]. fallback when disabled.
std::vector<double> function_features(const pir::Function& fn);
int unwind_for(const std::filesystem::path& cache_dir, const std::vector<double>& features, int fallback,
               int cap = 64);

#ifdef PRISM_HAS_Z3
std::vector<double> query_features(const Features& ft);
// Appends one line to <cache_dir>/solve_log.jsonl (capped at 32 MB).
void log_query(const std::filesystem::path& cache_dir, const Features& ft, const SolveResult& res,
               const std::vector<std::string>& raced, double wall_s);
#endif

}  // namespace prism::solver::predict
