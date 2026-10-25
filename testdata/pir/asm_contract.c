/* PIR tasks: inline assembly (docs/PIR.md "Inline assembly"). Without a
 * contract an asm block is NEEDS-HARNESS; `// prism: asm ensures <cond>`
 * states its effect: PRISM assumes it (the block writes only its output)
 * and a proof is PROVED-ASSUMING with the contract listed. */

int asm_nocontract(int x) {
    int r;
    __asm__("movl %1, %0" : "=r"(r) : "r"(x));
    return r;
}

int asm_ok(int x) {
    int table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int r;
    // prism: asm ensures r >= 0 && r <= 7
    __asm__("movl %1, %0\n\tandl $7, %0" : "=r"(r) : "r"(x));
    return table[r];
}

int asm_bad(int x) {
    int table[4] = {1, 2, 3, 4};
    int r;
    // prism: asm ensures r >= 0 && r <= 7
    __asm__("movl %1, %0\n\tandl $7, %0" : "=r"(r) : "r"(x));
    return table[r]; /* r in 4..7 reads past the end */
}
