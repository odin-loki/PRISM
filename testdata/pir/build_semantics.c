/* PIR tasks: --pir-build-semantics (docs/USER_GUIDE.md). The compile command has -fno-strict-overflow (Linux) or
 * -fwrapv (FreeBSD): signed + - * and signed << wrap in that build, so they are not undefined behaviour there.
 * Shift amounts and division stay undefined behaviour in every build. */

int wrap_add(int a, int b) { return a + b; }
int wrap_mul(int a) { return a * 3; }
int wrap_sub(int a, int b) { return a - b; }
int wrap_shl_sign(int a) { return a << 4; }
int folded_ovf(void) { return 2147483647 + 1; }
int folded_shl(void) { return 1 << 31; }

/* undefined in the -fwrapv build too */
int shift_amount_bad(int a, int s) { return a << s; }
int div_bad(int a, int b) { return a / b; }
int min_div_bad(int a) { return a / -1; }
