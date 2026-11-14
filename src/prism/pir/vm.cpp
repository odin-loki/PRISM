// PIR bytecode VM: compiler, CPU twin (FORGE task pool), CUDA driver runtime
// (FORGE dl + governor) and the input sweep. docs/GPU_PLAN.md G1.

#include "prism/pir_vm.hpp"

#include "prism/pool.hpp"
#include "fp.hpp"

#include <forge/core/dl.hpp>
#include <forge/core/memory.hpp>

// FORGE os headers may pull in <windows.h>; its ERROR macro breaks laws::ERROR.
#if defined(_WIN32) && defined(ERROR)
#  undef ERROR
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace prism::pir::vm {

#define PRISM_VM_FN inline
#include "vm_core.inc"
#undef PRISM_VM_FN

static_assert(sizeof(VmInsn) == sizeof(Insn));
static_assert(static_cast<int>(Op::Copy) == VM_COPY && static_cast<int>(Op::Havoc) == VM_HAVOC);
static_assert(static_cast<int>(Op::Add) == VM_ADD && static_cast<int>(Op::Xor) == VM_XOR);
static_assert(static_cast<int>(Op::Eq) == VM_EQ && static_cast<int>(Op::Sge) == VM_SGE);
static_assert(static_cast<int>(Op::Select) == VM_SELECT && static_cast<int>(Op::Trunc) == VM_TRUNC);
static_assert(static_cast<int>(Op::SMax) == VM_SMAX && static_cast<int>(Op::Bswap) == VM_BSWAP);
static_assert(static_cast<int>(Op::SAddOvf) == VM_SADDOVF && static_cast<int>(Op::UMulOvf) == VM_UMULOVF);
static_assert(static_cast<int>(Op::SDivOvf) == VM_SDIVOVF && static_cast<int>(Op::InexactS) == VM_INEXACTS);

namespace {

// Register counts the device kernels hold (src/cuda/pir_vm.cu); a larger
// program runs on the CPU twin only.
constexpr uint32_t kGpuRegTiers[] = {64, 256, 1024};
constexpr uint32_t kMaxRegs = 65535;

bool soft_prop(const std::string& p) { return p == "throw-unmodelled" || p == "unmodelled"; }

}  // namespace

std::optional<Program> compile(const Function& fn, const std::vector<std::string>& only_props, std::string* why) {
    auto fail = [&](std::string w) -> std::optional<Program> {
        if (why) *why = std::move(w);
        return std::nullopt;
    };
    if (fn.uses_memory) return fail("memory statements (the VM has no memory model yet, GPU_PLAN G4)");
    if (fn.nondet) return fail("nondet source");
    if (fn.blocks.empty()) return fail("no blocks");
    for (auto& v : fn.vars) {
        if (v.width == 0 || v.width > 64) return fail("a value wider than 64 bits");
        if (v.fp) return fail("floating point");
    }
    Program p;
    const uint32_t nv = static_cast<uint32_t>(fn.vars.size());
    std::map<uint64_t, uint32_t> consts;
    uint32_t nregs = nv;
    std::vector<uint64_t> cvals;
    auto creg = [&](uint64_t bits) -> uint32_t {
        auto [it, fresh] = consts.emplace(bits, nregs);
        if (fresh) {
            ++nregs;
            cvals.push_back(bits);
        }
        return it->second;
    };
    std::string bad;
    auto reg = [&](const Arg& a) -> uint32_t {
        if (a.is_const) return creg(a.bits);
        if (a.var < 0 || static_cast<uint32_t>(a.var) >= nv) bad = "argument out of range";
        return a.var < 0 ? 0u : static_cast<uint32_t>(a.var);
    };
    std::size_t max_phis = 0;
    for (auto& b : fn.blocks) max_phis = std::max(max_phis, b.phis.size());

    // Pass 1 emits code with temporaries numbered from kTemp; registers are
    // renumbered once the constant count is known.
    constexpr uint32_t kTemp = 0x80000000u;
    struct Raw {
        Insn i;
        uint32_t dst = 0, a = 0, b = 0, c = 0;
        int target = -1;  // JMP / BR: block index (patched to its pc)
        int local = -1;   // BR: index into code of its true-edge segment (patched)
    };
    std::vector<Raw> code;
    std::vector<uint32_t> start(fn.blocks.size(), 0);
    auto emit = [&](uint8_t op, uint8_t w, uint8_t xw, uint32_t dst, uint32_t a, uint32_t b, uint32_t c,
                    uint32_t imm = 0) -> std::size_t {
        Raw r;
        r.i.op = op;
        r.i.w = w;
        r.i.xw = xw;
        r.i.imm = imm;
        r.dst = dst;
        r.a = a;
        r.b = b;
        r.c = c;
        code.push_back(r);
        return code.size() - 1;
    };
    auto valid = [&](int b) { return b >= 0 && b < static_cast<int>(fn.blocks.size()); };
    // phi copies on the edge from -> to (parallel assignment through temps)
    auto edge = [&](int from, int to) {
        if (!valid(to)) return;
        std::vector<std::pair<uint32_t, uint32_t>> mv;  // (dst, src)
        for (auto& ph : fn.blocks[static_cast<std::size_t>(to)].phis)
            for (auto& [pred, a] : ph.in)
                if (pred == from) {
                    mv.push_back({static_cast<uint32_t>(ph.dst), reg(a)});
                    break;
                }
        if (mv.size() == 1) {
            emit(VM_COPY, 64, 64, mv[0].first, mv[0].second, 0, 0);
            return;
        }
        for (std::size_t k = 0; k < mv.size(); ++k) emit(VM_COPY, 64, 64, kTemp + static_cast<uint32_t>(k), mv[k].second, 0, 0);
        for (std::size_t k = 0; k < mv.size(); ++k) emit(VM_COPY, 64, 64, mv[k].first, kTemp + static_cast<uint32_t>(k), 0, 0);
    };
    auto jump = [&](int to) {
        if (!valid(to)) {
            emit(VM_STOP, 0, 0, 0, 0, 0, 0);
            return;
        }
        auto k = emit(VM_JMP, 0, 0, 0, 0, 0, 0);
        code[k].target = to;
    };
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto& bl = fn.blocks[bi];
        start[bi] = static_cast<uint32_t>(code.size());
        for (auto& s : bl.stmts) {
            switch (s.kind) {
                case Stmt::Assign: {
                    const int op = static_cast<int>(s.op);
                    if (s.op == Op::Havoc) return fail("havoc (an unconstrained value)");
                    if (op > VM_LAST_ALU || fp::is_fp_op(s.op)) return fail(std::string("operator ") + op_name(s.op));
                    if (s.dst < 0 || static_cast<uint32_t>(s.dst) >= nv) return fail("assignment target out of range");
                    if (s.args.size() > 3) return fail("operator with more than three arguments");
                    const unsigned w = fn.vars[static_cast<std::size_t>(s.dst)].width;
                    const unsigned xw = s.args.empty() ? w : s.args[0].width;
                    if (xw == 0 || xw > 64) return fail("argument wider than 64 bits");
                    uint32_t r[3] = {0, 0, 0};
                    for (std::size_t k = 0; k < s.args.size(); ++k) r[k] = reg(s.args[k]);
                    emit(static_cast<uint8_t>(op), static_cast<uint8_t>(w), static_cast<uint8_t>(xw),
                         static_cast<uint32_t>(s.dst), r[0], r[1], r[2]);
                    break;
                }
                case Stmt::Check: {
                    if (s.args.empty()) return fail("check without a condition");
                    if (soft_prop(s.prop)) {
                        emit(VM_SOFT, 0, 0, 0, reg(s.args[0]), 0, 0);
                        break;
                    }
                    if (!only_props.empty() &&
                        std::find(only_props.begin(), only_props.end(), s.prop) == only_props.end())
                        break;  // check_function drops it too
                    emit(VM_CHECK, 0, 0, 0, reg(s.args[0]), 0, 0, static_cast<uint32_t>(p.props.size()));
                    p.props.push_back(Prop{s.prop, s.cls, s.msg, s.line});
                    break;
                }
                case Stmt::Assume:
                    if (s.args.empty()) return fail("assume without a condition");
                    emit(VM_ASSUME, 0, 0, 0, reg(s.args[0]), 0, 0);
                    break;
                default: return fail("memory statement");
            }
        }
        const int b = static_cast<int>(bi);
        switch (bl.term.kind) {
            case Term::Jmp:
                edge(b, bl.term.t);
                jump(bl.term.t);
                break;
            case Term::Br: {
                auto k = emit(VM_BR, 0, 0, 0, reg(bl.term.cond), 0, 0);
                edge(b, bl.term.f);
                jump(bl.term.f);
                code[k].local = static_cast<int>(code.size());
                edge(b, bl.term.t);
                jump(bl.term.t);
                break;
            }
            case Term::Ret: emit(VM_RET, 0, 0, 0, 0, 0, 0); break;
            case Term::Stop: emit(VM_STOP, 0, 0, 0, 0, 0, 0); break;
        }
    }
    if (!bad.empty()) return fail(bad);
    const uint32_t ntemp = static_cast<uint32_t>(max_phis > 1 ? max_phis : 0);
    const uint32_t temp_base = nregs;
    const uint64_t total = uint64_t{nregs} + ntemp;
    if (total == 0 || total > kMaxRegs) return fail("more than 65535 registers");
    if (code.size() > 0x7fffffffu) return fail("program too large");
    auto fix = [&](uint32_t r) { return static_cast<uint16_t>(r >= kTemp ? temp_base + (r - kTemp) : r); };
    p.code.reserve(code.size());
    for (auto& r : code) {
        Insn i;
        std::memcpy(&i, &r.i, sizeof i);
        i.dst = fix(r.dst);
        i.a = fix(r.a);
        i.b = fix(r.b);
        i.c = fix(r.c);
        if (r.target >= 0) i.imm = start[static_cast<std::size_t>(r.target)];
        if (r.local >= 0) i.imm = static_cast<uint32_t>(r.local);
        p.code.push_back(i);
    }
    p.init.assign(static_cast<std::size_t>(total), 0);
    for (std::size_t k = 0; k < cvals.size(); ++k) p.init[nv + k] = cvals[k];
    for (int v : fn.params) {
        if (v < 0 || static_cast<uint32_t>(v) >= nv) return fail("parameter out of range");
        p.param_reg.push_back(static_cast<uint16_t>(v));
        p.param_w.push_back(static_cast<uint8_t>(fn.vars[static_cast<std::size_t>(v)].width));
        p.input_bits += fn.vars[static_cast<std::size_t>(v)].width;
    }
    p.text_hash = sha256_hex(to_text(fn));
    return p;
}

