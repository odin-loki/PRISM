// Recursive calls (--pir-recursion, docs/PIR.md "Recursion"). Included by translate.cpp after translate_va.inc: these
// are Tr member functions.
//
// Without the option a call of a function already on the inlining stack is `UNENCODED: recursive call @g`. With it the
// call is not inlined again; instead
//   * its arguments are evaluated (undef and poison uses are checked) and, with --pir-discharge, the callee's harness
//     is checked at the call as for any call (call_obligations),
//   * the call is assumed to have no violation for these arguments. For the analysed function itself this is the
//     induction hypothesis of a proof by induction on the recursion depth: a violation in a terminating execution
//     happens at some finite depth, and the body is proved for every entry state, the states after recursive calls
//     included, so no depth has one. For another function g on the stack it is g's own verdict (its own row), which this
//     one then rests on. Either way it is recorded as a named assumption, with the entry assumptions of the callee
//     (its harness, kernel state) assumed to hold at the call,
//   * its effects are a havoc of all memory (every object's contents become arbitrary; sizes, liveness and kinds are
//     kept, so it is assumed to free nothing) and an arbitrary result (a pointer: NULL or a fresh object of
//     kUnknownPtrBytes arbitrary bytes), and it is assumed to return.
// A bounded unrolling would give BOUNDED, which is never a proof; this gives an unbounded claim under a named
// assumption (PROVED-ASSUMING at best).

void Tr::recursive_call(Frame& fr, const ir::Inst& in, int& cur, int line) {
    const auto& n = in.callee;
    for (auto& o : in.ops) (void)operand(fr, o, cur, line);  // arguments evaluated (undef / poison uses checked)
    if (!in.result.empty()) {
        if (auto pit = fr.pairs.find(in.result); pit != fr.pairs.end()) {
            mt.havoc_all(cur);
            for (int v : {pit->second.first, pit->second.second})
                havoc(cur, out.vars[static_cast<std::size_t>(v)].width, false, false, v);
        } else if (in.ty.kind == ir::Type::Ptr) {
            mt.havoc_all(cur);
            int dst = result_var(fr, in);
            if (dst >= 0) emit_assign(cur, dst, Op::Copy, {opaque_ptr(cur, "returned by the recursive call of @" + n, line)});
        } else if (in.ty.kind == ir::Type::Int || in.ty.kind == ir::Type::Float) {
            mt.havoc_all(cur);
            int dst = result_var(fr, in);
            if (dst >= 0) havoc(cur, out.vars[static_cast<std::size_t>(dst)].width, false, false, dst);
        } else if (in.ty.kind != ir::Type::Void) {
            throw Unenc{"UNENCODED: recursive call @" + n + " (result of type " + in.ty.text + ")"};
        } else {
            mt.havoc_all(cur);
        }
    } else {
        mt.havoc_all(cur);
    }
    const bool self = !stack.empty() && stack.front() == n;
    note_assumption(std::string("recursive-call: @") + n + " at line " + std::to_string(line) + " is not inlined again: " +
                    (self ? "it has no violation for its arguments (the induction hypothesis on the recursion depth; "
                            "the analysed function's entry assumptions are assumed to hold at the call)"
                          : "it has no violation for its arguments (@" + n + "'s own verdict, with its entry assumptions "
                            "assumed to hold at the call)") +
                    "; every object's contents become arbitrary, the result is arbitrary, and it is assumed to return "
                    "and to free nothing");
}
