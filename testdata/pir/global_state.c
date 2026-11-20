/* PIR tasks: a violation that needs arbitrary values of mutable globals (docs/PIR.md "Global state").
 * Mutable globals hold arbitrary bytes at entry; with the globals' initial values (inputs still arbitrary) a violation
 * that does not occur is NEEDS-HARNESS ("violation needs mutable-global values"), one that still occurs stays FAILED. */

int g_base = 0;   /* mutable, initially 0 */
int g_idx = 1;    /* mutable, initially 1 */
int g_k = 7;      /* mutable, initially 7 */
int tab[4];
int *g_ptr;       /* mutable pointer, initially NULL */

/* only g_base == INT_MAX overflows: the violation needs a value the initializer does not give */
int glob_ovf(void) { return g_base + 1; }

/* only g_idx outside 0..3 is out of bounds */
int glob_oob(void) { return tab[g_idx]; }

/* two globals: both are named */
int glob_two(void) { return g_base + g_idx + 1; }

/* the pointer is NULL in its initial state too: the violation stays */
int glob_null(void) { return *g_ptr; }

/* the input alone makes it fail, whatever the globals hold (a different check than the global one may be reported) */
int in_div(int x) { return g_k / x; }
int in_ovf(int x) { return x + 1; }

/* a global-dependent overflow and an input-dependent division in one function: the division is a violation from
 * inputs alone, so the function is never NEEDS-HARNESS */
int two_sites(int x) { int a = g_base + 2147483000; return a + 100 / x; }

/* no violation at all: never turned into anything else */
int glob_ok(int x) { return (x & 3) + (g_idx & 1); }
