#pragma once

#include "prism/export.hpp"
#include "prism/models.hpp"

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
    // ParanoidBSD tree (--pbsd PATH or PRISM_PBSD); empty = not configured.
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
    // Explicit adapter binaries (--tool NAME=PATH). Searched before ~/.prism/tools and PATH.
    std::map<std::string, std::filesystem::path> tools;
    // Law 9: running code from the scanned tree (compiled harnesses, sanitizer
    // builds, perl -c, cargo clippy, eslint) or from the LLM is opt-in
    // (--allow-exec). Default: those steps are NOTRUN. prism/config.py allow_exec.
    bool allow_exec = false;
    // Roadmap 9.3: requirement documents (markdown/text files or directories)
    // the model may draft contracts from (--requirements PATH, repeatable).
    std::vector<std::filesystem::path> requirements;
    // Human approvals of drafted contracts (--contracts-approved PATH);
    // empty = <root>/contracts.approved.json.
    std::filesystem::path contracts_approved;
    // --strict-aliasing: the pir stage also checks effective types (C11
    // 6.5p6-7) per byte. Opt-in: real code often violates them on purpose.
    // C++ engine only (the Python engine is frozen, roadmap D8).
    bool strict_aliasing = false;
    // --pir-drafts: pir may take pointer-object sizes from the template
    // harness draft (ai::draft_harness) when no requires clause gives them.
    // Opt-in: without it unguarded pointer parameters stay NEEDS-HARNESS (Law 6).
    bool pir_drafts = false;

    PRISM_API bool want(std::string_view name) const;
    PRISM_API std::optional<std::filesystem::path> which(
        std::initializer_list<std::string_view> names) const;
    // (1) tools[], (2) the pinned scripts/fetch_deps.py build
    // <tools_home>/<component>/<commit>/bin/ (commit from third_party/MANIFEST.toml,
    // baked in at configure time), (3) PATH.
    PRISM_API std::optional<std::filesystem::path> which_adapter(
        std::string_view stage, std::initializer_list<std::string_view> names) const;
};

PRISM_API Config default_config();
// True when p is root or lies below it. Law 9: pinned adapter binaries are
// never taken from inside the scanned tree without --allow-exec.
PRISM_API bool path_within(const std::filesystem::path& p, const std::filesystem::path& root);
// "python scripts/fetch_deps.py --tool <component> ..." or a system-tool hint.
PRISM_API std::string adapter_install(std::string_view stage);
// $PRISM_TOOLS_DIR or ~/.prism/tools (scripts/fetch_deps.py install root).
PRISM_API std::filesystem::path tools_home();
// Commit that third_party/MANIFEST.toml pins for an external component.
PRISM_API std::optional<std::string> pinned_commit(std::string_view component);
// Manifest commit when exe is under <tools_home>/<name>/<commit>/, else
// "path:<abs>;sha256:<hash>". prism/config.py tool_identity.
PRISM_API std::string tool_identity(const std::filesystem::path& exe);
// extra["tool_sha"] = tool_identity(exe) on every finding (existing kept).
PRISM_API void stamp_tool_sha(std::vector<Finding>& findings, const std::filesystem::path& exe);
PRISM_API std::string sha256_hex(std::string_view data);

}  // namespace prism