Device device_from_string(const std::string& s) {
    if (s == "gpu" || s == "cuda") return Device::Gpu;
    if (s == "cpu") return Device::Cpu;
    if (s == "off" || s == "0" || s == "none") return Device::Off;
    return Device::Auto;
}

namespace {

struct LaunchData {
    std::vector<uint8_t> poff;
    VmLaunch L{};
};

LaunchData launch_for(const Program& p, bool exhaustive, uint64_t seed, uint64_t edge_lanes, uint32_t step_limit) {
    LaunchData d;
    unsigned off = 0;
    for (auto w : p.param_w) {
        d.poff.push_back(static_cast<uint8_t>(std::min(off, 63u)));
        off += w;
    }
    auto& L = d.L;
    L.code = reinterpret_cast<const VmInsn*>(p.code.data());
    L.init = p.init.data();
    L.preg = p.param_reg.data();
    L.pw = p.param_w.data();
    L.poff = d.poff.data();
    L.ncode = static_cast<vm_u32>(p.code.size());
    L.nregs = static_cast<vm_u32>(p.init.size());
    L.nparams = static_cast<vm_u32>(p.param_reg.size());
    L.step_limit = step_limit;
    L.exhaustive = exhaustive ? 1u : 0u;
    L.seed = seed;
    L.edge_lanes = edge_lanes;
    return d;
}

uint64_t edge_lanes_for(const Program& p, uint64_t samples) {
    uint64_t n = 1;
    for (std::size_t k = 0; k < p.param_w.size(); ++k) {
        if (n > samples / VM_EDGES) return 0;  // the product does not fit: random lanes only
        n *= VM_EDGES;
    }
    return std::min(n, samples);
}

// CPU twin: one chunk [lane0, lane0 + n) on FORGE's task pool.
void cpu_chunk(const VmLaunch& L0, uint64_t lane0, uint64_t n, int jobs, uint64_t* out) {
    std::atomic<uint64_t> best{out[0]};
    std::mutex m;
    const std::size_t grain = 1024;
    prism::pool::parallel_for(jobs, static_cast<std::size_t>(n), grain, [&](std::size_t b, std::size_t e) {
        std::vector<vm_u64> regs(L0.nregs);
        uint64_t cnt[VM_S_COUNT] = {};
        for (std::size_t i = b; i < e; ++i) {
            const uint64_t lane = lane0 + i;
            if (lane > best.load(std::memory_order_relaxed)) break;
            vm_u32 prop = 0;
            const vm_u32 st = vm_lane(L0, lane, regs.data(), &prop);
            ++cnt[st];
            if (st == VM_S_VIOLATION) {
                uint64_t cur = best.load();
                while (lane < cur && !best.compare_exchange_weak(cur, lane)) {}
                break;
            }
        }
        std::lock_guard<std::mutex> lk(m);
        for (int s = 0; s < static_cast<int>(VM_S_COUNT); ++s) out[1 + s] += cnt[s];
    });
    out[0] = best.load();
}

// ---------------------------------------------------------------------------
// CUDA driver API through FORGE's dl (no cudart, no nvcc at link time)
// ---------------------------------------------------------------------------

#if __has_include("prism/pir_vm_ptx.inc")
#include "prism/pir_vm_ptx.inc"  // kPirVmPtx[], kPirVmPtxSize (generated by CMake from src/cuda/pir_vm.cu)
#else
const char kPirVmPtx[] = {0};
const std::size_t kPirVmPtxSize = 0;
#endif

using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
using CUfunction = void*;
using CUdeviceptr = unsigned long long;

struct Cuda {
    std::mutex mu;
    bool ok = false;
    std::string why = "not initialised";
    std::string name;
    int sms = 0;
    void* lib = nullptr;
    CUcontext ctx = nullptr;
    CUmodule mod = nullptr;
    CUfunction kern[3] = {};
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuDeviceGetCount)(int*) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*cuDeviceGetName)(char*, int, CUdevice) = nullptr;
    CUresult (*cuDeviceGetAttribute)(int*, int, CUdevice) = nullptr;
    CUresult (*cuDevicePrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
    CUresult (*cuCtxSetCurrent)(CUcontext) = nullptr;
    CUresult (*cuModuleLoadDataEx)(CUmodule*, const void*, unsigned, void*, void**) = nullptr;
    CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (*cuMemAlloc)(CUdeviceptr*, std::size_t) = nullptr;
    CUresult (*cuMemFree)(CUdeviceptr) = nullptr;
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, std::size_t) = nullptr;
    CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, std::size_t) = nullptr;
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void*,
                               void**, void**) = nullptr;
    CUresult (*cuCtxSynchronize)() = nullptr;
    CUresult (*cuGetErrorString)(CUresult, const char**) = nullptr;

    std::string err(CUresult r) const {
        const char* s = nullptr;
        if (cuGetErrorString) cuGetErrorString(r, &s);
        return s ? std::string(s) : "CUDA error " + std::to_string(r);
    }

    void init() {
        if (kPirVmPtxSize == 0) {
            why = "PRISM was built without the PTX kernel (clang with the nvptx64 target not found at configure)";
            return;
        }
        lib = dlopen(FORGE_DLNAME("libcuda.so.1", "nvcuda.dll"), RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            why = "CUDA driver not found (" FORGE_DLNAME("libcuda.so.1", "nvcuda.dll") ")";
            return;
        }
        auto sym = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, n));
            return f != nullptr;
        };
        if (!sym(cuInit, "cuInit") || !sym(cuDeviceGetCount, "cuDeviceGetCount") || !sym(cuDeviceGet, "cuDeviceGet") ||
            !sym(cuDeviceGetName, "cuDeviceGetName") || !sym(cuDeviceGetAttribute, "cuDeviceGetAttribute") ||
            !sym(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain") || !sym(cuCtxSetCurrent, "cuCtxSetCurrent") ||
            !sym(cuModuleLoadDataEx, "cuModuleLoadDataEx") || !sym(cuModuleGetFunction, "cuModuleGetFunction") ||
            !sym(cuMemAlloc, "cuMemAlloc_v2") || !sym(cuMemFree, "cuMemFree_v2") ||
            !sym(cuMemcpyHtoD, "cuMemcpyHtoD_v2") || !sym(cuMemcpyDtoH, "cuMemcpyDtoH_v2") ||
            !sym(cuLaunchKernel, "cuLaunchKernel") || !sym(cuCtxSynchronize, "cuCtxSynchronize")) {
            why = "CUDA driver lacks a required entry point";
            return;
        }
        sym(cuGetErrorString, "cuGetErrorString");
        CUresult r = cuInit(0);
        if (r != 0) {
            why = "cuInit: " + err(r);
            return;
        }
        int n = 0;
        if (cuDeviceGetCount(&n) != 0 || n <= 0) {
            why = "no CUDA device";
            return;
        }
        CUdevice dev = 0;
        cuDeviceGet(&dev, 0);
        char nm[256] = {};
        cuDeviceGetName(nm, sizeof nm - 1, dev);
        name = nm;
        cuDeviceGetAttribute(&sms, 16 /* CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT */, dev);
        if ((r = cuDevicePrimaryCtxRetain(&ctx, dev)) != 0) {
            why = "context: " + err(r);
            return;
        }
        cuCtxSetCurrent(ctx);
        if ((r = cuModuleLoadDataEx(&mod, kPirVmPtx, 0, nullptr, nullptr)) != 0) {
            why = "PTX module did not load (" + err(r) + ")";
            return;
        }
        const char* names[3] = {"prism_pir_vm_64", "prism_pir_vm_256", "prism_pir_vm_1024"};
        for (int k = 0; k < 3; ++k)
            if ((r = cuModuleGetFunction(&kern[k], mod, names[k])) != 0) {
                why = std::string("kernel ") + names[k] + ": " + err(r);
                return;
            }
        ok = true;
        why.clear();
    }
};

