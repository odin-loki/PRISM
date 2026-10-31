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
#include <set>

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

const std::set<std::string>& pir_win_c_files() {
    static const std::set<std::string> k = {
        "asm_contract.c", "calls.c",       "casts.c",      "div0.c",      "folded.c",     "kinduct.c",
        "loops.c",        "mem_alias.c",   "mem_array.c",  "mem_contract.c", "mem_ptr.c", "overflow.c",
        "select.c",       "shift.c",       "switch.c",     "unencoded.c", "uninit.c",
    };
    return k;
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

std::vector<std::string> lp64_clang_driver_flags() {
    std::vector<std::string> f = lp64_target_flags();
#ifdef _WIN32
    const std::filesystem::path shim =
        std::filesystem::path(__FILE__).parent_path() / "models" / "libc" / "shim";
    std::error_code ec;
    if (std::filesystem::is_directory(shim, ec)) {
        f.push_back("-idirafter");
        f.push_back(shim.string());
    }
#endif
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
#ifdef _WIN32
    if (!lp64_clang_probe(*fe.clang)) {
        if (notrun_reason) {
            *notrun_reason =
                "NOTRUN: pir needs clang to lower for target " + std::string(lp64_target_triple())
                + " (LP64); probe failed (install LLVM 18 clang with that target)";
        }
        return false;
    }
#endif
    return true;
}

bool pir_test_c_file_supported(const char* basename) {
#ifndef _WIN32
    (void)basename;
    return true;
#else
    if (!basename) return false;
    return pir_win_c_files().count(basename) > 0;
#endif
}

bool pir_cxx_usable(const Config& cfg, std::string* notrun_reason) {
#ifdef _WIN32
    if (notrun_reason) {
        *notrun_reason = "NOTRUN: C++ PIR needs libstdc++ (not available on Windows)";
    }
    return false;
#else
    if (!frontend_usable(cfg, notrun_reason)) return false;
    auto fe = find_frontend(cfg);
    if (!fe.clangxx) {
        if (notrun_reason) *notrun_reason = "NOTRUN: clang++ not on PATH (C++ PIR)";
        return false;
    }
    return true;
#endif
}

}  // namespace prism::pir
