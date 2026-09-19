/* POINTER copy. Honest requires give the buffer size; a harness proof is
 * PROVED-ASSUMING, never unconditional PROVED. */
void copy(char *dst, char *src, int n)
{
    // requires: n >= 0
    // requires: n < 8
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* Single-object pointer: p != 0 is an honest non-null, size-1 harness. */
int deref_ok(int *p)
{
    // requires: p != 0
    *p = 1;
    return *p;
}
