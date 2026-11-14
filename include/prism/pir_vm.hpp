#pragma once

// PIR bytecode VM and input sweep (docs/GPU_PLAN.md G1).
//
// compile() flattens a scalar PIR function into bytecode; sweep() runs it on
// many inputs at once, on the GPU when the CUDA driver and the embedded PTX
// are there (loaded through FORGE's dl, device memory charged to FORGE's
// governor pool "cuda:0"), else on the CPU twin over FORGE's task pool. Both
// run the same VM source (src/prism/pir/vm_core.inc).
//
// A sweep can only refute: its smallest violating input is replayed in
// pir::interpret by the caller before anything is reported (confirm()). A
// sweep that finds nothing is coverage, never a proof (Law 3), even when it
// ran every input.

#include "prism/export.hpp"
#include "prism/pir.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prism::pir::vm {

struct Insn {
    uint8_t op = 0, w = 0, xw = 0, pad = 0;
    uint16_t dst = 0, a = 0, b = 0, c = 0;
    uint32_t imm = 0;
};
static_assert(sizeof(Insn) == 16);

struct Prop {
    std::string prop, cls, msg;
    int line = 0;
};

struct Program {
    std::vector<Insn> code;
    std::vector<uint64_t> init;      // one per register: constants, 0 elsewhere
    std::vector<uint16_t> param_reg;
    std::vector<uint8_t> param_w;
    std::vector<Prop> props;         // CHECK imm -> property
    unsigned input_bits = 0;         // sum of parameter widths
    std::string text_hash;           // sha256 of pir::to_text of the compiled function
};

// nullopt with *why set when the function is outside the VM's fragment
// (memory, floating point, havoc / nondet, wider than 64 bits, too large).
// only_props: as CheckOptions::only_props (empty: every property).
PRISM_API std::optional<Program> compile(const Function& fn, const std::vector<std::string>& only_props,
                                         std::string* why);

enum class Device { Auto, Gpu, Cpu, Off };
PRISM_API Device device_from_string(const std::string& s);  // "auto" "gpu"/"cuda" "cpu" "off"/"0"

struct SweepOptions {
    Device device = Device::Auto;
    unsigned exhaustive_bits = 0;    // 0: the device default (GPU 32, CPU 20)
    uint64_t samples = 0;            // 0: the device default (GPU 2^24, CPU 2^16)
    uint64_t seed = 0x5052495354ull;
    uint32_t step_limit = 100000;    // VM instructions per lane
    double budget_s = 5.0;           // wall time for one sweep
    int jobs = 0;                    // CPU twin workers (0: all hardware threads)
};

struct SweepResult {
    bool ran = false;
    std::string engine;              // "gpu (NVIDIA GeForce RTX 3090, 82 SMs)" / "cpu (64 threads)"
    std::string gpu_note;            // why the GPU was not used (empty when it was)
    bool exhaustive = false;         // every input was run
    uint64_t lanes = 0;              // inputs run
    uint64_t counts[6] = {};         // by status: returned stopped violation assume steps soft
    std::optional<uint64_t> first_violation;  // smallest violating lane
    std::vector<uint64_t> args;      // its parameter values
    uint32_t prop = 0;               // its property index (Program::props)
    double seconds = 0;
    std::string summary() const;     // one line for extra.sweep
};

PRISM_API SweepResult sweep(const Program& p, const SweepOptions& o);

// Runs one input on the CPU twin: status (0 returned, 1 stopped, 2 violation,
// 3 assume failed, 4 step limit, 5 unmodelled) and the property index.
PRISM_API uint32_t run_one(const Program& p, const std::vector<uint64_t>& args, uint32_t step_limit, uint32_t* prop);

// The GPU as the VM sees it: available (driver, device, PTX module) and a
// one-line description or the reason it is not.
struct GpuInfo {
    bool available = false;
    std::string detail;
};
PRISM_API GpuInfo gpu_info();

// The sweep's violation replayed in pir::interpret on fn (only_props applied):
// true when the interpreter reports the same property at the same line.
PRISM_API bool confirm(const Function& fn, const std::vector<std::string>& only_props, const Program& p,
                       const SweepResult& r);

}  // namespace prism::pir::vm
