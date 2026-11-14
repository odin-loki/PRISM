// PIR bytecode VM: the one source of the CPU twin (src/prism/pir/vm.cpp) and
// the GPU kernel (src/cuda/pir_vm.cu, clang -> PTX). docs/GPU_PLAN.md G1.
//
// Every operator mirrors pir::eval_op (src/prism/pir/pir.cpp) bit for bit,
// including Z3's total division; values in registers are always masked to
// their width, as in pir::interpret. No standard library, no CUDA headers:
// the device build is `clang -x cuda --cuda-device-only -nocudainc`.
//
// Include with PRISM_VM_FN defined (host: `inline`, device:
// `__attribute__((device)) inline`).

#ifndef PRISM_VM_FN
#error "define PRISM_VM_FN before including vm_core.inc"
#endif

#if defined(__UINT64_TYPE__) && defined(__INT64_TYPE__)
typedef __UINT64_TYPE__ vm_u64;  // the same type as uint64_t, so host vectors pass straight in
typedef __INT64_TYPE__ vm_i64;
#else
typedef unsigned long long vm_u64;
typedef long long vm_i64;
#endif
typedef unsigned int vm_u32;
typedef unsigned short vm_u16;
typedef unsigned char vm_u8;

// Opcodes. The first block is pir::Op in order (vm.cpp static_asserts the
// mapping for each one it emits); control and property opcodes follow.
enum VmOp : vm_u8 {
    VM_COPY = 0, VM_HAVOC,
    VM_ADD, VM_SUB, VM_MUL, VM_UDIV, VM_SDIV, VM_UREM, VM_SREM, VM_SHL, VM_LSHR, VM_ASHR, VM_AND, VM_OR, VM_XOR,
    VM_EQ, VM_NE, VM_ULT, VM_ULE, VM_UGT, VM_UGE, VM_SLT, VM_SLE, VM_SGT, VM_SGE,
    VM_SELECT, VM_ZEXT, VM_SEXT, VM_TRUNC,
    VM_SMAX, VM_SMIN, VM_UMAX, VM_UMIN, VM_ABS, VM_CTLZ, VM_CTTZ, VM_CTPOP, VM_BSWAP,
    VM_SADDOVF, VM_SSUBOVF, VM_SMULOVF, VM_UADDOVF, VM_USUBOVF, VM_UMULOVF,
    VM_SDIVOVF, VM_SHIFTOOB, VM_SHLSOVF, VM_SHLNSWOVF, VM_SHLNUWOVF, VM_LOSTBITSL, VM_LOSTBITSA,
    VM_INEXACTU, VM_INEXACTS,
    VM_LAST_ALU = VM_INEXACTS,
    VM_CHECK = 100,   // r[a] & 1: property imm violated
    VM_SOFT,          // r[a] & 1: an unmodelled construct is reached (lane undecided)
    VM_ASSUME,        // !(r[a] & 1): the input is outside the precondition
    VM_JMP,           // pc = imm
    VM_BR,            // r[a] & 1: pc = imm, else fall through
    VM_RET,
    VM_STOP,
};

// 16 bytes, the same layout on host and device.
struct VmInsn {
    vm_u8 op, w, xw, pad;
    vm_u16 dst, a, b, c;
    vm_u32 imm;
};

// Lane outcome (pir::InterpResult::Status plus Unmodelled).
enum VmStatus : vm_u32 {
    VM_S_RETURNED = 0, VM_S_STOPPED = 1, VM_S_VIOLATION = 2, VM_S_ASSUME = 3, VM_S_STEPS = 4, VM_S_SOFT = 5,
    VM_S_COUNT = 6,
};

// Kernel / CPU-engine launch description (plain data, passed by value).
struct VmLaunch {
    const VmInsn* code;
    const vm_u64* init;      // nregs initial values (constants; 0 elsewhere)
    const vm_u16* preg;      // parameter registers
    const vm_u8* pw;         // parameter widths
    const vm_u8* poff;       // exhaustive: bit offset of each parameter in the lane number
    vm_u32 ncode, nregs, nparams, step_limit;
    vm_u32 exhaustive;       // 1: lane number is the input; 0: generated from (seed, lane)
    vm_u32 pad;
    vm_u64 seed;
    vm_u64 edge_lanes;       // sampled: lanes [0, edge_lanes) enumerate the edge-value product
    vm_u64 lane0, nlanes;
    vm_u64* out;             // [0] smallest violating lane (init ~0), [1 + s] count of status s
};

