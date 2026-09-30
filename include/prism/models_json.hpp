#pragma once

// JSON form of the report records (report.json, stages.jsonl,
// functions.json). nlohmann ADL hooks: `nlohmann::json j = finding;` and
// `j.get<prism::Finding>()`. Loading is per field and tolerant, as the
// Python engine's from_dict is: a missing, null or mistyped field takes its
// default (a numeric string such as "3" is read as the number), so one bad
// field never loses the record.

#include "prism/export.hpp"
#include "prism/models.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace prism {

PRISM_API void to_json(nlohmann::json& j, const Finding& f);
PRISM_API void from_json(const nlohmann::json& j, Finding& f);
PRISM_API void to_json(nlohmann::json& j, const StageResult& s);
PRISM_API void from_json(const nlohmann::json& j, StageResult& s);
// `body_line` / `body_col` are written (functions.json), except inside a
// RunReport (report.json keeps the Python engine's function shape);
// `cxx_std` is not (set per run).
PRISM_API void to_json(nlohmann::json& j, const FunctionInfo& f);
PRISM_API void from_json(const nlohmann::json& j, FunctionInfo& f);
PRISM_API void to_json(nlohmann::json& j, const RunReport& r);
PRISM_API void from_json(const nlohmann::json& j, RunReport& r);

// Serialise for a file: invalid UTF-8 in any string becomes U+FFFD instead
// of throwing (a source byte in a message must not abort the run).
PRISM_API std::string dump_json(const nlohmann::json& j, int indent = -1);

}  // namespace prism
