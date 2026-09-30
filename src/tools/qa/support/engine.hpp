#pragma once

// Running the PRISM engine under test on one task and reading its report.

#include "replay.hpp"
#include "task.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace prism::qa {

// The engine binary: --prism, else $PRISM_BIN, else ./build/prism under the
// repository (empty when none exists).
std::vector<std::string> prism_command(const std::string& prism_arg);
// stages the engine lists (--list-stages); empty when it cannot run
std::vector<std::string> list_stages(const std::vector<std::string>& cmd);

struct PrismRun {
    std::optional<std::string> error;  // TIMEOUT, no report.json (...), unreadable report.json (...)
    // stage (renamed) -> function -> findings [{status, cls, message, counterexample, line, extra?}]
    std::map<std::string, std::map<std::string, ojson>> findings;
    std::map<std::string, std::string> stage_status;
    double seconds = 0;
    int exit = 0;
    std::vector<std::string> argv;

    const ojson& found(const std::string& stage, const std::string& fn) const;
};

// One PRISM run on task.source with `--no-llm --stage STAGES --out
// WORK/TAG/<ident>`. mem_limit_mb caps the address space of the run and every
// child it starts (0: none): a run over it is no answer, never a proof.
PrismRun run_prism(const std::vector<std::string>& cmd, const Task& task, const std::vector<std::string>& stages,
                   const fs::path& work, double timeout, int unwind, const std::vector<std::string>& extra_args = {},
                   const std::map<std::string, std::string>& rename = {}, const std::string& tag = "prism",
                   long mem_limit_mb = 0);

}  // namespace prism::qa
