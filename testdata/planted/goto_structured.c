/* bmc goto model (docs/SVCOMP.md "goto"): forward jumps out of nested
 * blocks and loops, backward gotos that form a loop, and the unstructured
 * shapes that stay NEEDS-HARNESS. */

/* Forward out of a nested loop: the overflow is only on the skipped path. */
int goto_out_ok(int x) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (x > 100) goto done;
        }
    }
    x = x + 2000000000;
done:
    return x;
}

/* Same shape, the overflow is reachable after the label. */
int goto_out_bad(int x) {
    for (int i = 0; i < 4; i++) {
        if (x < -100) goto done;
    }
    return 0;
done:
    return x - 2147483600;
}

/* Backward goto forming a loop that closes: i ends at 5. */
int goto_loop_ok(int x) {
    int i = 0;
again:
    i++;
    if (i < 5) goto again;
    return 10 / (i - 4);
}

/* Same loop, i - 5 is zero at the exit. */
int goto_loop_bad(int x) {
    int i = 0;
again:
    i++;
    if (i < 5) goto again;
    return 10 / (i - 5);
}

/* Unbounded backward goto: closed by the k-induction step, never guessed. */
int goto_loop_open(int n) {
    int i = 0;
top:
    if (i >= n) goto out;
    i++;
    goto top;
out:
    return i;
}

/* Jump into a block: unstructured, NEEDS-HARNESS. */
int goto_into_block(int x) {
    if (x) goto in;
    {
        int y = 1;
    in:
        y = y + 1;
        x = y;
    }
    return x;
}

/* Jump past a declaration: y is indeterminate at the label. */
int goto_past_decl(int x) {
    if (x) goto l;
    int y = 1;
l:
    return 10 / y;
}

/* The division by zero is at the 20th pass: beyond unwind 8, and the
 * k-induction step reaches it, so the verdict is BOUNDED, never a proof. */
int goto_loop_deep(int x) {
    int i = 0;
L:
    i++;
    if (i == 20) return 10 / (i - 20);
    if (i < 100) goto L;
    return 0;
}

/* y is read uninitialised on the goto path. */
int goto_uninit(int x) {
    int y;
    if (x) goto l;
    y = 1;
l:
    return y;
}
