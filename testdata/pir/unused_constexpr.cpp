// PIR task: a unit whose only functions are unused constexpr ones lowers to
// no definitions; it is lowered again with every declaration emitted.
constexpr int unused_cexpr_bad(int a) { return a * a; }
constexpr int unused_cexpr_ok(int a) {
    if (a < -1000 || a > 1000) return 0;
    return a * a;
}
