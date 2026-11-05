#pragma once

#include "prism/export.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

// C ABI widths for the hand BMC encoder, the source-level stages and SV-COMP
// replay: ILP32 / LP64 (Linux, glibc) and LLP64 (Windows x64, MSVC).
struct AbiModel {
    int int_w = 32;
    int long_w = 64;
    int long_long_w = 64;
    int size_w = 64;
    int ptr_w = 64;
    int wchar_w = 32;  // glibc: 32-bit signed; MSVC: 16-bit unsigned
    int ptr_bytes() const { return ptr_w / 8; }
    int size_bytes() const { return size_w / 8; }
    int long_bytes() const { return long_w / 8; }
};

PRISM_API AbiModel abi_lp64();
PRISM_API AbiModel abi_ilp32();
// Windows x64: 32-bit long, 64-bit long long / size_t / pointers, 16-bit wchar_t.
PRISM_API AbiModel abi_llp64();
// Only "ILP32" (any case) selects 32-bit long, size_t and pointers; "LLP64"
// selects the Windows x64 widths. "LP64", empty, and every other string stay
// LP64: a typo must not narrow widths.
PRISM_API AbiModel abi_from_data_model(std::string_view data_model);
PRISM_API bool is_ilp32(std::string_view data_model);

// ---- analysis target (--target / PRISM_TARGET) ----------------------------
// The platform the scanned code is compiled for. It decides the clang triple
// pir lowers with, the C library and C++ standard library headers, and the
// type widths every stage assumes. Default: the host (a Windows x64 host
// analyses for x86_64-pc-windows-msvc, every other host for x86_64 Linux).
enum class TargetKind { linux_lp64, linux_ilp32, windows_llp64 };

struct Target {
    TargetKind kind = TargetKind::linux_lp64;
    // canonical: x86_64-unknown-linux-gnu, i386-unknown-linux-gnu, x86_64-pc-windows-msvc
    std::string triple = "x86_64-unknown-linux-gnu";
    AbiModel abi;
    bool msvc() const { return kind == TargetKind::windows_llp64; }
    bool linux_gnu() const { return !msvc(); }
    // "LP64", "ILP32" or "LLP64"
    PRISM_API std::string_view data_model() const;
    // the C++ standard library the target's headers provide
    std::string_view cxx_stdlib() const { return msvc() ? "MSVC STL" : "libstdc++"; }
};

PRISM_API Target target_of(TargetKind k);
// x86_64-pc-windows-msvc on a Windows x64 host, x86_64-unknown-linux-gnu elsewhere.
PRISM_API const char* host_target_triple();
// Accepts the canonical triples and their common spellings
// (x86_64-linux-gnu, x86_64-pc-linux-gnu, i686-linux-gnu, x86_64-windows-msvc,
// x86_64-pc-windows-msvc19.44 ...); nullopt for anything else (an unsupported
// target is an error, never a silent fallback to another ABI).
PRISM_API std::optional<Target> parse_target(std::string_view triple);
// The canonical triples parse_target accepts, for error messages and --help.
PRISM_API std::vector<std::string> supported_target_triples();
// The target of a run: data_model ILP32 (SV-COMP) wins, then the triple
// (--target, else PRISM_TARGET, else the host). An unparsable triple yields
// nullopt; callers report it (the CLI exits 2).
PRISM_API std::optional<Target> resolve_target(std::string_view triple, std::string_view data_model);

// The target of the running analysis, for the source-level width tables
// (interval, interp, astlint). Set for a pipeline run by ActiveTargetScope;
// outside one it is the host target.
PRISM_API const Target& active_target();
class PRISM_API ActiveTargetScope {
public:
    explicit ActiveTargetScope(TargetKind k);
    ~ActiveTargetScope();
    ActiveTargetScope(const ActiveTargetScope&) = delete;
    ActiveTargetScope& operator=(const ActiveTargetScope&) = delete;

private:
    int prev_;
};

}  // namespace prism
