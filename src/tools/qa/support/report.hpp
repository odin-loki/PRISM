#pragma once

// Scoring: one row per (task, function, verdict stage), its outcome, and the
// metrics, certified-mode summary and markdown built from the rows.
//
// A row (results.json) is an ordered JSON object:
//   task, origin, category, function, expected, property, stage, law_task,
//   findings [{status, cls, message, counterexample, line, extra?}],
//   seconds, error?, outcome, replay?

#include "replay.hpp"
#include "task.hpp"

#include <string>
#include <vector>

namespace prism::qa {

// wrong-proof | false-alarm | proved | refuted | bounded | no-answer | missing |
// failed-other-property | law-ok | law6-violation | unexpected-status
std::string classify(const Task& task, const std::string& fn, const ojson& found);

std::string pct(long a, long b);
// Python round(x, n) (correctly rounded, then the nearest double)
double py_round(double x, int n);

ojson compute_metrics(const std::vector<ojson>& rows, const std::vector<std::string>& stages);
// Roadmap 3 exit criterion: loop-free `true` functions -> PROVED-CERTIFIED.
ojson certified_summary(const std::vector<ojson>& rows);
std::string markdown(const ojson& metrics, const std::vector<ojson>& rows, const ojson& meta);

}  // namespace prism::qa
