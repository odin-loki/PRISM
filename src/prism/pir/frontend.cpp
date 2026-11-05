#include <atomic>
#include "prism/exec.hpp"
#include "prism/pir/frontend.hpp"

#include "prism/abi_model.hpp"
#include "prism/pir.hpp"

#include "prism/exec.hpp"
#include "prism/sandbox.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <optional>
#include <set>
#include <cstdlib>

namespace prism::pir {
namespace {

// One clang run for the frontend target: does it lower a C file (that
// includes the target's C library headers when with_libc), and is the IR for
// that triple? "" when it does, else the first line of why not. Cached per
// clang, flags and target (frontend_usable is asked once per unit and test).
std::string target_clang_probe(const std::filesystem::path& clang, const std::vector<std::string>& flags,
                               const std::string& triple_prefix, bool with_libc) {
    namespace fs = std::filesystem;
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    std::string key = clang.string() + "|" + triple_prefix + (with_libc ? "|libc" : "");
    for (auto& f : flags) key += "|" + f;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    // Windows clang with stdin "-" writes "-.ll" on disk instead of IR on stdout; use temp files.
    std::error_code ec;
    std::random_device rd;
    auto dir = fs::temp_directory_path() / ("prism_target_probe_" + std::to_string(rd()));
    fs::create_directories(dir, ec);
    std::string why;
    if (ec) why = "cannot create a temporary directory";
    auto src = dir / "probe.c";
    auto out = dir / "probe.ll";
    if (why.empty()) {
        std::ofstream f(src, std::ios::binary);
        if (with_libc) f << "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
        f << "int main(void){return 0;}\n";
        if (!f) why = "cannot write the probe source";
    }
    if (why.empty()) {
        std::vector<std::string> argv{clang.string()};
        argv.insert(argv.end(), flags.begin(), flags.end());
        argv.insert(argv.end(), {"-S", "-emit-llvm", "-x", "c", "-o", out.string(), src.string()});
        auto r = prism::exec::run_process(argv, 30.0);
        std::string ir;
        if (!r.failed && !r.timed_out && r.rc == 0) {
            std::ifstream in(out, std::ios::binary);
            ir.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (r.failed || r.timed_out) {
            why = r.timed_out ? "clang timed out" : "clang could not be started";
        } else if (r.rc != 0) {
            std::string line;
            for (size_t p = 0; p < r.text.size();) {
                auto q = r.text.find('\n', p);
                std::string l = r.text.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (l.find("error") != std::string::npos) {
                    line = l;
                    break;
                }
                if (q == std::string::npos) break;
                p = q + 1;
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            why = line.empty() ? "clang exited " + std::to_string(r.rc) : line;
        } else if (ir.find("target triple = \"" + triple_prefix) == std::string::npos) {
            why = "clang did not emit IR for " + triple_prefix;
        }
    }
    fs::remove_all(dir, ec);
    std::lock_guard<std::mutex> lock(mu);
    cache[key] = why;
    return why;
}

}  // namespace

const char* lp64_target_triple() {
    static constexpr char k[] = "x86_64-unknown-linux-gnu";
    return k;
}

std::string lp64_llvm_triple_metadata_line() {
    return std::string("target triple = \"") + lp64_target_triple() + "\"";
}

std::vector<std::string> lp64_target_flags() {
#ifdef _WIN32
    return {"--target=" + std::string(lp64_target_triple())};
#else
    return {};
#endif
}

namespace {
// TargetKind of the running pir lowering; -1 = Linux LP64, the frontend's
// default outside a pir run (svcomp debug IR, tests)
std::atomic<int> g_target{-1};

const Target& target_ref(TargetKind k) {
    static const Target all[] = {target_of(TargetKind::linux_lp64), target_of(TargetKind::linux_ilp32),
                                 target_of(TargetKind::windows_llp64)};
    return all[static_cast<int>(k)];
}
}  // namespace
const Target& frontend_target() {
    const int k = g_target.load();
    return target_ref(k < 0 ? TargetKind::linux_lp64 : static_cast<TargetKind>(k));
}
bool frontend_ilp32() { return frontend_target().kind == TargetKind::linux_ilp32; }
bool frontend_msvc() { return frontend_target().msvc(); }
const char* ilp32_target_triple() { return "i386-unknown-linux-gnu"; }
const char* msvc_target_triple() { return "x86_64-pc-windows-msvc"; }
const char* frontend_target_triple() { return frontend_target().triple.c_str(); }
std::vector<std::string> frontend_target_flags() {
    switch (frontend_target().kind) {
        case TargetKind::linux_ilp32: return {"--target=" + std::string(ilp32_target_triple())};
        case TargetKind::linux_lp64: return lp64_target_flags();
        case TargetKind::windows_llp64: break;
    }
    // Windows x64 (LLP64) with the UCRT and MSVC STL headers. clang finds
    // Visual Studio and the Windows SDK by itself on a Windows host;
    // elsewhere PRISM_MSVC_SYSROOT names a root holding both (the layout of
    // clang's /winsysroot). _NO_CRT_STDIO_INLINE keeps printf and friends
    // plain library calls (the UCRT otherwise defines them inline over
    // __stdio_common_vfprintf, which has no model).
    // -fno-wrapv: clang's MSVC driver adds -fwrapv on its own, which makes
    // signed overflow defined in the IR (no nsw) and so would silently drop
    // every signed-overflow check (Law 8). C and C++ leave it undefined on
    // Windows too, and MSVC's optimizer relies on that.
    std::vector<std::string> f{"--target=" + std::string(msvc_target_triple()), "-fno-wrapv", "-D_NO_CRT_STDIO_INLINE",
                               "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_WARNINGS"};
    if (const char* e = std::getenv("PRISM_MSVC_SYSROOT"); e && *e) {
        f.push_back("-Xmicrosoft-windows-sys-root");
        f.push_back(e);
    }
    return f;
}
FrontendTargetScope::FrontendTargetScope(bool ilp32)
    : prev_(g_target.exchange(static_cast<int>(ilp32 ? TargetKind::linux_ilp32 : TargetKind::linux_lp64))) {}
FrontendTargetScope::FrontendTargetScope(TargetKind k) : prev_(g_target.exchange(static_cast<int>(k))) {}
FrontendTargetScope::~FrontendTargetScope() { g_target.store(prev_); }

std::optional<std::filesystem::path> linux_sysroot() {
#ifdef __linux__
    return std::nullopt;  // native glibc + libstdc++
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    if (const char* e = std::getenv("PRISM_LINUX_SYSROOT"); e && *e) {
        if (fs::is_directory(fs::path(e) / "usr" / "include", ec)) return fs::path(e);
        return std::nullopt;
    }
    // scripts/fetch_toolchain.py linux-sysroot (third_party/TOOLCHAIN_PINS.toml):
    // ~/.prism/toolchain/linux-sysroot/<version>/root
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    if (!home || !*home) return std::nullopt;
    const fs::path base = fs::path(home) / ".prism" / "toolchain" / "linux-sysroot";
    std::optional<fs::path> best;
    for (auto it = fs::directory_iterator(base, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (fs::exists(it->path() / ".verified", ec) && fs::is_directory(it->path() / "root" / "usr" / "include", ec))
            if (!best || it->path().filename() > best->parent_path().filename()) best = it->path() / "root";
    }
    return best;
#endif
}

std::vector<std::string> lp64_clang_driver_flags() {
    std::vector<std::string> f = frontend_target_flags();
    if (auto sr = frontend_msvc() ? std::nullopt : linux_sysroot()) {
        f.push_back("--sysroot=" + sr->string());
        // i386 on an x86_64 Ubuntu root: like gcc -m32, glibc's bits/ live in the
        // x86_64 multiarch dir (libc6-dev-i386 adds gnu/stubs-32.h there)
        if (frontend_ilp32()) {
            f.push_back("-isystem");
            f.push_back((*sr / "usr" / "include" / "x86_64-linux-gnu").string());
        }
    }
    // C23 headers the frontend's libc may lack (<stdbit.h>); searched after
    // the system headers, so a libc that has them wins
    {
        const std::filesystem::path c23 =
            std::filesystem::path(__FILE__).parent_path() / "models" / "libc" / "c23";
        std::error_code ec2;
        if (std::filesystem::is_directory(c23, ec2)) {
            f.push_back("-idirafter");
            f.push_back(c23.string());
        }
    }
    return f;
}

std::string lowerswitch_pass(const std::filesystem::path& opt) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    const std::string key = opt.string();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    std::string name = "lowerswitch";
    auto r = prism::exec::run_process({key, "--print-passes"}, 30.0);
    if (!r.failed && !r.timed_out && r.rc == 0 && r.text.find("lowerswitch") == std::string::npos &&
        r.text.find("lower-switch") != std::string::npos)
        name = "lower-switch";
    std::lock_guard<std::mutex> lock(mu);
    cache[key] = name;
    return name;
}

std::string c_ir_passes(const std::filesystem::path& opt) {
    return "-passes=mem2reg," + lowerswitch_pass(opt) + ",loop-simplify,lcssa,instnamer";
}

bool frontend_usable(const Config& cfg, std::string* notrun_reason) {
    // asked for the target cfg's pir run would lower for (--target, data model)
    const auto t = resolve_target(cfg.target, cfg.data_model);
    if (!t) {
        if (notrun_reason) *notrun_reason = "NOTRUN: unsupported target '" + cfg.target + "'";
        return false;
    }
    FrontendTargetScope scope(t->kind);
    auto fe = find_frontend(cfg);
    if (!fe.clang || !fe.opt) {
        if (notrun_reason) {
            *notrun_reason = std::string("NOTRUN: pir needs clang and opt on PATH (clang ")
                             + (fe.clang ? "found" : "missing") + ", opt "
                             + (fe.opt ? "found" : "missing") + ")";
        }
        return false;
    }
    if (frontend_msvc()) {
        // Windows x64 target: the UCRT / Windows SDK headers must be found
        auto why = target_clang_probe(*fe.clang, lp64_clang_driver_flags(), msvc_target_triple(), true);
        if (!why.empty()) {
            if (notrun_reason)
                *notrun_reason = std::string("NOTRUN: pir lowers for ") + msvc_target_triple() +
                                 " (LLP64) and needs the MSVC and Windows SDK headers (install Visual Studio or "
                                 "the Build Tools with the Windows SDK; off Windows set PRISM_MSVC_SYSROOT); "
                                 "clang probe: " + why;
            return false;
        }
        return true;
    }
#ifndef __linux__
    if (!linux_sysroot()) {
        if (notrun_reason)
            *notrun_reason = std::string("NOTRUN: pir lowers for ") + frontend_target_triple() +
                             " and needs the pinned Linux sysroot (python scripts/fetch_toolchain.py linux-sysroot, "
                             "or PRISM_LINUX_SYSROOT)";
        return false;
    }
#endif
#ifdef _WIN32
    if (auto why = target_clang_probe(*fe.clang, frontend_target_flags(), frontend_target_triple(), false);
        !why.empty()) {
        if (notrun_reason) {
            *notrun_reason = std::string("NOTRUN: pir needs clang to lower for target ") + frontend_target_triple() +
                             "; probe failed: " + why + " (install the pinned LLVM clang: scripts/fetch_llvm.py)";
        }
        return false;
    }
#endif
    return true;
}

bool pir_cxx_usable(const Config& cfg, std::string* notrun_reason) {
    if (!frontend_usable(cfg, notrun_reason)) return false;
    auto fe = find_frontend(cfg);
    if (!fe.clangxx) {
        if (notrun_reason) *notrun_reason = "NOTRUN: clang++ not on PATH (C++ PIR)";
        return false;
    }
    return true;
}

}  // namespace prism::pir
