/* ACSL memory / logic predicates Frama-C WP encodes in a heap model.
   PRISM has no such model: ERROR, never a proof. */

int wp_valid_bad(int x) {
    // requires: \valid(&x)
    // ensures: result == x
    return x;
}

/* \old snapshots the pre-state. Unencoded: ERROR. */
int wp_old_bad(int x) {
    // requires: x < 100
    // ensures: result == \old(x) + 1
    return x + 1;
}

/* Quantifiers are VCs Frama-C sends to a prover with a logic type.
   PRISM does not decide \forall: ERROR. */
int wp_forall_bad(int x) {
    // requires: \forall integer k; k == x
    // ensures: result == x
    return x;
}

/* Block ACSL with \valid is the same unencodable predicate as // requires:. */
/*@ requires \valid(&x);
    ensures \result == x;
 */
int wp_acsl_valid_bad(int x) {
    return x;
}

/* Member access is not a scalar WP atom. `->` must not QED-prove as `-` `>`. */
int wp_arrow_bad(int x) {
    // ensures: p->x == p->x
    return x;
}
