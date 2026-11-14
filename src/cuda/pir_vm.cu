// PIR bytecode VM on the GPU (docs/GPU_PLAN.md G1): one thread per input lane.
//
// Built to PTX by clang alone (no CUDA SDK, no nvcc):
//   clang -x cuda --cuda-device-only -nocudainc -nocudalib --cuda-gpu-arch=sm_86 -O3 -S
// and embedded in PRISM (CMakeLists.txt, prism/pir_vm_ptx.inc). The driver
// JIT-compiles the PTX for the card it runs on (sm_86 and newer). PRISM loads
// it through the CUDA driver API via FORGE's dl (src/prism/pir/vm.cpp).
//
// The VM itself is src/prism/pir/vm_core.inc, the same code the CPU twin runs.

#define PRISM_VM_FN __attribute__((device)) static inline
#include "../prism/pir/vm_core.inc"

#define PRISM_KERNEL extern "C" __attribute__((global))

__attribute__((device)) static inline vm_u64 lane_index() {
    return (vm_u64)__nvvm_read_ptx_sreg_ctaid_x() * __nvvm_read_ptx_sreg_ntid_x() + __nvvm_read_ptx_sreg_tid_x();
}
__attribute__((device)) static inline vm_u64 lane_stride() {
    return (vm_u64)__nvvm_read_ptx_sreg_nctaid_x() * __nvvm_read_ptx_sreg_ntid_x();
}

template <int R>
__attribute__((device)) static inline void run_lanes(const VmLaunch& L) {
    vm_u64 r[R];
    vm_u64 cnt[VM_S_COUNT] = {0, 0, 0, 0, 0, 0};
    vm_u64 best = ~0ull;
    for (vm_u64 i = lane_index(); i < L.nlanes; i += lane_stride()) {
        const vm_u64 lane = L.lane0 + i;
        vm_u32 prop = 0;
        const vm_u32 st = vm_lane(L, lane, r, &prop);
        ++cnt[st];
        if (st == VM_S_VIOLATION) {
            best = lane;  // lanes rise along the stride: the first is this thread's smallest
            break;
        }
    }
    if (best != ~0ull) __nvvm_atom_min_gen_ull((unsigned long long*)&L.out[0], (unsigned long long)best);
    for (int s = 0; s < (int)VM_S_COUNT; ++s)
        if (cnt[s]) __nvvm_atom_add_gen_ll((long long*)&L.out[1 + s], (long long)cnt[s]);
}

PRISM_KERNEL void prism_pir_vm_64(VmLaunch L) { run_lanes<64>(L); }
PRISM_KERNEL void prism_pir_vm_256(VmLaunch L) { run_lanes<256>(L); }
PRISM_KERNEL void prism_pir_vm_1024(VmLaunch L) { run_lanes<1024>(L); }
