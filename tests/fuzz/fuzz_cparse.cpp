// libFuzzer target for PRISM's own input handling (roadmap 6.2).
//
// One binary, three parsers; the first input byte picks the target so a
// single corpus directory can hold all three kinds of input (set
// PRISM_FUZZ_TARGET=cparse|pir|report to force one target and spend the
// whole budget on it):
//
//   0  cparse: prism::extract_functions on a C/C++ source file
//   1  pir:    prism::pir::ir::parse_module on LLVM IR text
//   2  report: prism::RunReport::load on report.json, then
//              prism::journal_read_stages / journal_read_functions on the same
//              bytes as stages.jsonl / functions.json
//
// The contract under test: arbitrary bytes never crash, hang or corrupt
// memory. A parser may reject input by returning empty/nullopt or by throwing
// a std::exception (the callers catch those; pir::ir::parse_module reports
// malformed IR by throwing). Anything else -- a sanitizer report, an abort,
// a non-std exception, a timeout -- is a finding for docs/FUZZ_SELF.md.
//
// Build: cmake -DPRISM_FUZZ=ON ... && cmake --build <dir> --target prism_fuzz_self
// Run:   <dir>/prism_fuzz_self -max_total_time=300 corpus/   (tools/fuzz_self/run_cpp.sh)
#include "prism/cparse.hpp"
#include "prism/journal.hpp"
#include "prism/models.hpp"
#include "prism/pir.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// One private scratch directory per process; files are overwritten per input.
const fs::path& scratch() {
    static const fs::path dir = [] {
        auto d = fs::temp_directory_path() / ("prism-fuzz-self-" + std::to_string(::getpid()));
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

void write_file(const fs::path& p, std::string_view data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void fuzz_cparse(std::string_view data) {
    const auto p = scratch() / "input.c";
    write_file(p, data);
    auto fns = prism::extract_functions(p, "input.c");
    for (auto& f : fns) {
        (void)f.name.size();
        (void)f.body.size();
    }
    (void)prism::parse_gaps(p);
}

void fuzz_pir(std::string_view data) {
    try {
        auto m = prism::pir::ir::parse_module(data);
        (void)m.functions.size();
    } catch (const std::exception&) {
        // documented rejection of malformed IR
    }
}

void fuzz_report(std::string_view data) {
    const auto dir = scratch() / "out";
    fs::create_directories(dir);
    write_file(dir / "report.json", data);
    write_file(dir / "stages.jsonl", data);
    write_file(dir / "functions.json", data);
    auto r = prism::RunReport::load(dir / "report.json");
    if (r) (void)r->stages.size();
    (void)prism::journal_read_stages(dir);
    (void)prism::journal_read_functions(dir);
}

int forced_target() {
    const char* e = std::getenv("PRISM_FUZZ_TARGET");
    if (!e) return -1;
    if (std::strcmp(e, "cparse") == 0) return 0;
    if (std::strcmp(e, "pir") == 0) return 1;
    if (std::strcmp(e, "report") == 0) return 2;
    return -1;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const int forced = forced_target();
    if (size == 0) return 0;
    std::string_view body(reinterpret_cast<const char*>(data) + 1, size - 1);
    switch (forced >= 0 ? forced : data[0] % 3) {
        case 0: fuzz_cparse(body); break;
        case 1: fuzz_pir(body); break;
        default: fuzz_report(body); break;
    }
    return 0;
}
