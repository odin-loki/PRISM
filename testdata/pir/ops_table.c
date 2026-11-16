/* --ops-table (docs/PIR.md "Indirect calls through ops tables"): calls through a field
 * of an ops table; ops_table.tsv names the targets of each field. The unit takes the
 * address of 19 functions of type int (int), more than the default limit of 16 targets
 * for an indirect call, so without the option every call below is UNENCODED. With the
 * option only the 2 targets of the called field are tried. */

struct ops {
    int (*safe)(int);
    int (*divide)(int);
};

struct wide {
    int (*pad)(int);
    int (*f)(int);
};

int safe_target(int x) { return x & 0x7f; }
int div_target(int x) { return 100 / x; }

#define DECOY(n) static int decoy##n(int x) { return x + n; }
DECOY(0)
DECOY(1)
DECOY(2)
DECOY(3)
DECOY(4)
DECOY(5)
DECOY(6)
DECOY(7)
DECOY(8)
DECOY(9)
DECOY(10)
DECOY(11)
DECOY(12)
DECOY(13)
DECOY(14)
DECOY(15)
DECOY(16)
int (*const decoys[])(int) = {decoy0, decoy1, decoy2, decoy3, decoy4, decoy5, decoy6, decoy7, decoy8, decoy9, decoy10, decoy11, decoy12, decoy13, decoy14, decoy15, decoy16};

/* the safe field: no undefined behaviour */
int ops_safe_ok(int x) {
    struct ops t = {safe_target, div_target};
    return t.safe(x);
}

/* the dividing field, reached with x == 0 */
int ops_div_bad(int x) {
    struct ops t = {safe_target, div_target};
    return t.divide(x);
}

/* the dividing field, reached only when x != 0 */
int ops_div_ok(int x) {
    struct ops t = {safe_target, div_target};
    if (x == 0) return 0;
    return t.divide(x);
}

/* nine targets for one field: beyond --ops-table's default maximum of 8 */
int ops_wide_many(int x) {
    struct wide t = {safe_target, safe_target};
    return t.f(x);
}
