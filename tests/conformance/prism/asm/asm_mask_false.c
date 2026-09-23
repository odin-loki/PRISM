/* PRISM conformance task asm/asm_mask_false.c: expected false (no-oob) */
int asm_mask_false(int x) {
    int table[4] = {1, 2, 3, 4};
    int r;
    // prism: asm ensures r >= 0 && r <= 7
    __asm__("movl %1, %0\n\tandl $7, %0" : "=r"(r) : "r"(x));
    return table[r]; /* r in 4..7 */
}