PRISM_VM_FN vm_u64 vm_mask(vm_u32 w) { return w >= 64 ? ~0ull : ((1ull << w) - 1); }
PRISM_VM_FN vm_i64 vm_sval(vm_u64 v, vm_u32 w) {
    if (w >= 64) return (vm_i64)v;
    const vm_u64 sign = 1ull << (w - 1);
    v &= vm_mask(w);
    return (vm_i64)((v ^ sign) - sign);
}
PRISM_VM_FN vm_u64 vm_smin(vm_u32 w) { return (1ull << (w - 1)) & vm_mask(w); }

PRISM_VM_FN vm_u64 vm_mulhi(vm_u64 a, vm_u64 b) {
#if defined(__CUDA_ARCH__)
    return __nvvm_mulhi_ull(a, b);
#elif defined(__SIZEOF_INT128__)
    return (vm_u64)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
#else
    const vm_u64 al = a & 0xffffffffull, ah = a >> 32, bl = b & 0xffffffffull, bh = b >> 32;
    const vm_u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const vm_u64 mid = (ll >> 32) + (lh & 0xffffffffull) + (hl & 0xffffffffull);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}
PRISM_VM_FN vm_u32 vm_clz64(vm_u64 x) { return x ? (vm_u32)__builtin_clzll(x) : 64u; }
PRISM_VM_FN vm_u32 vm_ctz64(vm_u64 x) { return x ? (vm_u32)__builtin_ctzll(x) : 64u; }
PRISM_VM_FN vm_u32 vm_popc64(vm_u64 x) { return (vm_u32)__builtin_popcountll(x); }