Cuda& cuda() {
    static Cuda* c = [] {
        auto* x = new Cuda;  // never destroyed: the driver may be gone at exit
        x->init();
        return x;
    }();
    return *c;
}

// One GPU sweep phase; false (with *why) when a driver call failed.
struct GpuRun {
    Cuda& c;
    const Program& p;
    std::vector<CUdeviceptr> bufs;
    forge::mem::lease lease;
    CUdeviceptr code = 0, init = 0, preg = 0, pw = 0, poff = 0, out = 0;
    CUfunction k = nullptr;
    std::string why;

    GpuRun(Cuda& cc, const Program& pp) : c(cc), p(pp) {}
    ~GpuRun() {
        for (auto b : bufs) c.cuMemFree(b);
    }
    bool alloc(CUdeviceptr& d, const void* src, std::size_t n) {
        n = std::max<std::size_t>(n, 8);
        CUresult r = c.cuMemAlloc(&d, n);
        if (r != 0) {
            why = "cuMemAlloc: " + c.err(r);
            return false;
        }
        bufs.push_back(d);
        if (src && (r = c.cuMemcpyHtoD(d, src, n)) != 0) {
            why = "copy to device: " + c.err(r);
            return false;
        }
        return true;
    }
    bool setup(const std::vector<uint8_t>& offs) {
        const std::size_t nregs = p.init.size();
        int tier = -1;
        for (int t = 0; t < 3; ++t)
            if (nregs <= kGpuRegTiers[t]) {
                tier = t;
                break;
            }
        if (tier < 0) {
            why = "more registers (" + std::to_string(nregs) + ") than the device kernel holds (1024)";
            return false;
        }
        k = c.kern[tier];
        const std::size_t bytes = p.code.size() * sizeof(Insn) + nregs * 8 + p.param_reg.size() * 3 + 64 + 6 * 8;
        // Shared accounting with FORGE inference on the same card.
        lease = forge::mem::governor::instance().pool("cuda:0").try_reserve(bytes, "prism pir vm");
        if (!lease) {
            why = "FORGE device memory pool cuda:0 is full";
            return false;
        }
        return alloc(code, p.code.data(), p.code.size() * sizeof(Insn)) && alloc(init, p.init.data(), nregs * 8) &&
               alloc(preg, p.param_reg.data(), p.param_reg.size() * 2) &&
               alloc(pw, p.param_w.data(), p.param_w.size()) && alloc(poff, offs.data(), offs.size()) &&
               alloc(out, nullptr, 7 * 8);
    }
    bool chunk(const VmLaunch& host, uint64_t lane0, uint64_t n, uint64_t* res) {
        VmLaunch L = host;
        L.code = reinterpret_cast<const VmInsn*>(code);
        L.init = reinterpret_cast<const vm_u64*>(init);
        L.preg = reinterpret_cast<const vm_u16*>(preg);
        L.pw = reinterpret_cast<const vm_u8*>(pw);
        L.poff = reinterpret_cast<const vm_u8*>(poff);
        L.lane0 = lane0;
        L.nlanes = n;
        L.out = reinterpret_cast<vm_u64*>(out);
        uint64_t zero[7] = {~uint64_t{0}, 0, 0, 0, 0, 0, 0};
        CUresult r = c.cuMemcpyHtoD(out, zero, sizeof zero);
        const unsigned threads = 128;
        const uint64_t want = (n + threads - 1) / threads;
        const unsigned blocks = static_cast<unsigned>(std::min<uint64_t>(want, uint64_t(std::max(1, c.sms)) * 16));
        void* args[] = {&L};
        if (r == 0) r = c.cuLaunchKernel(k, std::max(1u, blocks), 1, 1, threads, 1, 1, 0, nullptr, args, nullptr);
        if (r == 0) r = c.cuCtxSynchronize();
        if (r == 0) r = c.cuMemcpyDtoH(res, out, 7 * 8);
        if (r != 0) {
            why = "kernel: " + c.err(r);
            return false;
        }
        return true;
    }
};

}  // namespace

