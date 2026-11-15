/* --ops-table (docs/PIR.md "Indirect calls through ops tables"): calls through the
 * fields of a const ops table. ops_table.tsv names the targets of each field.
 * Without the option every call through a field stays UNENCODED (havoc fallback). */

struct ops {
    int (*safe)(int);
    int (*divide)(int);
};

struct wide {
    int (*f)(int);
};

static int safe_target(int x) { return x & 0x7f; }
static int div_target(int x) { return 100 / x; }

const struct ops the_ops = {safe_target, div_target};
const struct wide the_wide = {safe_target};

/* the safe target: no undefined behaviour */
int ops_safe_ok(int x) { return the_ops.safe(x); }

/* the dividing target, reached through the pointer with x == 0 */
int ops_div_bad(int x) { return the_ops.divide(x); }

/* the dividing target, reached only when x != 0 */
int ops_div_ok(int x) {
    if (x == 0) return 0;
    return the_ops.divide(x);
}

/* nine targets for one field: beyond --ops-table's default maximum of 8 */
int ops_wide_many(int x) { return the_wide.f(x); }
