/* requires n > 0 */
int precond_bad(int n) {
    return n;
}

/* requires n > 0 */
int precond_ok(int n) {
    if (n <= 0)
        return 0;
    return n;
}
