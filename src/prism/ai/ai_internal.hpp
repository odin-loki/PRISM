#pragma once

// Private helpers shared by src/prism/ai/*.cpp (not installed API).

#include "prism/ai.hpp"

#include <optional>
#include <string>
#include <vector>

namespace prism::ai {

// Plain-HTTP client of the pipeline (defined in src/prism/stages_rest.cpp,
// same socket code the llm stage uses). nullopt on connection failure or non-2xx.
std::optional<std::string> http_request_raw(const std::string& method, const std::string& url,
                                            const std::string& body, int timeout_ms);

// Grammar with its `ident` rule narrowed to exactly `names` (GBNF alternatives).
std::string grammar_for(const std::string& name, const std::vector<std::string>& names);

// Why model features are unavailable right now ("" when a backend is bound).
std::string model_unavailable_reason();

std::string trim(const std::string& s);
bool is_identifier(const std::string& s);
std::string join(const std::vector<std::string>& v, const std::string& sep);

// Source text of fn from its file (comments kept: they are part of what a
// model would read, which is exactly why it is fenced as untrusted).
std::string function_source(const FunctionInfo& fn);

}  // namespace prism::ai
