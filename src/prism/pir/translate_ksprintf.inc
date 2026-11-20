// Linux kernel sprintf (--pir-kernel-sprintf with --kernel-api linux, docs/PIR.md "Kernel sprintf"). Included by
// translate.cpp after translate_rec.inc: a Tr member function.
//
// The kernel spec of sprintf (models/kernel/linux_misc.cpp) is a soft stop: the output length depends on the kernel's
// %p extensions. With a literal format, pirmem::plan_kernel_format gives a sound upper bound B on the characters the
// kernel's vsnprintf writes for it (every conversion at its longest, %p extensions by their fixed maximum); a %s argument
// without a precision adds its string length (strlen, which checks that it is a NUL-terminated string), or 8 when the
// pointer is to no object (the kernel prints "(null)" / "(efault)"). The call then writes an arbitrary length
// n <= B + sum of the string lengths of arbitrary characters, a NUL after them (each byte checked against the buffer),
// and returns n. A format with no bound is UNENCODED.

bool Tr::kernel_sprintf(Frame& fr, const ir::Inst& in, int& cur, int line) {
    if (!opt.kernel_sprintf || opt.kernel_api != "linux" || in.callee != "sprintf" || in.ops.size() < 2) return false;
    if (const auto* own = m.find("sprintf"); own && !own->is_model) return false;  // the unit's own sprintf is inlined
    auto fmt = mt.const_string(in.ops[1].v);
    if (!fmt) throw Unenc{"UNENCODED: kernel sprintf with a non-literal format"};
    auto plan = pirmem::plan_kernel_format(*fmt, 2);
    if (!plan.unencoded.empty()) throw Unenc{plan.unencoded};
    if (plan.nargs > in.ops.size())
        throw Unenc{"UNENCODED: kernel sprintf: the format has more conversions than the call has arguments"};
    std::vector<Arg> args;
    for (auto& o : in.ops) args.push_back(operand(fr, o, cur, line));  // undef / poison uses checked
    const unsigned pw = ptr_w();
    Arg bound = Arg::c(pw, plan.max_len);
    for (std::size_t i : plan.str_args) {
        if (in.ops[i].ty.kind != ir::Type::Ptr) throw Unenc{"UNENCODED: kernel sprintf %s argument is not a pointer"};
        if (in.ops[i].v.kind == ir::Value::Null) {  // a literal NULL: "(null)"
            bound = assign(cur, Op::Add, pw, {bound, Arg::c(pw, 8)});
            continue;
        }
        Arg p = args[i];
        p.width = pw;
        Arg no_obj = assign(cur, Op::Eq, 1, {assign(cur, Op::LShr, pw, {p, Arg::c(pw, obj_shift())}), Arg::c(pw, 0)}, "c");
        int sl = newvar(tmpname("kslen"), pw);
        int yes = newblock("ksprintf.null"), no = newblock("ksprintf.str"), join = newblock("ksprintf.join");
        Term br;
        br.kind = Term::Br;
        br.cond = no_obj;
        br.t = yes;
        br.f = no;
        out.blocks[static_cast<std::size_t>(cur)].term = br;
        Term j;
        j.kind = Term::Jmp;
        j.t = join;
        out.blocks[static_cast<std::size_t>(yes)].term = j;
        add_phi(join, sl, yes, Arg::c(pw, 8));  // "(null)" / "(efault)"
        int c = no;
        const std::string rn = tmpname("ksprintf.strlen");
        int rv = newvar(rn, pw);  // size_t
        fr.env[rn] = Arg::v(rv, pw);
        ir::Inst call;
        call.op = "call";
        call.callee = "strlen";
        call.result = rn;
        call.ty = ir::Type{ir::Type::Int, pw, "i" + std::to_string(pw), {}};
        call.ops = {in.ops[i]};
        if (!m.find("strlen")) throw Unenc{"UNENCODED: kernel sprintf %s needs the strlen model"};
        inline_call(fr, call, c, line);
        out.blocks[static_cast<std::size_t>(c)].term = j;
        add_phi(join, sl, c, Arg::v(rv, pw));
        cur = join;
        bound = assign(cur, Op::Add, pw, {bound, Arg::v(sl, pw)});
    }
    Arg buf = args[0];
    buf.width = pw;
    Arg len = havoc(cur, pw, false, false);
    assume(cur, assign(cur, Op::Ule, 1, {len, bound}, "c"));
    mt.write_havoc(cur, buf, len, line);
    int end = newvar(tmpname("end"), pw);
    const ir::Type ti8{ir::Type::Int, 8, "i8", {}};
    mt.gep(cur, end, buf, ti8, {len}, true, line);
    mt.store(cur, Arg::v(end, pw), Arg::c(8, 0), Arg::c(1, 1), ti8, 1, line);
    if (int dst = in.result.empty() ? -1 : result_var(fr, in); dst >= 0) {
        unsigned w = out.vars[static_cast<std::size_t>(dst)].width;
        emit_assign(cur, dst, w < pw ? Op::Trunc : Op::Copy, {len});
    }
    note_assumption("linux-sprintf: kernel sprintf (lib/vsprintf.c) with the format at line " + std::to_string(line) +
                    " writes at most " + std::to_string(plan.max_len) + " characters" +
                    (plan.str_args.empty() ? std::string() : " plus the lengths of its %s strings (8 for a pointer to no object)") +
                    " and a NUL, and returns the count written; the characters are arbitrary and the argument types are "
                    "not checked");
    return true;
}
