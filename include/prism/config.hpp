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
    std::filesystem::path fpk;  // FORGE model (.fpk), PRISM_FPK
    int jobs = 2;
    double timeout = 30.0;
    int unwind = 8;
    // pir: verify only these Check props (CheckOptions::only_props); internal
    std::vector<std::string> pir_only_props;
    bool conc_tso = false;
    int conc_store_buffer = 2;  // --store-buffer k: TSO store-buffer entries per thread  // --memory-model tso: conc stage checks x86-TSO (store buffers) instead of SC
    double fuzz_budget = 8.0;
    int fuzz_iters = 2048;
    bool llm = true;
    int repair_rounds = 3;
    std::optional<std::vector<std::string>> stages;
    std::vector<std::string> skip;
    bool resume = false;
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
    // --pir-harness: pir binds a pointer parameter that no requires clause or
    // draft covers to an object built from the IR types (docs/PIR.md
    // "Automatic pointer harness"); every choice is a named assumption, so the
    // best verdict is PROVED-ASSUMING. Opt-in (Law 6).
    bool pir_harness = false;
    bool pir_discharge = false;     // --pir-discharge: assume-guarantee, harness obligations checked at every call site (docs/HARNESS_GENERATOR.md)
    bool pir_container_of = false;  // --pir-container-of: a parameter used at a negative constant offset gets its containing object
    bool pir_vacuity = false;       // --pir-vacuity: a harnessed function's exit must be reachable under its assumptions
    int pir_harness_depth = 2;                 // --pir-harness-depth N: nested pointer levels
    unsigned long long pir_harness_elems = 16;  // --pir-harness-elems N: default array-like pointee length
    unsigned long long pir_harness_bytes = 64;  // --pir-harness-bytes N: size when the pointee type is unknown
    // --jsonl FILE: the pir stage appends one JSON line per function (docs/PIR.md "Machine-readable output")
    std::filesystem::path jsonl;
    // --fp-checks: pir also reports IEEE floating-point exceptions (division
    // by zero, invalid operation, overflow) that Annex F defines. Opt-in.
    bool fp_checks = false;
    // Roadmap 3.2 (--certified): the pir stage asks the solver library for an
    // LRAT certificate of every verification condition; a function whose VCs
    // are all checked by cake_lpr is PROVED-CERTIFIED. prism/config.py certified.
    bool certified = false;
    // Solver query cache (--solver-cache DIR); empty = the solver library's
    // default ($XDG_CACHE_HOME/prism/solver). prism/config.py solver_cache.
    std::filesystem::path solver_cache;
    // --compile-commands FILE: the pir stage compiles each unit with the flags
    // of its entry in this compile_commands.json (GNU C dialect, kernel
    // defines, include path, ABI flags; docs/PIR.md "Units with their own
    // flags"). Empty = the scan root's database for -I/-D only, as before.
    std::filesystem::path compile_commands;
    // --kernel-api linux|freebsd: the pir stage links the kernel API specs of
    // that kernel (src/prism/pir/models/kernel, docs/KERNEL_MODELS.md) and
    // models inline asm, asm goto, atomics and barriers as named assumptions.
    // "" = off (default). --assume-unknown-calls: a call without a spec havocs
    // its result and the objects its pointer arguments point to, recorded as an
    // assumption (opt-in, never on by default).
    std::string pir_kernel_api;
    bool pir_assume_unknown_calls = false;
    // --ops-table FILE: indirect calls through a field of a known ops table are checked
    // against that field's targets (docs/PIR.md). Empty = off (default).
    std::filesystem::path ops_table;
    // Harness phase 2 (docs/HARNESS_PHASE2.md), all opt-in, each use a named assumption:
    //  --callee-contracts FILE: assume-guarantee stubs for callees without a body (TSV, requires checked at the call site)
    //  --pir-ops-auto: ops tables derived from the unit's own function-pointer stores and initialisers; a harness
    //    function-pointer field is bound to one of its targets
    //  --pir-addr-values: ptrtoint / inttoptr constant expressions and integer-address initialisers
    //  --pir-harness-deep: harness depth 4 and 64 objects per parameter (unless set explicitly)
    std::filesystem::path callee_contracts;
    std::filesystem::path pir_field_invariants;  // --pir-field-invariants FILE: struct field value invariants for the auto harness
    bool pir_ops_auto = false;
    bool pir_addr_values = false;
    bool pir_harness_deep = false;
    bool pir_build_semantics = false;   // --pir-build-semantics: take the unit's -fwrapv / -fno-strict-overflow (signed + - * << wrap)
    bool pir_kernel_stops = false;      // --pir-kernel-stops: BUG() / panic() end the path (named), not a FUNC-CONTRACT finding
    bool pir_kernel_current = false;    // --pir-kernel-current: current / curthread point to a valid object (named)
    bool pir_global_harness = false;    // --pir-global-harness: mutable pointer globals point to a valid object at entry (named)
    bool pir_lock_wrappers = false;     // --pir-lock-wrappers: acquire-only / release-only roots are lock wrappers (named)
    bool pir_builtin_contracts = false;  // --pir-builtin-contracts: contracts for llvm.read_register / prefetch / objectsize (needs --kernel-api)
    bool pir_icall_havoc = false;       // --pir-icall-havoc: unresolved indirect calls are a havoc of all memory (named)
    // --lock-contracts FILE (TSV, docs/KERNEL_CONCURRENCY.md section 14): lock contracts of
    // unmodified kernel code, read by the pir stage (needs --kernel-api). "" = none.
    std::string pir_lock_contracts;
    // SV-COMP / BMC: "LP64" (default) or "ILP32" (32-bit long, size_t, pointers in bmc).
    std::string data_model{"LP64"};

    PRISM_API bool want(std::string_view name) const;
    PRISM_API std::optional<std::filesystem::path> which(
        std::initializer_list<std::string_view> names) const;
    // The one configured LLVM toolchain (third_party/llvm/PIN.toml).
    // Directory holding clang, clang++, opt, llvm-link, lli. Empty = resolve:
    // $PRISM_LLVM_BIN, then the pinned fetch dir baked in at build time
    // (PRISM_LLVM_PINNED_BIN, scripts/fetch_llvm.py), then PATH.
    std::filesystem::path llvm_bin;
    // tools[name] override, then llvm_bin/name, then PATH name. No versioned
    // fallbacks: PRISM's IR parser targets exactly the pinned release.
    PRISM_API std::optional<std::filesystem::path> llvm_tool(std::string_view name) const;
    // (1) tools[], (2) the pinned `prism-deps tool` build
    // <tools_home>/<component>/<commit>/bin/ (commit from third_party/MANIFEST.toml,
    // baked in at build time), (3) PATH.
    PRISM_API std::optional<std::filesystem::path> which_adapter(
        std::string_view stage, std::initializer_list<std::string_view> names) const;
};