// pir::eval_op at result width w, first-argument width xw.
PRISM_VM_FN vm_u64 vm_eval(vm_u32 op, vm_u32 w, vm_u32 xw, vm_u64 x, vm_u64 y, vm_u64 z) {
    const vm_u64 m = vm_mask(w);
    switch (op) {
        case VM_COPY: return x & m;
        case VM_HAVOC: return 0;
        case VM_ADD: return (x + y) & m;
        case VM_SUB: return (x - y) & m;
        case VM_MUL: return (x * y) & m;
        case VM_UDIV: return y == 0 ? m : (x / y) & m;
        case VM_UREM: return y == 0 ? x : (x % y) & m;
        case VM_SDIV: {
            if (y == 0) return vm_sval(x, w) < 0 ? 1 : m;
            if (x == vm_smin(w) && y == m) return x;
            return (vm_u64)(vm_sval(x, w) / vm_sval(y, w)) & m;
        }
        case VM_SREM: {
            if (y == 0) return x;
            if (x == vm_smin(w) && y == m) return 0;
            return (vm_u64)(vm_sval(x, w) % vm_sval(y, w)) & m;
        }
        case VM_SHL: return y >= w ? 0 : (x << y) & m;
        case VM_LSHR: return y >= w ? 0 : (x >> y) & m;
        case VM_ASHR: {
            const vm_i64 sx = vm_sval(x, w);
            if (y >= w) return sx < 0 ? m : 0;
            return (vm_u64)(sx >> y) & m;
        }
        case VM_AND: return x & y;
        case VM_OR: return x | y;
        case VM_XOR: return (x ^ y) & m;
        case VM_EQ: return x == y;
        case VM_NE: return x != y;
        case VM_ULT: return x < y;
        case VM_ULE: return x <= y;
        case VM_UGT: return x > y;
        case VM_UGE: return x >= y;
        case VM_SLT: return vm_sval(x, xw) < vm_sval(y, xw);
        case VM_SLE: return vm_sval(x, xw) <= vm_sval(y, xw);
        case VM_SGT: return vm_sval(x, xw) > vm_sval(y, xw);
        case VM_SGE: return vm_sval(x, xw) >= vm_sval(y, xw);
        case VM_SELECT: return (x & 1u) ? (y & m) : (z & m);
        case VM_ZEXT: return (x & vm_mask(xw)) & m;
        case VM_SEXT: return (vm_u64)vm_sval(x, xw) & m;
        case VM_TRUNC: return x & m;
        case VM_SMAX: return vm_sval(x, w) >= vm_sval(y, w) ? x : y;
        case VM_SMIN: return vm_sval(x, w) <= vm_sval(y, w) ? x : y;
        case VM_UMAX: return x >= y ? x : y;
        case VM_UMIN: return x <= y ? x : y;
        case VM_ABS: return vm_sval(x, w) < 0 ? (~x + 1) & m : x;
        case VM_CTLZ: {
            const vm_u64 v = x & m;
            return (v ? (vm_u64)(vm_clz64(v) - (64u - w)) : (vm_u64)w) & m;
        }
        case VM_CTTZ: {
            const vm_u64 v = x & m;
            return (v ? (vm_u64)vm_ctz64(v) : (vm_u64)w) & m;
        }
        case VM_CTPOP: return (vm_u64)vm_popc64(x & m) & m;
        case VM_BSWAP: {
            vm_u64 r = 0;
            for (vm_u32 i = 0; i < w / 8; ++i) r |= ((x >> (i * 8)) & 0xff) << ((w / 8 - 1 - i) * 8);
            return r & m;
        }
        case VM_SADDOVF: {
            const vm_u64 r = (x + y) & vm_mask(xw);
            if (xw >= 64) return (((x ^ r) & (y ^ r)) >> 63) & 1u;
            return vm_sval(x, xw) + vm_sval(y, xw) != vm_sval(r, xw);
        }
        case VM_SSUBOVF: {
            const vm_u64 r = (x - y) & vm_mask(xw);
            if (xw >= 64) return (((x ^ y) & (x ^ r)) >> 63) & 1u;
            return vm_sval(x, xw) - vm_sval(y, xw) != vm_sval(r, xw);
        }
        case VM_SMULOVF: {
            const vm_i64 sx = vm_sval(x, xw), sy = vm_sval(y, xw);
            const vm_u64 lo = (vm_u64)sx * (vm_u64)sy;
            const vm_u64 hi = vm_mulhi((vm_u64)sx, (vm_u64)sy) - (sx < 0 ? (vm_u64)sy : 0) - (sy < 0 ? (vm_u64)sx : 0);
            const vm_i64 r = vm_sval(lo & vm_mask(xw), xw);
            return !(hi == (r < 0 ? ~0ull : 0ull) && lo == (vm_u64)r);
        }
        case VM_UADDOVF: return ((x + y) & vm_mask(xw)) < x || (xw == 64 && x + y < x);
        case VM_USUBOVF: return y > x;
        case VM_UMULOVF: return vm_mulhi(x, y) != 0 || (x * y) > vm_mask(xw);
        case VM_SDIVOVF: return x == vm_smin(xw) && y == vm_mask(xw);
        case VM_SHIFTOOB: return y >= xw;
        case VM_SHLSOVF: {
            if (y >= xw) return 1;
            if (vm_sval(x, xw) < 0) return 1;
            return (x >> (xw - 1 - y)) != 0;
        }
        case VM_SHLNSWOVF: {
            if (y >= xw) return 1;
            const vm_u64 r = (x << y) & vm_mask(xw);
            return ((vm_u64)(vm_sval(r, xw) >> y) & vm_mask(xw)) != x;
        }
        case VM_SHLNUWOVF: {
            if (y >= xw) return 1;
            return (((x << y) & vm_mask(xw)) >> y) != x;
        }
        case VM_LOSTBITSL: {
            if (y >= xw) return 1;
            return (((x >> y) << y) & vm_mask(xw)) != x;
        }
        case VM_LOSTBITSA: {
            if (y >= xw) return 1;
            const vm_u64 r = (vm_u64)(vm_sval(x, xw) >> y) & vm_mask(xw);
            return ((r << y) & vm_mask(xw)) != x;
        }
        case VM_INEXACTU: return y != 0 && (x % y) != 0;
        case VM_INEXACTS: {
            if (y == 0) return 0;
            if (x == vm_smin(xw) && y == vm_mask(xw)) return 0;
            return (vm_sval(x, xw) % vm_sval(y, xw)) != 0;
        }
        default: return 0;
    }
}

