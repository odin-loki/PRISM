// Variadic functions (--pir-variadic, docs/PIR.md "Variadic functions"). Included by translate.cpp after
// translate_kernel.inc: these are Tr member functions.
//
// A variadic function's IR is translated like any other; the variadic arguments are an over-approximation:
// llvm.va_start makes the va_list point to fresh objects of arbitrary initialised bytes (the register save area and
// the stack argument area of the target's ABI), with the ABI's offsets arbitrary within their ABI range. Every
// va_arg the function performs (clang lowers it to loads and pointer arithmetic on the va_list) then reads an
// arbitrary value. The named assumption is the caller's side of C11 7.16.1.1p2: the callers pass at least the
// variadic arguments the function reads, of the types it reads them as (reading more, or another type, is undefined
// behaviour of the call, which this function's verdict does not cover). A call of a variadic function defined in
// the unit evaluates every argument (undef and poison uses are checked) and inlines the callee with its fixed ones.

namespace {
// bytes of the arbitrary stack-argument area (va_arg past it is reported as out of bounds, never proved)
constexpr uint64_t kVaStackBytes = uint64_t{1} << 32;
bool struct_is(const ir::Module& m, const char* name, std::initializer_list<std::pair<ir::Type::Kind, unsigned>> f) {
    auto it = m.types.find(name);
    if (it == m.types.end() || it->second.kind != ir::Type::Struct || it->second.elems.size() != f.size()) return false;
    std::size_t i = 0;
    for (auto [k, bits] : f) {
        const auto& e = it->second.elems[i++];
        if (e.kind != k || (k == ir::Type::Int && e.bits != bits)) return false;
    }
    return true;
}
}  // namespace

// 1: x86-64 SysV {i32 gp_offset, i32 fp_offset, ptr overflow_arg_area, ptr reg_save_area};
// 2: AArch64 AAPCS64 {ptr __stack, ptr __gr_top, ptr __vr_top, i32 __gr_offs, i32 __vr_offs};
// 3: one pointer to the next argument (i386, RISC-V, PowerPC64, Arm: va_list is char * or struct { void * });
// 0: not known (UNENCODED).
int Tr::va_layout() const {
    using K = ir::Type::Kind;
    if (struct_is(m, "struct.__va_list_tag", {{K::Int, 32}, {K::Int, 32}, {K::Ptr, 0}, {K::Ptr, 0}})) return 1;
    if (struct_is(m, "struct.__va_list", {{K::Ptr, 0}, {K::Ptr, 0}, {K::Ptr, 0}, {K::Int, 32}, {K::Int, 32}})) return 2;
    if (m.types.count("struct.__va_list_tag") || m.types.count("struct.__va_list")) {
        if (struct_is(m, "struct.__va_list", {{K::Ptr, 0}})) return 3;  // Arm (AAPCS): struct { void *__ap; }
        return 0;
    }
    // x86-64 / AArch64 (p270 in the datalayout) always declare their struct when they use va_start; i386 (p:32) is a
    // pointer. Any other target is taken as a pointer: a va_list struct of another layout would leave its other fields
    // uninitialised (reported when read), never proved.
    if (m.datalayout.find("p270") != std::string::npos && m.datalayout.find("p:32:32") == std::string::npos) return 0;
    return 3;
}