GpuInfo gpu_info() {
    auto& c = cuda();
    if (!c.ok) return {false, c.why};
    return {true, c.name + ", " + std::to_string(c.sms) + " SMs, CUDA driver via FORGE dl"};
}

namespace {

std::vector<uint64_t> args_of(const VmLaunch& L, uint64_t lane) {
    std::vector<uint64_t> a;
    for (vm_u32 k = 0; k < L.nparams; ++k) a.push_back(vm_lane_arg(L, k, lane));
    return a;
}

}  // namespace

uint32_t run_one(const Program& p, const std::vector<uint64_t>& args, uint32_t step_limit, uint32_t* prop) {
    std::vector<vm_u64> r(p.init.begin(), p.init.end());
    for (std::size_t k = 0; k < p.param_reg.size() && k < args.size(); ++k)
        r[p.param_reg[k]] = args[k] & vm_mask(p.param_w[k]);
    vm_u32 pr = 0;
    const vm_u32 st = vm_run(reinterpret_cast<const VmInsn*>(p.code.data()), static_cast<vm_u32>(p.code.size()),
                             r.data(), step_limit, &pr);
    if (prop) *prop = pr;
    return st;
}

std::string SweepResult::summary() const {
    if (!ran) return "not run" + (gpu_note.empty() ? std::string() : ": " + gpu_note);
    std::string s = engine + ": " + std::to_string(lanes) + (exhaustive ? " inputs (every input)" : " inputs (sampled)");
    if (first_violation) s += ", violation at input " + std::to_string(*first_violation);
    else s += ", no property violated";
    if (counts[VM_S_ASSUME]) s += ", " + std::to_string(counts[VM_S_ASSUME]) + " outside the precondition";
    if (counts[VM_S_STEPS]) s += ", " + std::to_string(counts[VM_S_STEPS]) + " hit the step limit";
    if (counts[VM_S_SOFT]) s += ", " + std::to_string(counts[VM_S_SOFT]) + " reached an unmodelled construct";
    char buf[48];
    std::snprintf(buf, sizeof buf, " in %.3f s", seconds);
    s += buf;
    if (!gpu_note.empty()) s += " (GPU not used: " + gpu_note + ")";
    s += "; a sweep is never a proof";
    return s;
}

