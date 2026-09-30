#pragma once

// Task definitions (SV-COMP task-definition format 2.0, and PRISM's own
// conformance task files) are YAML. No YAML library is linked, so this is
// a reader for the subset those files use, with PyYAML's (YAML 1.1)
// scalar typing:
//
//   - block mappings and block sequences (including `- key: value` items),
//   - flow sequences and flow mappings on one line (`[1, 2]`, `{a: b}`),
//   - plain, 'single-' and "double-quoted" scalars, `|` / `>` block scalars,
//   - comments, blank lines and a leading `---`.
//
// Anything else (anchors, tags, multi-line flow collections, multi-line
// plain scalars, several documents) is rejected with an error rather than
// read wrongly.

#include "prism/export.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace prism::taskdef {

struct YamlResult {
    std::optional<nlohmann::ordered_json> value;
    std::string error;  // why value is empty
};
PRISM_API YamlResult parse_yaml(const std::string& text);
PRISM_API YamlResult load_yaml(const std::filesystem::path& path);

struct Property {
    std::string property_file;           // as written (relative to the .yml)
    std::optional<bool> expected_verdict;
    std::string subproperty;
};

// SV-COMP task-definition format 2.0.
struct TaskDef {
    std::string format_version;
    std::vector<std::string> input_files;
    std::vector<Property> properties;
    std::string language;    // options.language ("" when absent)
    std::string data_model;  // options.data_model ("" when absent)
};
// nullopt (and error) when the document is not a task definition.
PRISM_API std::optional<TaskDef> parse_taskdef(const nlohmann::ordered_json& doc, std::string* error = nullptr);

}  // namespace prism::taskdef
