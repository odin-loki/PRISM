#pragma once

// PIR clang/opt usability probes for tests and stages (docs/MSVC_PORT.md §7 PIR).

#include "prism/config.hpp"
#include "prism/export.hpp"

#include <string>
#include <vector>

namespace prism::pir {

// PIR libc models and Windows lowering use this LLVM triple (LP64); Linux hosts use it only on _WIN32.
PRISM_API const char* lp64_target_triple();
// IR/metadata line: target triple = "<lp64_target_triple()>"
PRISM_API std::string lp64_llvm_triple_metadata_line();

// On _WIN32: {"--target=x86_64-unknown-linux-gnu"}; elsewhere empty (native LP64).
PRISM_API std::vector<std::string> lp64_target_flags();

// LP64 target plus, on _WIN32, -idirafter the libc name-header shim (conc, testdata/pir, debug_ir).
PRISM_API std::vector<std::string> lp64_clang_driver_flags();

// Target of the unit and library-model lowering: LP64 (above) or, with
// --data-model ILP32, i386-unknown-linux-gnu (32-bit long, size_t and
// pointers; pir's PtrModel follows the datalayout). Set for a pir run by
// FrontendTargetScope; the library-model cache is keyed by the triple.
PRISM_API bool frontend_ilp32();
PRISM_API const char* ilp32_target_triple();
PRISM_API const char* frontend_target_triple();
// target flags for the current frontend target ({} for native LP64 off _WIN32)
PRISM_API std::vector<std::string> frontend_target_flags();
class PRISM_API FrontendTargetScope {
public:
    explicit FrontendTargetScope(bool ilp32);
    ~FrontendTargetScope();
    FrontendTargetScope(const FrontendTargetScope&) = delete;
    FrontendTargetScope& operator=(const FrontendTargetScope&) = delete;

private:
    bool prev_;
};

// True when find_frontend has clang and opt and the LP64 triple probe succeeds.
// When false and notrun_reason is non-null, set to a Law-1 MESSAGE ("NOTRUN: ...").
PRISM_API bool frontend_usable(const Config& cfg, std::string* notrun_reason = nullptr);

// testdata/pir/*.c that need no glibc headers; always true off _WIN32 (MSVC_PORT §7 step 4).
PRISM_API bool pir_test_c_file_supported(const char* basename);

// C++ PIR (libstdc++ models): false on _WIN32; else frontend_usable and clang++ on PATH.
PRISM_API bool pir_cxx_usable(const Config& cfg, std::string* notrun_reason = nullptr);

}  // namespace prism::pir