SweepResult sweep(const Program& p, const SweepOptions& o) {
    SweepResult res;
    if (o.device == Device::Off) {
        res.gpu_note = "PRISM_GPU=off";
        return res;
    }
    auto& c = cuda();
    bool gpu = false;
    if (o.device == Device::Cpu) res.gpu_note = "PRISM_GPU=cpu";
    else if (!c.ok) res.gpu_note = c.why;
    else gpu = true;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    const int jobs = o.jobs > 0 ? o.jobs : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::unique_lock<std::mutex> lk(c.mu, std::defer_lock);
    std::optional<GpuRun> g;
    std::vector<uint8_t> offs;
    {
        unsigned off = 0;
        for (auto w : p.param_w) {
            offs.push_back(static_cast<uint8_t>(std::min(off, 63u)));
            off += w;
        }
    }
    if (gpu) {
        lk.lock();
        c.cuCtxSetCurrent(c.ctx);
        g.emplace(c, p);
        if (!g->setup(offs)) {
            res.gpu_note = g->why;
            g.reset();
            lk.unlock();
            gpu = false;
        }
    }
    res.engine = gpu ? "gpu (" + c.name + ", " + std::to_string(c.sms) + " SMs)"
                     : "cpu (" + std::to_string(jobs) + " threads)";
    const unsigned ex_bits = o.exhaustive_bits ? o.exhaustive_bits : (gpu ? 32u : 20u);
    const uint64_t samples = o.samples ? o.samples : (gpu ? (uint64_t{1} << 24) : (uint64_t{1} << 16));
    const double target = gpu ? 0.05 : 0.1;  // seconds per chunk (keeps the display driver's watchdog away)

    auto phase = [&](bool exhaustive, uint64_t total, bool may_abandon) -> int {  // 1 found, 0 done, -1 abandoned
        const uint64_t edges = exhaustive ? 0 : edge_lanes_for(p, total);
        auto d = launch_for(p, exhaustive, o.seed, edges, o.step_limit);
        uint64_t lane = 0, chunk = gpu ? (uint64_t{1} << 18) : (uint64_t{1} << 12);
        res.lanes = 0;
        std::fill(std::begin(res.counts), std::end(res.counts), 0);
        while (lane < total) {
            const uint64_t n = std::min(chunk, total - lane);
            uint64_t out[7] = {~uint64_t{0}, 0, 0, 0, 0, 0, 0};
            const auto c0 = std::chrono::steady_clock::now();
            if (gpu) {
                if (!g->chunk(d.L, lane, n, out)) {
                    res.gpu_note = g->why;
                    return -2;
                }
            } else {
                cpu_chunk(d.L, lane, n, jobs, out);
            }
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
            for (int s = 0; s < static_cast<int>(VM_S_COUNT); ++s) res.counts[s] += out[1 + s];
            if (out[0] != ~uint64_t{0}) {
                res.first_violation = out[0];
                res.args = args_of(d.L, out[0]);
                run_one(p, res.args, o.step_limit, &res.prop);
                res.lanes = out[0] + 1;  // every smaller input ran clean
                res.exhaustive = false;
                return 1;
            }
            lane += n;
            res.lanes = lane;
            const double left = o.budget_s - elapsed();
            if (may_abandon && dt > 0 && double(total - lane) / double(n) * dt > left) return -1;
            if (left <= 0) return 0;
            if (dt < target && chunk < (uint64_t{1} << 30)) chunk *= 2;
        }
        res.exhaustive = exhaustive;
        return 0;
    };
    // Every input when the parameters are few bits and the projection fits
    // the budget; else sampled. -2: the GPU failed.
    auto policy = [&](unsigned bits_cap, uint64_t n_samples) {
        int st = -1;
        if (p.input_bits <= bits_cap) st = phase(true, uint64_t{1} << p.input_bits, true);
        if (st == -1) st = phase(false, n_samples, false);
        return st;
    };
    if (policy(ex_bits, samples) == -2) {
        // driver failure mid-sweep: the CPU twin runs instead (Law 7: said in gpu_note)
        g.reset();
        if (lk.owns_lock()) lk.unlock();
        gpu = false;
        res.engine = "cpu (" + std::to_string(jobs) + " threads)";
        res.first_violation.reset();
        policy(20u, uint64_t{1} << 16);
    }
    res.ran = true;
    res.seconds = elapsed();
    return res;
}

bool confirm(const Function& fn, const std::vector<std::string>& only_props, const Program& p, const SweepResult& r) {
    if (!r.first_violation || r.prop >= p.props.size()) return false;
    Function f = fn;
    if (!only_props.empty())
        for (auto& b : f.blocks)
            std::erase_if(b.stmts, [&](const Stmt& s) {
                return s.kind == Stmt::Check &&
                       std::find(only_props.begin(), only_props.end(), s.prop) == only_props.end();
            });
    auto ir = interpret(f, r.args);
    const auto& want = p.props[r.prop];
    return ir.status == InterpResult::Violation && ir.prop == want.prop && ir.line == want.line && ir.cls == want.cls;
}

}  // namespace prism::pir::vm
