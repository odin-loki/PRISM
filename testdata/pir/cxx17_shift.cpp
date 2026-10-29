// PIR task: signed left shift in a C++ unit without -std= is checked by the
// rules of gnu++17 (the default of the C++ compilers PRISM runs, as in bmc):
// shifting a set bit past the sign bit is UB before C++20.
int shl17_bad(int a) {
    if (a < 0) return 0;
    return a << 4;
}
int shl17_sign_ok(int s) {
    if (s < 0 || s > 31) return 0;
    return 1 << s;  // into the sign bit: defined since C++11
}
