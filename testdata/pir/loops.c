/* PIR tasks: bounded unrolling and the unwinding assertion. */
int count4_ok(int x) {
    int s = 0;
    for (int i = 0; i < 4; i++)
        s += i;
    return s + (x & 1);
}
int loop_ovf_bad(int x) {
    for (int i = 0; i < 4; i++)
        x = x + x;
    return x;
}
/* s == i at the loop head: closes with a loop invariant (docs/PIR.md "Loop invariants") */
int loop_long_inv(int n) {
    int s = 0;
    for (int i = 0; i < 100; i++)
        s += 1;
    return s + (n & 1);
}
/* s == i * (i - 1) / 2 is not a template: stays BOUNDED (Law 2) */
int loop_long_bounded(int n) {
    int s = 0;
    for (int i = 0; i < 100; i++)
        s += i;
    return s + (n & 1);
}
int nested_ok(int n) {
    int c = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            c++;
    return c + (n & 1);
}
