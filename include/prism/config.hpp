#pragma once

#include "prism/export.hpp"

#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

struct Config {
    std::filesystem::path root{"."};
    std::filesystem::path out{"prism-out"};
    std::filesystem::path pbsd_root;
    std::string model{"qwen3.5:9b"};
    std::string ollama_host{"http://127.0.0.1:11434"};
    std::filesystem::path gguf;
    std::string llama_server{"http://127.0.0.1:8080"};
    int jobs = 2;
    double timeout = 30.0;
    int unwind = 8;
    double fuzz_budget = 8.0;
    int fuzz_iters = 2048;
    bool llm = true;
    int repair_rounds = 3;
    std::optional<std::vector<std::string>> stages;
    std::vector<std::string> skip;
    bool resume = false;
    bool gui = false;
    // Explicit adapter binaries (--tool NAME=PATH). Searched before vendored/PATH.
    std::map<std::string, std::filesystem::path> tools;
    // Law 9: running code from the scanned tree (compiled harnesses, sanitizer
    // builds, perl -c, cargo clippy, eslint) or from the LLM is opt-in
    // (--allow-exec). Default: those steps are NOTRUN. prism/config.py allow_exec.
    bool allow_exec = false;

    PRISM_API bool want(std::string_view name) const;
    PRISM_API std::optional<std::filesystem::path> which(
        std::initializer_list<std::string_view> names) const;
    // (1) tools[], (2) third_party/<vendor>/ built exe, (3) PATH.
    PRISM_API std::optional<std::filesystem::path> which_adapter(
        std::string_view stage, std::initializer_list<std::string_view> names) const;
};

PRISM_API Config default_config();
// True when p is root or lies below it. Law 9: vendored adapter binaries are
// never taken from inside the scanned tree without --allow-exec.
PRISM_API bool path_within(const std::filesystem::path& p, const std::filesystem::path& root);
PRISM_API std::string adapter_install(std::string_view stage);

}  // namespace prism
