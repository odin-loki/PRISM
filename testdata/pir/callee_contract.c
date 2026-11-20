/* --callee-contracts (docs/HARNESS_PHASE2.md): assume-guarantee stubs for callees without a body.
 * callee_contract.tsv gives the contract of each hw_* function below. Without the option (or without
 * --assume-unknown-calls) every call here is UNENCODED (NEEDS-HARNESS); a callee without a row, or whose
 * row does not fit the call, is treated as before. */

int hw_count(void);                           /* ensures 0 <= \result <= 16 */
int hw_read(int reg);                         /* ensures 0 <= \result <= 255 */
void hw_fill(char *buf, unsigned long n);     /* requires \valid_bytes(buf, n); assigns \bytes(buf, n) */
int hw_log(const char *s);                    /* assigns \nothing */
char *hw_alloc(unsigned long n);              /* ensures \result == \null || \valid_bytes(\result, n) */
int hw_false(void);                           /* ensures \false: a FALSE ensures (soundness test) */
int hw_nocontract(int x);                     /* no row in the file */
int hw_badsig(int x);                         /* a row whose signature does not match (u64 vs int) */
int hw_defined(int x) { return x; }           /* defined here: a row for it must be ignored */

/* the result is in [0, 16]: r + 1 is never 0 */
int stub_ensures_ok(void) {
    int r = hw_count();
    return 100 / (r + 1);
}

/* ... but r can be 0: a bug behind a stubbed call, found because the stub is faithful */
int stub_ensures_bad(void) { return 100 / hw_count(); }

/* the planted bug: an index that the contract's range does not keep inside the array */
int planted_oob(void) {
    int a[4] = {1, 2, 3, 4};
    int r = hw_count();
    return a[r];
}
int planted_oob_ok(void) {
    int a[17] = {0};
    int r = hw_count();
    return a[r];
}

/* requires: the buffer is valid for n bytes */
int stub_requires_ok(void) {
    char b[8];
    hw_fill(b, 8);
    return b[0] & 1;
}
int stub_requires_bad(void) { /* 4 bytes, 8 required: a finding about the caller */
    char b[4];
    hw_fill(b, 8);
    return b[0] & 1;
}

/* assigns: only b[0..4) is written, b[7] keeps its value */
int stub_assigns_only_ok(void) {
    char b[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    hw_fill(b, 4);
    return 100 / b[7];
}
/* ... and b[0] is arbitrary after the call */
int stub_assigns_written_bad(void) {
    char b[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    hw_fill(b, 4);
    return 100 / b[0];
}
int stub_nothing_ok(void) { /* assigns \nothing: the buffer is unchanged */
    char b[4] = {1, 1, 1, 1};
    hw_log(b);
    return 100 / b[0];
}

int stub_alloc_ok(void) {
    char *p = hw_alloc(16);
    if (!p) return -1;
    return p[15] & 1;
}
int stub_alloc_null_bad(void) { /* NULL is allowed by the contract */
    char *p = hw_alloc(16);
    return p[0];
}
int stub_alloc_oob_bad(void) { /* 16 bytes are valid, not 17 */
    char *p = hw_alloc(16);
    if (!p) return -1;
    return p[16];
}

/* SOUNDNESS: the contract of hw_false says it never returns (a false ensures), so everything after the call is
 * dead and this "proves": PROVED-ASSUMING, never PROVED, and the verdict names the stub, its status and its ensures. */
int stub_false_ensures(void) {
    hw_false();
    return 100 / 0;
}

int stub_unspecified(void) { return hw_nocontract(1); }
int stub_badsig(void) { return hw_badsig(1); }

/* the contract of a function with a body is never used: hw_defined(0) is 0 */
int stub_defined_bad(void) { return 100 / hw_defined(0); }
