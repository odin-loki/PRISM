// PRISM conformance task regress/call_arg_true.c: expected true (no-div0)
// regression: S6: arguments of an unmodelled call are evaluated and checked
int sink_t(int v) { return v & 1; }
int call_arg_true(int d) {
    if (d == 0) return 0;
    return sink_t(100 / d);
}