PRISM_API Config default_config();
// True when p is root or lies below it. Law 9: pinned adapter binaries are
// never taken from inside the scanned tree without --allow-exec.
PRISM_API bool path_within(const std::filesystem::path& p, const std::filesystem::path& root);
// Findings and FunctionInfo.file carry paths relative to the scan root. A stage that opens one must go through
// source_path(), never against the process cwd: from any other directory the file is not found, the unit loses
// its includes and types, and contracts/wp report ERROR ("unknown type name 'uint32_t'"). The pipeline sets the
// root once before the first stage. source_path returns `file` unchanged when it is absolute, no root is set,
// or nothing exists under the root.
PRISM_API void set_source_root(const std::filesystem::path& root);
PRISM_API std::filesystem::path source_path(const std::filesystem::path& file);
// "prism-deps tool <component> ..." or a system-tool hint.
PRISM_API std::string adapter_install(std::string_view stage);
// A leading ~ or ~/ replaced by the home directory (Path.expanduser).
PRISM_API std::filesystem::path expand_user(const std::filesystem::path& p);
// $PRISM_TOOLS_DIR or ~/.prism/tools (the `prism-deps tool` install root).
PRISM_API std::filesystem::path tools_home();
// Commit that third_party/MANIFEST.toml pins for an external component.
PRISM_API std::optional<std::string> pinned_commit(std::string_view component);
// Manifest commit when exe is under <tools_home>/<name>/<commit>/, else
// "path:<abs>;sha256:<hash>". prism/config.py tool_identity.
PRISM_API std::string tool_identity(const std::filesystem::path& exe);
// extra["tool_sha"] = tool_identity(exe) on every finding (existing kept).
PRISM_API void stamp_tool_sha(std::vector<Finding>& findings, const std::filesystem::path& exe);
PRISM_API std::string sha256_hex(std::string_view data);

// The documented environment variables (src/prism/config.cpp).
struct EnvVar {
    std::string_view name, purpose;
};
PRISM_API const std::vector<EnvVar>& env_vars();

}  // namespace prism
