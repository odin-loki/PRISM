/* KLEE Executor::fork plant: then-branch is a conjunction extreme/interesting
 * seeds miss (none of 0,1,-1,4,31,32,INT_MAX is in (8,12)). Z3 on the
 * negated predicate should recover x=10; the concrete oracle then hits
 * INT-DIV-ZERO. Comparisons short-circuit so INT_MAX is not a signed
 * overflow in the guard. CLEAN without Z3 is not a proof. */
int klee_fork_neg(int x) {
    if (x > 8 && x < 12 && x % 17 == 10) {
        return 1 / (x - 10);
    }
    return x;
}

/* Then-branch is unsat (x cannot be both 4 and 5). Executor::fork drops
 * that side. A full budget with no UB is CLEAN, not a proof. */
int klee_fork_unsat(int x) {
    if (x == 4 && x == 5) {
        return 1 / x;
    }
    return x;
}
