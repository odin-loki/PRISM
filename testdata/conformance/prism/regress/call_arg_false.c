// PRISM conformance task regress/call_arg_false.c: expected false (no-div0)
// regression: S6: arguments of an unmodelled call are evaluated and checked
int sink_f(int v) { return v & 1; }
int call_arg_false(int d) {
    return sink_f(100 / d);
}
