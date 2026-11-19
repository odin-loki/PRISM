// Address-space pointers (--pir-addrspace with --kernel-api, docs/PIR.md "Address spaces"). Included by translate.cpp
// after translate_ksprintf.inc: Tr member functions.
//
// x86 kernels reach per-CPU data through segment-relative pointers (`ptr addrspace(256)`, %gs: FreeBSD's
// `struct pcpu __seg_gs *__pc = NULL; __pc->pc_cpuid`, Linux's this_cpu_* with __seg_gs; `addrspace(257)`, %fs, on
// i386). The memory model has one address space, so a segment pointer is mapped onto it where it is used as an
// address (load, store, getelementptr base):
//   * a pointer to no object (null plus an offset, an integer constant): the current CPU's per-CPU area at that
//     offset. The area is one object of kPcpuBytes arbitrary initialised bytes, allocated on first use; an access
//     beyond it is reported (never proved);
//   * a pointer to an object (the address of a per-CPU variable): that object (the current CPU's copy is the object
//     the program names).
// A load of a segment pointer from an internal global initialised to null that no instruction of the unit writes
// (FreeBSD's `static __pc`) is null. Named (`addrspace: ...`): no other CPU, interrupt or migration changes the
// per-CPU data during the function, and the segment base is the current CPU's area.

namespace {
constexpr uint64_t kPcpuBytes = uint64_t{1} << 20;
}  // namespace

bool Tr::seg_ptr(const ir::Type& t) const {
    return opt.addrspace && !opt.kernel_api.empty() && t.kind == ir::Type::Ptr && t.text == "ptr addrspace";
}

Arg Tr::seg_operand(Frame& fr, const ir::Operand& o, int cur, int line) {
    // `inttoptr (iN K to ptr addrspace(N))`: the segment offset K (no --pir-addr-values needed: it names no object)
    if (o.v.kind == ir::Value::ConstExpr && o.v.ce_op == "inttoptr" && !o.v.elems.empty() &&
        o.v.elems[0].v.kind == ir::Value::Int)
        return Arg::c(ptr_w(), o.v.elems[0].v.bits);
    return operand(fr, o, cur, line);
}

Arg Tr::seg_addr(int b, Arg p, int line) {
    const unsigned pw = ptr_w();
    p.width = pw;
    if (!pcpu_) {
        pcpu_ = mt.alloc(-1, Arg::c(64, kPcpuBytes), MemKind::Static, 2, "per-CPU area (segment base)", line);
        note_assumption("addrspace: an access through a segment pointer (ptr addrspace(N): x86 %gs / %fs per-CPU data) is "
                        "an access of the current CPU's per-CPU area (one object of " + std::to_string(kPcpuBytes) +
                        " arbitrary initialised bytes) at the pointer's offset when it points to no object, else of the "
                        "object it points to; no other CPU, interrupt or migration changes the per-CPU data during the "
                        "function");
    }
    Arg no_obj = assign(b, Op::Eq, 1, {assign(b, Op::LShr, pw, {p, Arg::c(pw, obj_shift())}), Arg::c(pw, 0)}, "c");
    // the area is at offset 0 of its object: area | offset is the packed pointer to that offset
    Arg q = assign(b, Op::Or, pw, {*pcpu_, assign(b, Op::And, pw, {p, Arg::c(pw, off_mask())})}, "seg");
    return assign(b, Op::Select, pw, {no_obj, q, p}, "seg");
}

bool Tr::seg_global_null(const ir::Operand& o) {
    if (o.v.kind != ir::Value::Global) return false;
    const auto* g = m.find_global(o.v.name);
    if (!g || g->external || g->text.find(" internal ") == std::string::npos) return false;
    if (g->init.size() != 1 || (g->init[0].v.kind != ir::Value::Null && g->init[0].v.kind != ir::Value::Zero)) return false;
    if (auto it = seg_null_memo_.find(o.v.name); it != seg_null_memo_.end()) return it->second;
    // every use of @g in the unit is the address of a load (never stored to, never escapes)
    bool ok = true;
    auto names = [&](const ir::Value& v, auto&& self) -> bool {
        if (v.kind == ir::Value::Global && v.name == o.v.name) return true;
        for (auto& e : v.elems)
            if (self(e.v, self)) return true;
        return false;
    };
    for (auto& f : m.functions) {
        for (auto& bl : f.blocks)
            for (auto& x : bl.insts) {
                for (std::size_t k = 0; k < x.ops.size() && ok; ++k)
                    if (names(x.ops[k].v, names) && !(x.op == "load" && k == 0 && x.ops[k].v.kind == ir::Value::Global))
                        ok = false;
                for (auto& [iv, _] : x.incoming)
                    if (names(iv.v, names)) ok = false;
                if (!x.parsed && x.text.find("@" + o.v.name) != std::string::npos) ok = false;  // unread: may write it
            }
        if (!ok) break;
    }
    for (auto& gl : m.globals)
        for (auto& iv : gl.init)
            if (names(iv.v, names)) ok = false;  // its address is stored in another global
    seg_null_memo_[o.v.name] = ok;
    return ok;
}
