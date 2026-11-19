/* A declaration with the name of a library contract but another signature
 * is another function: the contract does not answer it, the call stays
 * NEEDS-HARNESS (UNENCODED: call @__ilog2_u32 ... does not match). */
long __ilog2_u32(long n);

int wrong_signature(long n) { return (int)__ilog2_u32(n); }
