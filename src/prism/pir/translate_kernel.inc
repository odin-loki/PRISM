// Kernel mode of the Clang -> PIR translator (docs/KERNEL_MODELS.md; roadmap
// requirements R2 and R3 of docs/feedback/DRIVER-REQUIREMENTS.md). Included by
// translate.cpp after translate_ctl.inc: these are Tr member functions.
//
// Everything here is an *assumption*, not a proof step: each use is recorded
// in Function::assumptions by note_assumption (a PROVED verdict becomes
// PROVED-ASSUMING and lists it), and none of it is active without
// `--kernel-api` (inline asm, atomics, barriers) or `--assume-unknown-calls`
// (calls without a spec).
//
// Inline assembly (asm_havoc). The asm statement is one execution that
//   * writes every direct output: the result of the call is an arbitrary value
//     of its type (a pointer result: null or a pointer to a fresh object of
//     kUnknownPtrBytes arbitrary initialised bytes, see opaque_ptr),
//   * writes through every indirect output operand (`=*m`, `+*m`), and, for a
//     `~{memory}` clobber, through every pointer operand: the bytes of the
//     object the pointer points into, from the pointer to the end of the
//     object, become arbitrary (havoc_object_tail),
//   * changes nothing else the memory model can see: registers and flags are
//     not modelled, and memory that no pointer operand points into is assumed
//     unchanged. (A `memory` clobber really means all memory; PRISM's memory
//     model has no "havoc everything" statement, so this is the part of the
//     claim that is assumed, and the assumption text says so.)
// An asm that is `volatile` is never removed or merged: it is translated where
// it stands, once per execution. asm goto is a call that may continue at any of
// its labels (callbr, translate.cpp).
//
// Calls without a spec (unknown_call, --assume-unknown-calls): the result is
// havocked, the objects the pointer arguments point into are havocked from the
// pointer to the end of the object (not Const, not freed objects), the callee
// is assumed to return, not to free anything, not to retain or change any
// other memory, not to call back into the unit.

namespace {
// bytes of the object an unknown pointer result points to (null is also possible)
constexpr uint64_t kUnknownPtrBytes = uint64_t{1} << 20;
}  // namespace

void Tr::note_assumption(const std::string& text) {
    for (auto& a : out.assumptions)
        if (a == text) return;
    out.assumptions.push_back(text);
}

namespace {
std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (c == ',' && depth == 0) {
            r.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() || !r.empty()) r.push_back(cur);
    return r;
}
}  // namespace

void Tr::asm_havoc(Frame& fr, const ir::Inst& in, int& cur, int line) {
    // operands: evaluated once (undef / poison uses are checked)
    std::vector<Arg> args;
    for (auto& o : in.ops) args.push_back(operand(fr, o, cur, line));
    bool mem_clobber = false;
    std::size_t n_direct = 0, argi = 0;
    std::set<std::size_t> written;  // operand indices written through (indirect outputs)
    for (auto c : split_commas(in.asm_constraints)) {
        if (c.starts_with("~{")) {
            if (c.find("memory") != std::string::npos) mem_clobber = true;
            continue;
        }
        const bool is_out = !c.empty() && (c[0] == '=' || c[0] == '+');
        const bool indirect = c.find('*') != std::string::npos;
        if (is_out && !indirect) {
            ++n_direct;
            continue;
        }
        if (is_out) written.insert(argi);
        ++argi;
    }
    std::size_t havocked_ptrs = 0;
    for (std::size_t i = 0; i < args.size() && i < in.ops.size(); ++i)
        if (in.ops[i].ty.kind == ir::Type::Ptr && (mem_clobber || written.count(i))) {
            mt.havoc_object_tail(cur, args[i], line);
            ++havocked_ptrs;
        }
    // direct outputs: the call's result
    if (!in.result.empty()) {
        if (auto pit = fr.pairs.find(in.result); pit != fr.pairs.end()) {
            for (int v : {pit->second.first, pit->second.second})
                havoc(cur, out.vars[static_cast<std::size_t>(v)].width, false, false, v);
        } else if (in.ty.kind == ir::Type::Ptr) {
            int dst = result_var(fr, in);
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {opaque_ptr(cur, "asm output", line)});
        } else if (in.ty.kind == ir::Type::Int) {
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
        } else if (in.ty.kind != ir::Type::Void) {
            throw Unenc{"UNENCODED: inline assembly with outputs of type " + in.ty.text};
        }
    }
    std::string what = "asm-havoc: \"" + in.asm_constraints + "\" (line " + std::to_string(line) + "): ";
    what += n_direct ? std::to_string(n_direct) + " output(s) havocked" : "no register output";
    if (!written.empty()) what += "; " + std::to_string(written.size()) + " indirect output(s) written through the pointer operand(s)";
    if (mem_clobber)
        what += "; memory clobber: the objects its " + std::to_string(havocked_ptrs) +
                " pointer operand(s) point into are havocked, all other memory is assumed unchanged";
    else
        what += "; no memory effect besides the outputs";
    if (in.asm_sideeffect) what += "; volatile: translated once where it stands";
    note_assumption(what);
}

// A pointer result of an unspecified function or asm: null or a pointer to a
// fresh object of kUnknownPtrBytes arbitrary initialised bytes. (Not an
// ERR_PTR value, and an access beyond the first kUnknownPtrBytes bytes is
// reported as out of bounds: both are part of the assumption.)
Arg Tr::opaque_ptr(int& cur, const std::string& what, int line) {
    Arg is_null = havoc(cur, 1, false, false);
    Arg p = mt.alloc(cur, Arg::c(ptr_w(), kUnknownPtrBytes), MemKind::Extern, 2, what, line);
    return assign(cur, Op::Select, ptr_w(), {is_null, Arg::c(ptr_w(), 0), p}, "uptr");
}

void Tr::unknown_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
    const auto& n = in.callee;
    std::vector<Arg> args;
    for (auto& o : in.ops) args.push_back(operand(fr, o, cur, line));
    std::size_t nptr = 0;
    for (std::size_t i = 0; i < args.size() && i < in.ops.size(); ++i)
        if (in.ops[i].ty.kind == ir::Type::Ptr) {
            mt.havoc_object_tail(cur, args[i], line);
            ++nptr;
        }
    if (!in.result.empty()) {
        if (auto pit = fr.pairs.find(in.result); pit != fr.pairs.end()) {
            for (int v : {pit->second.first, pit->second.second})
                havoc(cur, out.vars[static_cast<std::size_t>(v)].width, false, false, v);
        } else if (in.ty.kind == ir::Type::Ptr) {
            int dst = result_var(fr, in);
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {opaque_ptr(cur, "returned by @" + n, line)});
        } else if (in.ty.kind == ir::Type::Int) {
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
        } else if (in.ty.kind != ir::Type::Void) {
            throw Unenc{"UNENCODED: call @" + n + " (result of type " + in.ty.text + ")"};
        }
    }
    note_assumption("unknown-call: @" + n + " has no spec (--assume-unknown-calls): its result is arbitrary, the " +
                    std::to_string(nptr) +
                    " object(s) its pointer argument(s) point into are havocked, and it is assumed to return, to free nothing "
                    "and to change no other memory");
}
