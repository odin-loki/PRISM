/* Folds over arrays: min, max (the index of the first extreme element) and
 * a sum that cannot overflow (byte elements into a long). */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
/* --- end prelude --- */

#define FN 4

/*@ requires \valid_read(a + (0..n-1));
    requires 1 <= n <= 16;
    assigns \nothing;
    ensures 0 <= \result < n;
*/
int argmin(const int *a, int n) {
    int m = 0;
    for (int i = 1; i < n; i++)
        if (a[i] < a[m]) m = i;
    return m;
}

/*@ requires \valid_read(a + (0..n-1));
    requires 1 <= n <= 16;
    assigns \nothing;
    ensures 0 <= \result < n;
*/
int argmax(const int *a, int n) {
    int m = 0;
    for (int i = 1; i < n; i++)
        if (a[i] > a[m]) m = i;
    return m;
}

/*@ requires \valid_read(a + (0..n-1));
    requires 0 <= n <= 16;
    assigns \nothing;
    ensures 0 <= \result <= 255L * n;
*/
long sum_u8(const unsigned char *a, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

/* every element is >= the minimum, and the minimum comes first */
void argmin_contract(int a0, int a1, int a2, int a3, int n, int j) {
    int a[FN] = {a0, a1, a2, a3};
    if (n < 1 || n > FN) return;
    int m = argmin(a, n);
    assert(0 <= m && m < n);
    if (0 <= j && j < n) assert(a[j] >= a[m]);
    if (0 <= j && j < m) assert(a[j] > a[m]);
}

void argmax_contract(int a0, int a1, int a2, int a3, int n, int j) {
    int a[FN] = {a0, a1, a2, a3};
    if (n < 1 || n > FN) return;
    int m = argmax(a, n);
    assert(0 <= m && m < n);
    if (0 <= j && j < n) assert(a[j] <= a[m]);
    if (0 <= j && j < m) assert(a[j] < a[m]);
}

void sum_u8_contract(int n) {
    unsigned char a[FN];
    for (int i = 0; i < FN; i++) a[i] = (unsigned char)__VERIFIER_nondet_int();
    if (n < 0 || n > FN) return;
    long s = sum_u8(a, n), t = 0;
    for (int i = 0; i < n; i++) t += a[i];
    assert(s == t && s <= 255L * n);
}

/* Planted twin: an int sum of ints overflows. */
/*@ requires \valid_read(a + (0..n-1));
    requires 0 <= n <= 4;
*/
int sum_int_bad(const int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

/* Planted twin of a contract: "the minimum is at index 0". */
void argmin_first_bad(int a0, int a1) {
    int a[2] = {a0, a1};
    assert(argmin(a, 2) == 0);
}
