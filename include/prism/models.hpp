#pragma once

#include "prism/export.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism {

struct FunctionInfo {
    std::string file;
    std::string name;
    std::string kind;  // SCALAR | POINTER | VOID | OTHER
    int line = 0;
    std::string signature;
    std::vector<std::pair<std::string, std::string>> params;
    std::string return_type = "int";
    bool is_static = false;
    std::string body;
    std::pair<int, int> span{0, 0};
};

struct Finding {
    std::string stage;
    std::string status;
    std::string file;
    std::optional<std::string> function;
    std::optional<int> line;
    std::string cls;
    std::string message;
    std::string strength;
    std::string evidence;
    std::string counterexample;
    std::map<std::string, std::string> extra;
};

struct StageResult {
    std::string name;
    std::string status;
    std::string detail;
    double started = 0;
    double elapsed = 0;
    std::vector<Finding> findings;
    int records = 0;
    std::string install;
};

struct RunReport {
    std::string root;
    double started = 0;
    std::vector<StageResult> stages;
    std::vector<FunctionInfo> functions;
    double visibility = 0;
    double answer = 0;
    double resolution = 0;
    double confidence = 0;
    std::vector<std::string> notes;

    PRISM_API std::string dumps() const;
    PRISM_API void save(const std::filesystem::path& path) const;
    PRISM_API static std::optional<RunReport> load(const std::filesystem::path& path);
};

PRISM_API Finding finding_from_json_object(const std::string& json_object);
PRISM_API StageResult stage_from_json_object(const std::string& json_object);
PRISM_API FunctionInfo function_from_json_object(const std::string& json_object);

}  // namespace prism
