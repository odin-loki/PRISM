/* Binary search over a sorted array and insertion sort. The contract parts
 * with a quantifier (sorted input, every element before the answer is
 * smaller, the output is a sorted permutation) are stated by the harnesses
 * for arrays of up to SN elements (a size-bounded proof). */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
/* --- end prelude --- */

#define SN 4

/* Lower bound: the first index whose element is >= key, n if none. The
 * search halves [lo, hi) at most LB_STEPS times, enough for n < 2^LB_STEPS:
 * the cap is part of the reference implementation so that both stages close
 * the loop by unwinding. The uncapped `while (lo < hi)` form got no answer
 * within 300 s (docs/CONTRACTS.md, gap list). */
#define LB_STEPS 4
/*@ requires \valid_read(a + (0..n-1));
    requires 0 <= n <= 8;
    assigns \nothing;
    ensures 0 <= \result <= n;
*/
int lower_bound(const int *a, int n, int key) {
    int lo = 0, hi = n;
    for (int step = 0; step < LB_STEPS && lo < hi; step++) {
        int mid = lo + ((hi - lo) >> 1); /* never (lo + hi) / 2: that overflows */
        if (a[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Exact match: an index holding key, or -1. */
/*@ requires \valid_read(a + (0..n-1));
    requires 0 <= n <= 8;
    assigns \nothing;
    ensures -1 <= \result < n;
    ensures \result >= 0 ==> a[\result] == key;
*/
int bsearch_int(const int *a, int n, int key) {
    int i = lower_bound(a, n, key);
    return i < n && a[i] == key ? i : -1;
}

/*@ requires \valid(a + (0..n-1));
    requires 0 <= n <= SN;
    assigns a[0..n-1];
*/
void insertion_sort(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int x = a[i], j = i;
        while (j > 0 && a[j - 1] > x) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = x;
    }
}

/* Sorted symbolic input: lower_bound's answer splits the array at key, and
 * bsearch finds key exactly when it is present. */
void lower_bound_contract(int a0, int a1, int a2, int a3, int n, int key, int j) {
    int a[SN] = {a0, a1, a2, a3};
    if (n < 0 || n > SN) return;
    for (int i = 0; i + 1 < n; i++)
        if (a[i] > a[i + 1]) return; /* the precondition (sorted) */
    int r = lower_bound(a, n, key);
    assert(0 <= r && r <= n);
    if (0 <= j && j < r) assert(a[j] < key);
    if (r <= j && j < n) assert(a[j] >= key);
    int b = bsearch_int(a, n, key);
    if (0 <= j && j < n && a[j] == key) assert(b >= 0);
    if (b >= 0) assert(a[b] == key);
}

/* Output sorted and a permutation (every input value occurs as often). */
void insertion_sort_contract(int a0, int a1, int a2, int a3, int n, int v) {
    int a[SN] = {a0, a1, a2, a3}, in[SN] = {a0, a1, a2, a3};
    if (n < 0 || n > SN) return;
    insertion_sort(a, n);
    for (int i = 0; i + 1 < n; i++) assert(a[i] <= a[i + 1]);
    int c_in = 0, c_out = 0;
    for (int i = 0; i < n; i++) {
        c_in += in[i] == v;
        c_out += a[i] == v;
    }
    assert(c_in == c_out);
    for (int i = n; i < SN; i++) assert(a[i] == in[i]); /* untouched past n */
}

/* Planted twin: (lo + hi) / 2 midpoint, hi = n near INT_MAX overflows. */
/*@ requires \valid_read(a + (0..n-1));
    requires 0 <= n;
*/
int lower_bound_overflow_bad(const int *a, int n, int key) {
    int lo = 0, hi = n;
    if (n < 2147483000) return 0;
    lo = n - 2;
    int mid = (lo + hi) / 2;
    return a[mid] < key ? mid + 1 : mid;
}

/* Planted twin: "insertion sort leaves a[0] in place". */
void insertion_sort_bad(int a0, int a1) {
    int a[2] = {a0, a1};
    insertion_sort(a, 2);
    assert(a[0] == a0);
}
