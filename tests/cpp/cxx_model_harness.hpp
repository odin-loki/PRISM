// Shared driver for the C++ standard-library model tests (tests/cpp/test_cxx_*.cpp).
//
// Each model test compiles a trace program (tests/cxx_models/*_trace.cpp) twice
// with clang++ and ASan/UBSan, once against libstdc++ and once with the model
// headers (src/prism/pir/models/cxx) first on the include path, and compares
// what the two print. This header holds what every one of those files needs:
// running a command, choosing the compiler, the libstdc++ probe, building a
// trace program and reading its output lines. Each test keeps its own flags,
// probe program and trace programs.
//
// Compiler: $PRISM_LLVM_BIN/clang++ first (CI fetches the pinned LLVM and sets
// it), then clang++ on PATH. The first one whose probe builds and runs wins.
//
// The probe always starts with `#include <bits/c++config.h>`: the models
// replace libstdc++'s containers and use its internals, so a clang++ on another
// standard library (the MSVC STL, libc++) has nothing to compare against and
// the cases are NOTRUN (prism_test::notrun), not failures and not passes.
#pragma once

#include "test_support.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace prism_test::cxx {

namespace fs = std::filesystem;

// The NOTRUN reason every model test gives when no compiler passes its probe.
inline constexpr const char* kNoCompiler = "clang++ with ASan/UBSan and libstdc++ not usable here";

// The source tree (PRISM_SOURCE_DIR).
fs::path repo();
// src/prism/pir/models/cxx: the model headers.
fs::path model_dir();
// tests/cxx_models/<file>: a trace program.
fs::path trace_src(std::string_view file);

// The flags most model tests build with: C++23, libstdc++ assertions,
// ASan+UBSan that stop at the first report, debug info, -O1, no warnings.
const std::vector<std::string>& asan_flags();

// Run argv (no shell). True when it started and exited 0. out / err, when
// given, receive its stdout / stderr (CR stripped from stdout on Windows).
bool run_cmd(const std::vector<std::string>& argv, std::string* out = nullptr, std::string* err = nullptr);

// Backslashes to forward slashes.
std::string fwd(std::string s);

// The compilers to try, in order: $PRISM_LLVM_BIN/clang++ (when it runs), then
// clang++ (when it runs).
const std::vector<std::string>& cxx_candidates();

// The first candidate for which probe(cxx) is true, or "" when none is.
std::string pick_cxx(const std::function<bool(const std::string&)>& probe);

// Write tmp/probe.cpp as `#include <bits/c++config.h>` followed by program,
// build it as cxx + flags + probe.cpp + after + -o tmp/probe, and run it.
// True when both steps exit 0.
bool probe_clang_asan(const std::string& cxx, const fs::path& tmp, std::string_view program,
                      const std::vector<std::string>& flags, const std::vector<std::string>& after = {});

// Build src as tmp/<stem>.model (with_models: -isystem model_dir() after the
// flags) or tmp/<stem>.lib. REQUIREs that the build exits 0.
fs::path build_prog(const std::string& cxx, const fs::path& tmp, const fs::path& src, const std::string& stem,
                    const std::vector<std::string>& flags, bool with_models);

// Run exe with args, REQUIRE it exits 0, and return its stdout lines.
std::vector<std::string> run_lines(const fs::path& exe, const std::vector<std::string>& args = {});

// temp_directory_path()/name, emptied and created.
fs::path fresh_tmp(std::string_view name);

}  // namespace prism_test::cxx
