#include <atomic>
#include "prism/exec.hpp"
#include "prism/pir/frontend.hpp"

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

#ifdef _WIN32
constexpr const char* kProbeSrc = "int main(void){return 0;}\n";
bool lp64_clang_probe(const std::filesystem::path& clang) {
    // Windows clang with stdin "-" writes "-.ll" on disk instead of IR on stdout; use temp files.
    namespace fs = std::filesystem;
    std::error_code ec;
    std::random_device rd;
    auto dir = fs::temp_directory_path() / ("prism_lp64_probe_" + std::to_string(rd()));
    fs::create_directories(dir, ec);
    if (ec) return false;
    auto src = dir / "probe.c";
    auto out = dir / "probe.ll";
    {
        std::ofstream f(src, std::ios::binary);
        if (!f) {
            fs::remove_all(dir, ec);
            return false;
        }
        f << kProbeSrc;
    }
    auto r = prism::exec::run_process({clang.string(), "--target=" + std::string(lp64_target_triple()), "-S", "-emit-llvm", "-x",
                                  "c", "-o", out.string(), src.string()},
                                 20.0);
    std::string ir;
    if (!r.failed && !r.timed_out && r.rc == 0) {
        std::ifstream in(out, std::ios::binary);
        ir.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    fs::remove_all(dir, ec);
    if (r.failed || r.timed_out || r.rc != 0 || ir.empty()) return false;
    const std::string want = lp64_llvm_triple_metadata_line();
    return ir.find(want) != std::string::npos;
}

#endif

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
std::atomic<bool> g_ilp32{false};
}
bool frontend_ilp32() { return g_ilp32.load(); }
const char* ilp32_target_triple() { return "i386-unknown-linux-gnu"; }
const char* frontend_target_triple() { return frontend_ilp32() ? ilp32_target_triple() : lp64_target_triple(); }
std::vector<std::string> frontend_target_flags() {
    if (frontend_ilp32()) return {"--target=" + std::string(ilp32_target_triple())};
    return lp64_target_flags();
}
FrontendTargetScope::FrontendTargetScope(bool ilp32) : prev_(g_ilp32.exchange(ilp32)) {}
FrontendTargetScope::~FrontendTargetScope() { g_ilp32.store(prev_); }

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
    if (auto sr = linux_sysroot()) {
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
    auto fe = find_frontend(cfg);
    if (!fe.clang || !fe.opt) {
        if (notrun_reason) {
            *notrun_reason = std::string("NOTRUN: pir needs clang and opt on PATH (clang ")
                             + (fe.clang ? "found" : "missing") + ", opt "
                             + (fe.opt ? "found" : "missing") + ")";
        }
        return false;
    }
#ifndef __linux__
    if (!linux_sysroot()) {
        if (notrun_reason)
            *notrun_reason = "NOTRUN: pir lowers for x86_64-unknown-linux-gnu and needs the pinned Linux "
                             "sysroot (python scripts/fetch_toolchain.py linux-sysroot, or PRISM_LINUX_SYSROOT)";
        return false;
    }
#endif
#ifdef _WIN32
    if (!lp64_clang_probe(*fe.clang)) {
        if (notrun_reason) {
            *notrun_reason =
                "NOTRUN: pir needs clang to lower for target " + std::string(lp64_target_triple())
                + " (LP64); probe failed (install the pinned LLVM clang: scripts/fetch_llvm.py)";
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