// splitmix64: the input generator (host and device agree).
PRISM_VM_FN vm_u64 vm_mix(vm_u64 z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

#define VM_EDGES 8u
// Edge value e (0 <= e < VM_EDGES) at width w.
PRISM_VM_FN vm_u64 vm_edge(vm_u32 e, vm_u32 w) {
    const vm_u64 m = vm_mask(w);
    switch (e) {
        case 0: return 0;
        case 1: return 1 & m;
        case 2: return 2 & m;
        case 3: return m;                     // -1
        case 4: return (m - 1) & m;           // -2
        case 5: return vm_smin(w);            // INT_MIN
        case 6: return (vm_smin(w) - 1) & m;  // INT_MAX
        default: return (vm_smin(w) + 1) & m; // INT_MIN + 1
    }
}

// Parameter k's value on `lane`.
PRISM_VM_FN vm_u64 vm_lane_arg(const VmLaunch& L, vm_u32 k, vm_u64 lane) {
    const vm_u32 w = L.pw[k];
    const vm_u64 m = vm_mask(w);
    if (L.exhaustive) return (lane >> L.poff[k]) & m;
    if (lane < L.edge_lanes) {
        vm_u64 q = lane;
        for (vm_u32 i = 0; i < k; ++i) q /= VM_EDGES;
        return vm_edge((vm_u32)(q % VM_EDGES), w);
    }
    const vm_u64 h = vm_mix(L.seed ^ vm_mix(lane * 0x9e3779b97f4a7c15ull + k));
    const vm_u32 sel = (vm_u32)(h & 7u);
    if (sel < 2) return vm_edge((vm_u32)((h >> 3) % VM_EDGES), w);
    if (sel == 2) return (vm_u64)((vm_i64)((h >> 3) % 33u) - 16) & m;            // small, both signs
    if (sel == 3) return ((1ull << ((h >> 3) % (w ? w : 1))) + ((h >> 10) % 3u) - 1u) & m;  // 2^k - 1, 2^k, 2^k + 1
    return vm_mix(h) & m;
}

// Runs one lane from pc 0 over registers r (initialised by the caller).
// Returns the status; *prop is the violated property index.
PRISM_VM_FN vm_u32 vm_run(const VmInsn* code, vm_u32 ncode, vm_u64* r, vm_u32 step_limit, vm_u32* prop) {
    vm_u32 pc = 0;
    for (vm_u32 steps = 0;; ++steps) {
        if (steps >= step_limit || pc >= ncode) return pc >= ncode ? VM_S_STOPPED : VM_S_STEPS;
        const VmInsn I = code[pc];
        if (I.op <= VM_LAST_ALU) {
            r[I.dst] = vm_eval(I.op, I.w, I.xw, r[I.a], r[I.b], r[I.c]) & vm_mask(I.w);
            ++pc;
            continue;
        }
        switch (I.op) {
            case VM_CHECK:
                if (r[I.a] & 1u) { *prop = I.imm; return VM_S_VIOLATION; }
                ++pc; break;
            case VM_SOFT:
                if (r[I.a] & 1u) return VM_S_SOFT;
                ++pc; break;
            case VM_ASSUME:
                if (!(r[I.a] & 1u)) return VM_S_ASSUME;
                ++pc; break;
            case VM_JMP: pc = I.imm; break;
            case VM_BR: pc = (r[I.a] & 1u) ? I.imm : pc + 1; break;
            case VM_RET: return VM_S_RETURNED;
            default: return VM_S_STOPPED;
        }
    }
}

// Initialises registers for `lane` and runs it.
PRISM_VM_FN vm_u32 vm_lane(const VmLaunch& L, vm_u64 lane, vm_u64* r, vm_u32* prop) {
    for (vm_u32 i = 0; i < L.nregs; ++i) r[i] = L.init[i];
    for (vm_u32 k = 0; k < L.nparams; ++k) r[L.preg[k]] = vm_lane_arg(L, k, lane);
    return vm_run(L.code, L.ncode, r, L.step_limit, prop);
}