void Tr::va_intrinsic(const std::string& n, Arg ap, Arg src, int& cur, int line) {
    if (starts(n, "llvm.va_end")) return;  // no effect on the model
    const int lay = va_layout();
    if (lay == 0) throw Unenc{"UNENCODED: call @" + n + " (va_list layout of this target not modelled)"};
    const ir::Type ti32{ir::Type::Int, 32, "i32", {}}, tptr{ir::Type::Ptr, 0, "ptr", {}};
    const unsigned pb = ptr_w() / 8;
    ap.width = ptr_w();
    if (starts(n, "llvm.va_copy")) {
        src.width = ptr_w();
        const uint64_t bytes = lay == 1 ? 24 : lay == 2 ? 32 : pb;
        mt.memcpy_(cur, ap, src, Arg::c(ptr_w(), bytes), false, line, true);
        return;
    }
    // llvm.va_start
    const uint64_t big = std::min<uint64_t>(kVaStackBytes, max_obj_size() / 2);
    Arg stack = mt.alloc(cur, Arg::c(64, big), MemKind::Extern, 2, "variadic stack arguments", line);
    auto put_ptr = [&](int64_t off, Arg v) { mt.store(cur, at(cur, ap, off, line), v, Arg::c(1, 1), tptr, pb, line); };
    auto put_i32 = [&](int64_t off, Arg v) { mt.store(cur, at(cur, ap, off, line), v, Arg::c(1, 1), ti32, 4, line); };
    auto in_range = [&](Arg v, bool sgn, uint64_t lo, uint64_t hi, uint64_t mask) {
        Op ge = sgn ? Op::Sge : Op::Uge, le = sgn ? Op::Sle : Op::Ule;
        assume(cur, assign(cur, ge, 1, {v, Arg::c(32, lo)}, "c"));
        assume(cur, assign(cur, le, 1, {v, Arg::c(32, hi)}, "c"));
        assume(cur, assign(cur, Op::Eq, 1, {assign(cur, Op::And, 32, {v, Arg::c(32, mask)}), Arg::c(32, 0)}, "c"));
    };
    std::string abi;
    if (lay == 1) {
        // gp_offset: 8 * (named integer arguments in registers) <= 48; fp_offset: 48 + 16 * (named SSE ones) <= 176
        Arg gp = havoc(cur, 32, false, false), fp = havoc(cur, 32, false, false);
        in_range(gp, false, 0, 48, 7);
        in_range(fp, false, 48, 176, 15);
        Arg regs = mt.alloc(cur, Arg::c(64, 176), MemKind::Extern, 2, "variadic register save area", line);
        put_i32(0, gp);
        put_i32(4, fp);
        put_ptr(8, stack);
        put_ptr(16, regs);
        abi = "x86-64 SysV: an arbitrary 176-byte register save area, gp_offset and fp_offset arbitrary in their ABI ranges";
    } else if (lay == 2) {
        // __gr_offs in [-64, 0] (multiple of 8) below __gr_top; __vr_offs in [-128, 0] (multiple of 16) below __vr_top
        Arg gr = mt.alloc(cur, Arg::c(64, 64), MemKind::Extern, 2, "variadic general register save area", line);
        Arg vr = mt.alloc(cur, Arg::c(64, 128), MemKind::Extern, 2, "variadic vector register save area", line);
        Arg go = havoc(cur, 32, false, false), vo = havoc(cur, 32, false, false);
        in_range(go, true, static_cast<uint32_t>(-64), 0, 7);
        in_range(vo, true, static_cast<uint32_t>(-128), 0, 15);
        put_ptr(0, stack);
        put_ptr(8, at(cur, gr, 64, line));
        put_ptr(16, at(cur, vr, 128, line));
        put_i32(24, go);
        put_i32(28, vo);
        abi = "AArch64 AAPCS64: arbitrary general (64-byte) and vector (128-byte) register save areas, __gr_offs and "
              "__vr_offs arbitrary in their ABI ranges";
    } else {
        put_ptr(0, stack);
        abi = "va_list is a pointer to the next argument";
    }
    note_assumption("variadic: the variadic arguments of @" + (stack.empty() ? std::string("?") : stack.back()) + " are arbitrary values (va_start at line " +
                    std::to_string(line) + ": " + abi + ", and an arbitrary stack argument area of " + std::to_string(big) +
                    " bytes); its callers pass at least the variadic arguments it reads, of the types it reads them as "
                    "(C11 7.16.1.1p2)");
}
